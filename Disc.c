//
// Created by lilyk on 7/14/2026.
//
/* ============================================================================
 *  disc.c  --  disc backing store, slot allocation, batched page transfer.
 * ========================================================================== */

#include "Vmm.h"

PVOID
create_page_file(PULONG64 number_of_pages) {
    PVOID p;
    ULONG64 num_bytes;

    if (*number_of_pages > MAX_DISC_SIZE) {
        *number_of_pages = MAX_DISC_SIZE;
    }
    num_bytes = *number_of_pages * PAGE_SIZE;

    p = _aligned_malloc(num_bytes, PAGE_SIZE);
    while (p == NULL) {
        num_bytes /= 2;
        p = _aligned_malloc(num_bytes, PAGE_SIZE);
    }
    *number_of_pages = num_bytes / PAGE_SIZE;

    disc_metadata = malloc(*number_of_pages * sizeof(DISC_METADATA));
    if (disc_metadata == NULL) {
        printf("could not allocate disc_metadata and fake disc\n");
        _aligned_free(p);
        return NULL;
    }
    memset(disc_metadata, 0, *number_of_pages * sizeof(DISC_METADATA));

    for (ULONG64 i = 0; i < *number_of_pages; i++) {
        disc_metadata[i].index      = i;
        disc_metadata[i].isOccupied = FALSE;
        LockedInsertTail(&disc_free_list, &disc_metadata[i].list);
    }

    return p;
}

ULONG64
find_free_disc_slot(VOID) {
    PLIST_ENTRY e = LockedRemoveHead(&disc_free_list);
    if (e == NULL) return INVALID_DISC_SLOT;
    PDISC_METADATA meta = CONTAINING_RECORD(e, DISC_METADATA, list);
    meta->isOccupied = TRUE;
    return meta->index;
}

/* Batched write: one MapUserPhysicalPages for the whole batch into the
 * multi-page scratch window, per-slot memcpy (poached pages skipped), one
 * unmap. Called once per batch from DiscThreadWorker Phase 2. */
VOID
write_to_disc(pfn_metadata** candidates, ULONG64 batch_count) {
    ULONG_PTR frame_array[DISC_BATCH];
    ULONG64   disc_slots[DISC_BATCH];

    for (ULONG64 i = 0; i < batch_count; i++) {
        disc_slots[i]  = candidates[i]->disc_index;
        frame_array[i] = candidates[i]->frame_number;
    }

    if (MapUserPhysicalPages(temp_disc_va_start, batch_count, frame_array) == FALSE) {
        printf("CRITICAL: Failed to map batch scratch space! Error: %lu\n", GetLastError());
        DebugBreak();
        return;
    }

    for (ULONG64 i = 0; i < batch_count; i++) {
        if (disc_slots[i] == INVALID_DISC_SLOT) {
            continue;   /* poached mid-batch; soft fault kept the memory copy */
        }
        if (disc_slots[i] >= NUM_DISC_PAGES) {
            printf("CRITICAL: Invalid disc slot index! i: %llu, slot: 0x%llx\n",
                   i, disc_slots[i]);
            DebugBreak();
            return;   /* NOTE: leaves scratch mapped -- see below */
        }

        void* source_va    = (char*)temp_disc_va_start + (i * PAGE_SIZE);
        void* disc_address = (char*)disc + (disc_slots[i] * PAGE_SIZE);
        memcpy(disc_address, source_va, PAGE_SIZE);
    }

    MapUserPhysicalPages(temp_disc_va_start, batch_count, NULL);
}

/* PASTE read_page_from_disc (or your batched read-back) here. */