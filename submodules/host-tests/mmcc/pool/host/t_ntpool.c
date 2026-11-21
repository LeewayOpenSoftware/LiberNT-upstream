/*
 * PROJECT:     ReactOS host-native tests
 * FILE:        submodules/host-tests/mmcc/pool/host/t_ntpool.c
 * PURPOSE:     NT pool allocator host-native regression tests
 *
 * SPDX-FileCopyrightText: 2026 Ahmed ARIF
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include <mm/pool/ex/expool.h>

#define ARENA_BYTES (128 * 1024 * 1024)
#define TAG_TEST 0x74507845

_Thread_local ULONG PoolHostProcessor;
_Thread_local volatile int *PoolHostLockBlocked;
ULONG KeNumberProcessors = 1;
int MiSystem;
static EPROCESS SystemProcess, TestProcess;
PEPROCESS PsInitialSystemProcess = &SystemProcess;
static PUCHAR Arena, Protection;
#define SESSION_ARENA_BYTES (32 * 1024 * 1024)
static PUCHAR SessionArena, SessionProtection;
static PVOID CurrentSessionPool;
static int SessionSpace;
static ULONG Failures, Checks;
extern POOL_KM_LAYOUT MiPoolLayout;

#define CHECK(e) do { Checks++; if (!(e)) { Failures++; \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); } } while (0)

ULONG64 KeQueryInterruptTime(VOID) { return 1; }
KIRQL KeGetCurrentIrql(VOID) { return 0; }
_Noreturn VOID KeBugCheckEx(ULONG Code, ULONG_PTR A, ULONG_PTR B, ULONG_PTR C, ULONG_PTR D)
{ fprintf(stderr, "bugcheck %x %zx %zx %zx %zx\n", Code, (size_t)A, (size_t)B, (size_t)C, (size_t)D); abort(); }
_Noreturn VOID ExRaiseStatus(NTSTATUS Status) { fprintf(stderr, "raise %x\n", Status); abort(); }
BOOLEAN MmUseSpecialPool(SIZE_T Size, ULONG Tag) { return FALSE; }
PVOID MmAllocateSpecialPool(SIZE_T Size, ULONG Tag, POOL_TYPE Type, ULONG Mode) { return NULL; }
BOOLEAN MmIsSpecialPoolAddress(PVOID Block) { return FALSE; }
VOID MmFreeSpecialPool(PVOID Block) { abort(); }
PEPROCESS PsGetCurrentProcess(VOID) { return &TestProcess; }
VOID ObReferenceObject(PEPROCESS Process) { Process->References++; }
VOID ObDereferenceObject(PEPROCESS Process) { Process->References--; }
VOID MiWakeBalanceSetManager(VOID) { }
PVOID MiSessionPoolOfCurrentProcess(VOID) { return CurrentSessionPool; }
BOOLEAN MmIsSessionAddress(PVOID Address)
{
    return SessionArena != NULL && (PUCHAR)Address >= SessionArena && (PUCHAR)Address < SessionArena + SESSION_ARENA_BYTES;
}

NTSTATUS MiSpaceCommitPinned(int *System, PVOID Space, ULONG64 Base, ULONG Pages, ULONG Protect)
{
    SIZE_T Offset = (SIZE_T)(Base - (ULONG64)(ULONG_PTR)SessionArena);
    CHECK(Space == &SessionSpace);
    CHECK(Offset < SESSION_ARENA_BYTES && Pages <= (SESSION_ARENA_BYTES - Offset) / PAGE_SIZE);
    if (Offset >= SESSION_ARENA_BYTES || Pages > (SESSION_ARENA_BYTES - Offset) / PAGE_SIZE)
        return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < Pages; i++)
    {
        CHECK(SessionProtection[(Offset >> PAGE_SHIFT) + i] == 0);
        SessionProtection[(Offset >> PAGE_SHIFT) + i] = (UCHAR)Protect;
    }
    return STATUS_SUCCESS;
}

VOID MiSpaceDecommitPinned(int *System, PVOID Space, ULONG64 Base, ULONG Pages)
{
    SIZE_T First = ((ULONG_PTR)Base - (ULONG_PTR)SessionArena) >> PAGE_SHIFT;
    CHECK(Space == &SessionSpace);
    for (ULONG i = 0; i < Pages; i++)
    {
        CHECK(SessionProtection[First + i] != 0);
        SessionProtection[First + i] = 0;
    }
}

static VOID SessionPoolTests(VOID)
{
    const SIZE_T Sizes[] = { 24, 300, PAGE_SIZE, PAGE_SIZE * 9, POOL_UNIT_SIZE + 64 };
    SIZE_T PagedBytes, NonPagedBytes;
    ULONG Allocations, i;
    POOL_BLOCK_INFO Info;
    PVOID Pool = NULL, Kept[3];

    CHECK(posix_memalign((void **)&SessionArena, POOL_UNIT_SIZE, SESSION_ARENA_BYTES) == 0);
    SessionProtection = calloc(SESSION_ARENA_BYTES / PAGE_SIZE, 1);
    CHECK(SessionArena != NULL && SessionProtection != NULL);
    if (SessionArena == NULL || SessionProtection == NULL)
        return;

    Kept[0] = ExAllocatePoolWithTag(PagedPoolSession, 64, TAG_TEST);
    CHECK(Kept[0] != NULL && !MmIsSessionAddress(Kept[0]));
    ExFreePoolWithTag(Kept[0], TAG_TEST);

    CHECK(ExpSessionPoolCreate(&SessionSpace, 7, (ULONG64)(ULONG_PTR)SessionArena, 16 * 1024 * 1024,
                               (ULONG64)(ULONG_PTR)SessionArena + 16 * 1024 * 1024, 16 * 1024 * 1024,
                               &Pool) == STATUS_SUCCESS);
    CHECK(Pool != NULL);
    if (Pool == NULL)
        return;
    CurrentSessionPool = Pool;
    for (i = 0; i < RTL_NUMBER_OF(Sizes); i++)
    {
        PVOID Paged = ExAllocatePoolWithTag(PagedPoolSession, Sizes[i], TAG_TEST);
        PVOID NonPaged = ExAllocatePoolWithTag(NonPagedPoolSessionNx, Sizes[i], TAG_TEST);
        PVOID Zeroed = ExAllocatePool2(POOL_FLAG_SESSION | POOL_FLAG_PAGED, Sizes[i], TAG_TEST);
        PVOID Global = ExAllocatePoolWithTag(PagedPool, Sizes[i], TAG_TEST);

        CHECK(Paged != NULL && (PUCHAR)Paged >= SessionArena && (PUCHAR)Paged < SessionArena + 16 * 1024 * 1024);
        CHECK(NonPaged != NULL && (PUCHAR)NonPaged >= SessionArena + 16 * 1024 * 1024 &&
              (PUCHAR)NonPaged < SessionArena + SESSION_ARENA_BYTES);
        CHECK(Zeroed != NULL && MmIsSessionAddress(Zeroed));
        CHECK(Global != NULL && !MmIsSessionAddress(Global));
        if (Paged == NULL || NonPaged == NULL || Zeroed == NULL || Global == NULL)
            continue;
        for (SIZE_T j = 0; j < Sizes[i]; j++)
            CHECK(((PUCHAR)Zeroed)[j] == 0);
        CHECK(ExpSessionPoolQuery(Paged, &Info) && Info.Size >= Sizes[i] && Info.Tag == TAG_TEST);
        CHECK(!ExpSessionPoolQuery(Global, &Info));
        memset(Paged, 0xCD, Sizes[i]);
        memset(NonPaged, 0xCD, Sizes[i]);
        ExpSessionPoolQueryUsage(Pool, &PagedBytes, &NonPagedBytes, &Allocations);
        CHECK(Allocations == ((1u << 16) | 2u) && PagedBytes >= 2 * Sizes[i] && NonPagedBytes >= Sizes[i]);
        ExFreePoolWithTag(Paged, TAG_TEST);
        ExFreePoolWithTag(NonPaged, TAG_TEST);
        ExFreePoolWithTag(Zeroed, TAG_TEST);
        ExFreePoolWithTag(Global, TAG_TEST);
        ExpSessionPoolQueryUsage(Pool, &PagedBytes, &NonPagedBytes, &Allocations);
        CHECK(Allocations == 0 && PagedBytes == 0 && NonPagedBytes == 0);
    }

    Kept[0] = ExAllocatePoolWithTag(PagedPoolSession, 100, TAG_TEST);
    Kept[1] = ExAllocatePoolWithTag(PagedPoolSession, PAGE_SIZE * 3, TAG_TEST);
    Kept[2] = ExAllocatePoolWithTag(NonPagedPoolSession, 40, TAG_TEST);
    CHECK(Kept[0] != NULL && Kept[1] != NULL && Kept[2] != NULL);
    ExpSessionPoolQueryUsage(Pool, &PagedBytes, &NonPagedBytes, &Allocations);
    CHECK(Allocations == ((1u << 16) | 2u) && PagedBytes >= 100 + PAGE_SIZE * 3 && NonPagedBytes >= 40);
    CurrentSessionPool = NULL;
    ExpSessionPoolDestroy(Pool);

    free(SessionProtection);
    free(SessionArena);
    SessionArena = NULL;
}
NTSTATUS MiWaitForMemory(NTSTATUS Status, PULONG Attempts) { return Status; }

NTSTATUS PsChargeProcessPoolQuota(PEPROCESS Process, ULONG Type, SIZE_T Size)
{
    CHECK(Type < 2);
    if (Type >= 2) return STATUS_INVALID_PARAMETER;
    Process->Quota[Type] += Size;
    return STATUS_SUCCESS;
}

VOID PsReturnPoolQuota(PEPROCESS Process, ULONG Type, SIZE_T Size)
{
    CHECK(Type < 2);
    if (Type >= 2) return;
    CHECK(Process->Quota[Type] >= Size);
    Process->Quota[Type] -= Size;
}

NTSTATUS MiSystemCommitPinned(int *System, ULONG64 Base, ULONG Pages, ULONG Protect)
{
    SIZE_T Offset = (SIZE_T)(Base - (ULONG64)(ULONG_PTR)Arena);
    CHECK(Offset < ARENA_BYTES && Pages <= (ARENA_BYTES - Offset) / PAGE_SIZE);
    if (Offset >= ARENA_BYTES || Pages > (ARENA_BYTES - Offset) / PAGE_SIZE)
        return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < Pages; i++)
    {
        CHECK(Protection[(Offset >> PAGE_SHIFT) + i] == 0);
        Protection[(Offset >> PAGE_SHIFT) + i] = (UCHAR)Protect;
    }
    return STATUS_SUCCESS;
}

VOID MiSystemDecommitPinned(int *System, ULONG64 Base, ULONG Pages)
{
    SIZE_T First = ((ULONG_PTR)Base - (ULONG_PTR)Arena) >> PAGE_SHIFT;
    for (ULONG i = 0; i < Pages; i++)
    {
        CHECK(Protection[First + i] != 0);
        Protection[First + i] = 0;
    }
}

static VOID CheckAllocation(PVOID Block, SIZE_T Size, BOOLEAN Execute, BOOLEAN Zero)
{
    CHECK(Block != NULL);
    if (Block == NULL) return;
    CHECK(((ULONG_PTR)Block & (Size >= PAGE_SIZE ? PAGE_SIZE - 1 : 15)) == 0);
    for (SIZE_T i = 0; i < Size; i += PAGE_SIZE)
        CHECK(Protection[((PUCHAR)Block + i - Arena) >> PAGE_SHIFT] ==
              (Execute ? MI_PROT_EXECUTE_READWRITE : MI_PROT_READWRITE));
    if (Zero)
        for (SIZE_T i = 0; i < Size; i++) CHECK(((PUCHAR)Block)[i] == 0);
    memset(Block, 0xAB, Size);
    ExFreePoolWithTag(Block, TAG_TEST);
}

int main(void)
{
    const SIZE_T Sizes[] = { 32, 160, PAGE_SIZE, PAGE_SIZE * 17, POOL_UNIT_SIZE * 2 };
    POOL_TAG_USAGE Usage;
    CHECK(posix_memalign((void **)&Arena, POOL_UNIT_SIZE, ARENA_BYTES) == 0);
    Protection = calloc(ARENA_BYTES / PAGE_SIZE, 1);
    CHECK(Arena != NULL && Protection != NULL);
    if (Arena == NULL || Protection == NULL)
    {
        free(Protection);
        free(Arena);
        return 1;
    }
    memset(Arena, 0, ARENA_BYTES);
    memset(Protection, MI_PROT_READWRITE, (4 * 1024 * 1024) / PAGE_SIZE);
    MiPoolLayout.NonPagedResidentBase = Arena;
    MiPoolLayout.NonPagedResidentBytes = 4 * 1024 * 1024;
    MiPoolLayout.NonPagedSharedBase = Arena + MiPoolLayout.NonPagedResidentBytes;
    MiPoolLayout.NonPagedSharedBytes = 60 * 1024 * 1024;
    MiPoolLayout.PagedBase = Arena + 64 * 1024 * 1024;
    MiPoolLayout.PagedBytes = 64 * 1024 * 1024;
    InitializePool(NonPagedPool, 0);
    InitializePool(PagedPool, 0);
    for (ULONG j = 0; j < 20; j++)
    {
        SIZE_T Size = Sizes[j % RTL_NUMBER_OF(Sizes)];
        CheckAllocation(ExAllocatePoolWithTag(NonPagedPoolExecute, Size, TAG_TEST), Size, TRUE, FALSE);
        CheckAllocation(ExAllocatePoolWithTag(NonPagedPoolNx, Size, TAG_TEST), Size, FALSE, FALSE);
        CheckAllocation(ExAllocatePool2(POOL_FLAG_NON_PAGED_EXECUTE, Size, TAG_TEST), Size, TRUE, TRUE);
        CheckAllocation(ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, TAG_TEST), Size, FALSE, TRUE);
        CheckAllocation(ExAllocatePool2(POOL_FLAG_NON_PAGED | POOL_FLAG_USE_QUOTA, Size, TAG_TEST), Size, FALSE, TRUE);
        CheckAllocation(ExAllocatePool2(POOL_FLAG_NON_PAGED_EXECUTE | POOL_FLAG_USE_QUOTA, Size, TAG_TEST), Size, TRUE, TRUE);
        CheckAllocation(ExAllocatePoolWithQuotaTag(NonPagedPool, Size, TAG_TEST), Size, TRUE, FALSE);
        CheckAllocation(ExAllocatePool2(POOL_FLAG_PAGED | POOL_FLAG_USE_QUOTA, Size, TAG_TEST), Size, FALSE, TRUE);
        CheckAllocation(ExAllocatePool3(POOL_FLAG_NON_PAGED_EXECUTE, Size, TAG_TEST, NULL, 0), Size, TRUE, TRUE);
        CHECK(TestProcess.References == 0);
        CHECK(TestProcess.Quota[0] == 0 && TestProcess.Quota[1] == 0);
    }
    CHECK(PoolTagQuery(&ExpPoolState.Tracker, TAG_TEST, &Usage));
    CHECK(Usage.Allocations[0] == 160 && Usage.Allocations[1] == 20);
    CHECK(Usage.Allocations[0] == Usage.Frees[0] && Usage.Allocations[1] == Usage.Frees[1]);
    CHECK(Usage.Bytes[0] == 0 && Usage.Bytes[1] == 0);
    for (ULONG i = 0; i < RTL_NUMBER_OF(Sizes); i++)
    {
        PVOID Block = ExAllocatePoolWithQuotaTag(NonPagedPoolNx, Sizes[i], TAG_TEST);
        POOL_BLOCK_INFO Info;
        PPOOL_HEAP Heap;
        ULONG HeapIndex;

        CHECK(Block != NULL);
        if (Block == NULL) continue;
        Heap = ExpPoolHeapFromAddress(Block, &HeapIndex);
        if (Heap == NULL || !PoolHeapQuery(Heap, Block, &Info))
        {
            CHECK(FALSE);
            ExFreePoolWithTag(Block, TAG_TEST);
            continue;
        }
        CHECK((Info.Flags & EX_POOL_BLOCK_QUOTA) != 0 && Info.Trailer == (ULONG_PTR)&TestProcess);
        CHECK(TestProcess.References == 1 && TestProcess.Quota[0] == Info.Size);
        ExReturnPoolQuota(Block);
        CHECK(TestProcess.References == 0 && TestProcess.Quota[0] == 0);
        CHECK(PoolHeapQuery(Heap, Block, &Info));
        CHECK((Info.Flags & EX_POOL_BLOCK_QUOTA) == 0 && Info.Trailer == 0);
        ExReturnPoolQuota(Block);
        ExFreePoolWithTag(Block, TAG_TEST);
        CHECK(TestProcess.References == 0 && TestProcess.Quota[0] == 0);
    }
    SessionPoolTests();
    free(Protection);
    free(Arena);
    printf("NT pool: %u checks, %u failures\n", Checks, Failures);
    return Failures != 0;
}
