//
// Created by lilyk on 7/14/2026.
//
/* ============================================================================
 *  pfn.c  --  the frame->metadata table (sparse, Option A: reserve-all,
 *  commit-per-frame). frame_valid_bitmap rejects holes so a corrupt PTE can't
 *  return an uninitialized (zeroed) lock.
 * ========================================================================== */

#include "Vmm.h"

VOID
setup_pfn_metadata(ULONG_PTR physical_page_count, PULONG_PTR physical_page_numbers) {
    for (ULONG_PTR j = 0; j < physical_page_count; j++) {
        if (physical_page_numbers[j] > max_frame_number) {
            max_frame_number = physical_page_numbers[j];
        }
    }

    ULONG_PTR table_reserve_size = (max_frame_number + 1) * sizeof(pfn_metadata);

    frame_to_pfn_table = (pfn_metadata*)VirtualAlloc(
        NULL, table_reserve_size, MEM_RESERVE, PAGE_READWRITE);

    if (frame_to_pfn_table == NULL) {
        printf("CRITICAL: Failed to reserve tracking table. Error: %lu\n", GetLastError());
        DebugBreak();
        return;
    }

    ULONG_PTR bitmap_bytes = ((max_frame_number / 64) + 1) * sizeof(ULONG64);
    frame_valid_bitmap = zero_malloc(bitmap_bytes);

    for (ULONG_PTR j = 0; j < physical_page_count; j++) {
        ULONG64 frame = physical_page_numbers[j];

        ensure_metadata_slot_is_committed(frame_to_pfn_table, frame);

        pfn_metadata* p = &frame_to_pfn_table[frame];

        p->frame_number         = frame;
        p->pte                  = NULL;
        p->isOccupied           = 0;
        p->disc_index           = INVALID_DISC_SLOT;
        p->isBeingWrittenToDisc = 0;

        InitializeCriticalSectionAndSpinCount(&p->lock, 4000);

        frame_valid_bitmap[frame >> 6] |= (1ULL << (frame & 63));

        InitializeListHead(&p->list);
        LockedInsertTail(&pfn_free_list, &p->list);
    }

    MEMORY_BASIC_INFORMATION mbi;
    ULONG_PTR committed = 0;
    PCHAR addr = (PCHAR)frame_to_pfn_table;
    PCHAR end  = addr + table_reserve_size;
    while (addr < end && VirtualQuery(addr, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT) committed += mbi.RegionSize;
        addr += mbi.RegionSize;
    }
    printf("PFN table: committed %llu MB of %llu MB reserved (%llu frames)\n",
           (ULONG64)committed / MB(1),
           (ULONG64)table_reserve_size / MB(1),
           (ULONG64)physical_page_count);
}

pfn_metadata*
find_pfn_from_frame_number(ULONG64 frame_number) {
    if (!FRAME_IS_VALID(frame_number)) return NULL;
    return &frame_to_pfn_table[frame_number];
}

VOID
ensure_metadata_slot_is_committed(pfn_metadata* table_base, ULONG64 frame_number) {
    ULONG_PTR struct_start_va = (ULONG_PTR)&table_base[frame_number];
    ULONG_PTR struct_end_va   = struct_start_va + sizeof(pfn_metadata) - 1;

    ULONG_PTR start_page = struct_start_va / PAGE_SIZE;
    ULONG_PTR end_page   = struct_end_va   / PAGE_SIZE;

    ULONG_PTR pages_to_commit = (end_page - start_page) + 1;
    ULONG_PTR bytes_to_commit = pages_to_commit * PAGE_SIZE;

    PVOID page_aligned_address = (PVOID)(struct_start_va & ~((ULONG_PTR)PAGE_SIZE - 1));

    if (VirtualAlloc(page_aligned_address, bytes_to_commit, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        printf("CRITICAL: Failed to commit metadata pages for frame %llu. Error: %lu\n",
               frame_number, GetLastError());
        DebugBreak();
    }
}

PVOID
zero_malloc(size_t num_bytes) {
    PVOID p = malloc(num_bytes);
    if (p == NULL) {
        DebugBreak();
    }
    memset(p, 0, num_bytes);
    return p;
}
