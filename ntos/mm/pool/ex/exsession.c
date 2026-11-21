/*
 * PROJECT:     LiberNT Pool Allocator
 * LICENSE:     GPL-3.0-only (https://spdx.org/licenses/GPL-3.0-only)
 * PURPOSE:     Session pool
 * COPYRIGHT:   Copyright 2026 mikuta0407 <mikuta0407@mikuta0407.net>
 */

#include "expool.h"

#define NDEBUG
#if !defined(POOL_HOST_TEST)
#include <debug.h>
#endif

#define EX_SESSION_POOL_NONPAGED 0
#define EX_SESSION_POOL_PAGED    1
#define EX_SESSION_POOL_COUNT    2

typedef struct _EX_SESSION_POOL
{
    PPOOL_HEAP Heap[EX_SESSION_POOL_COUNT];
    POOL_VA_REGION Region[EX_SESSION_POOL_COUNT];
    POOL_SESSION_BACKING Backing[EX_SESSION_POOL_COUNT];
    volatile LONG64 Bytes[EX_SESSION_POOL_COUNT];
    volatile LONG Allocations[EX_SESSION_POOL_COUNT];
    ULONG SessionId;
} EX_SESSION_POOL, *PEX_SESSION_POOL;

NTSTATUS
ExpSessionPoolCreate(
    _In_ PVOID Space,
    _In_ ULONG SessionId,
    _In_ ULONG64 PagedBase,
    _In_ SIZE_T PagedBytes,
    _In_ ULONG64 NonPagedBase,
    _In_ SIZE_T NonPagedBytes,
    _Out_ PVOID *SessionPool)
{
    PEX_SESSION_POOL Pool;
    POOL_BACKING Backing;
    ULONG i;
    NTSTATUS Status;

    *SessionPool = NULL;
    Pool = ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(*Pool), 'pSmM');
    if (Pool == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Pool, sizeof(*Pool));
    Pool->SessionId = SessionId;

    for (i = 0; i < EX_SESSION_POOL_COUNT; i++)
    {
        BOOLEAN Paged = (BOOLEAN)(i == EX_SESSION_POOL_PAGED);

        Pool->Backing[i].Space = Space;
        PoolBackSessionBacking(&Backing, &Pool->Backing[i]);
        Status = PoolVaRegionInitialize(&Pool->Region[i], (PVOID)(ULONG_PTR)(Paged ? PagedBase : NonPagedBase),
                                        Paged ? PagedBytes : NonPagedBytes, Paged, &Backing);
        if (NT_SUCCESS(Status))
            Status = ExpPoolCreateHeap(&Pool->Heap[i], &Pool->Region[i],
                                       Paged ? EX_POOL_HEAP_PAGED : EX_POOL_HEAP_NONPAGED);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Pool, 'pSmM');
            return Status;
        }
    }

    *SessionPool = Pool;
    return STATUS_SUCCESS;
}

VOID
ExpSessionPoolQueryUsage(
    _In_ PVOID SessionPool,
    _Out_ PSIZE_T PagedBytes,
    _Out_ PSIZE_T NonPagedBytes,
    _Out_ PULONG Allocations)
{
    PEX_SESSION_POOL Pool = SessionPool;
    ULONG Paged = (ULONG)POOL_LOAD_ACQUIRE32(&Pool->Allocations[EX_SESSION_POOL_PAGED]);
    ULONG NonPaged = (ULONG)POOL_LOAD_ACQUIRE32(&Pool->Allocations[EX_SESSION_POOL_NONPAGED]);

    *PagedBytes = (SIZE_T)Pool->Bytes[EX_SESSION_POOL_PAGED];
    *NonPagedBytes = (SIZE_T)Pool->Bytes[EX_SESSION_POOL_NONPAGED];
    *Allocations = (NonPaged << 16) | (Paged & 0xFFFF);
}

