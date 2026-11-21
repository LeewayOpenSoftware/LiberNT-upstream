/*
 * PROJECT:     LiberNT Pool Allocator
 * FILE:        ntoskrnl/nvs/pool/include/poolback.h
 * PURPOSE:     Pool backing provider interfaces
 *
 * SPDX-FileCopyrightText: 2026 Ahmed ARIF
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

typedef struct _POOL_KM_LAYOUT
{
    PVOID NonPagedResidentBase;
    SIZE_T NonPagedResidentBytes;
    PVOID NonPagedSharedBase;
    SIZE_T NonPagedSharedBytes;
    PVOID PagedBase;
    SIZE_T PagedBytes;
} POOL_KM_LAYOUT, *PPOOL_KM_LAYOUT;

VOID PoolBackQueryLayout(_Out_ PPOOL_KM_LAYOUT Layout);
VOID PoolBackDemandBacking(_Out_ PPOOL_BACKING Backing, _In_ BOOLEAN Executable);
VOID PoolBackResidentBacking(_Out_ PPOOL_BACKING Backing);

typedef struct _POOL_SESSION_BACKING
{
    PVOID Space;
    ULONG Protection;
} POOL_SESSION_BACKING, *PPOOL_SESSION_BACKING;

VOID PoolBackSessionBacking(_Out_ PPOOL_BACKING Backing, _In_ PPOOL_SESSION_BACKING Context);