VOID
ExpSessionPoolDestroy(
    _In_ PVOID SessionPool)
{
    PEX_SESSION_POOL Pool = SessionPool;
    SIZE_T PagedBytes, NonPagedBytes;
    ULONG Allocations;

    POOL_ASSERT(Pool != NULL);
    ExpSessionPoolQueryUsage(Pool, &PagedBytes, &NonPagedBytes, &Allocations);
    if (Allocations != 0)
    {
        DPRINT1("Session %lu unloaded with pool still allocated: %Iu paged bytes, %Iu nonpaged bytes, "
                "allocations 0x%08lx\n", Pool->SessionId, PagedBytes, NonPagedBytes, Allocations);
    }
    ExFreePoolWithTag(Pool, 'pSmM');
}

static
PEX_SESSION_POOL
ExpSessionPoolOfAddress(
    _In_ PVOID Block,
    _Out_ PULONG Index)
{
    PEX_SESSION_POOL Pool = MiSessionPoolOfCurrentProcess();
    ULONG i;

    if (Pool == NULL)
        return NULL;
    for (i = 0; i < EX_SESSION_POOL_COUNT; i++)
    {
        if (Pool->Heap[i] != NULL && PoolHeapContains(Pool->Heap[i], Block))
        {
            *Index = i;
            return Pool;
        }
    }
    return NULL;
}

PVOID
ExpSessionPoolAllocate(
    _In_ ULONG HeapIndex,
    _In_ SIZE_T NumberOfBytes,
    _In_ ULONG Tag,
    _In_ ULONG HeapFlags)
{
    PEX_SESSION_POOL Pool = MiSessionPoolOfCurrentProcess();
    ULONG Index = (HeapIndex == EX_POOL_HEAP_PAGED) ? EX_SESSION_POOL_PAGED : EX_SESSION_POOL_NONPAGED;
    SIZE_T Actual;
    PVOID Block;

    if (Pool == NULL)
        return ExpPoolAllocate(HeapIndex, NumberOfBytes, Tag, HeapFlags);

    Block = PoolHeapAllocateEx(Pool->Heap[Index], NumberOfBytes, Tag, HeapFlags, &Actual);
    if (Block == NULL)
    {
        DPRINT1("Session %lu pool allocation failed: %Iu bytes, tag %.4s\n", Pool->SessionId, NumberOfBytes,
                (PCHAR)&Tag);
        return NULL;
    }

    PoolTagCharge(&ExpPoolState.Tracker, Tag, ExpPoolClass(HeapIndex), Actual);
    POOL_ATOMIC_ADD64(&Pool->Bytes[Index], (LONG64)Actual);
    POOL_ATOMIC_ADD32(&Pool->Allocations[Index], 1);
    return Block;
}

BOOLEAN
ExpSessionPoolFree(
    _In_ PVOID Block)
{
    POOL_BLOCK_INFO Info;
    PEX_SESSION_POOL Pool;
    ULONG Index;

    Pool = ExpSessionPoolOfAddress(Block, &Index);
    if (Pool == NULL)
        return FALSE;

    if (Index == EX_SESSION_POOL_PAGED && KeGetCurrentIrql() > APC_LEVEL)
        KeBugCheckEx(BAD_POOL_CALLER, 0x09, KeGetCurrentIrql(), PagedPoolSession, (ULONG_PTR)Block);

    if (!PoolHeapFree(Pool->Heap[Index], Block, &Info))
        KeBugCheckEx(BAD_POOL_CALLER, 0x07, (ULONG_PTR)Block, Index, 0);

    PoolTagRelease(&ExpPoolState.Tracker, Info.Tag,
                   ExpPoolClass(Index == EX_SESSION_POOL_PAGED ? EX_POOL_HEAP_PAGED : EX_POOL_HEAP_NONPAGED),
                   Info.Size);
    POOL_ATOMIC_ADD64(&Pool->Bytes[Index], -(LONG64)Info.Size);
    POOL_ATOMIC_ADD32(&Pool->Allocations[Index], -1);
    return TRUE;
}

BOOLEAN
ExpSessionPoolQuery(
    _In_ PVOID Block,
    _Out_ PPOOL_BLOCK_INFO Info)
{
    PEX_SESSION_POOL Pool;
    ULONG Index;

    Pool = ExpSessionPoolOfAddress(Block, &Index);
    return Pool != NULL && PoolHeapQuery(Pool->Heap[Index], Block, Info);
}
