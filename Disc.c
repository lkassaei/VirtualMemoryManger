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

    disc_bitmap_words = DISC_BITMAP_WORDS(disc_page_count);
    disc_bitmap = (volatile LONG64*)zero_malloc(disc_bitmap_words * sizeof(LONG64));
    if (disc_bitmap == NULL) {
        printf("CRITICAL: could not allocate disc bitmap\n");
        return NULL;
    }

    /* The last word may cover slots past disc_page_count. Pre-mark those bits
     * OCCUPIED so the allocator can never hand out an out-of-range slot. */
    for (ULONG64 b = disc_page_count; b < disc_bitmap_words * 64; b++) {
        disc_bitmap[b >> 6] |= (1LL << (b & 63));
    }
    g_disc_free_count = (LONG64)disc_page_count;

    return p;
}

/* Per-thread scan cursor. This is what actually delivers parallel allocation:
 * without it every thread scans from word 0 and they collide on the same bits
 * and the same cache line. With it, threads claim in different words. */
static __declspec(thread) ULONG64 t_disc_hint = 0;

ULONG64
find_free_disc_slot(void) {
    if (disc_bitmap_words == 0) return INVALID_DISC_SLOT;

    ULONG64 start = t_disc_hint;
    if (start >= disc_bitmap_words) start = 0;

    for (ULONG64 pass = 0; pass < disc_bitmap_words; pass++) {
        ULONG64 w = start + pass;
        if (w >= disc_bitmap_words) w -= disc_bitmap_words;

        for (;;) {
            /* Plain read is fine: the BTS below re-verifies atomically, so a
             * stale word only costs one wasted attempt. */
            LONG64 word = disc_bitmap[w];
            if ((ULONG64)word == ~0ULL) break;      /* word full -> next word */

            unsigned long bit;
            _BitScanForward64(&bit, (unsigned __int64)(~(unsigned __int64)word));

            /* lock bts: returns the PREVIOUS bit. 0 means we flipped it 0->1,
             * so the slot is exclusively ours. */
            if (_interlockedbittestandset64(&disc_bitmap[w], (LONG64)bit) == 0) {
                t_disc_hint = w;
                InterlockedDecrement64(&g_disc_free_count);
                return (w << 6) + bit;
            }
            /* lost the race for that bit -- re-read the word and try again.
             * Terminates: every failure means the word gained a set bit. */
        }
    }
    return INVALID_DISC_SLOT;   /* disc full */
}

VOID
free_disc_slot(ULONG64 slot) {
    VMM_ASSERT(slot < disc_page_count,
               "free_disc_slot: slot %llu out of range (count=%llu)\n", slot, disc_page_count);

    ULONG64 w   = slot >> 6;
    LONG64  bit = (LONG64)(slot & 63);

    /* lock btr: returns the PREVIOUS bit. It must have been 1 -- a 0 means
     * this slot was freed twice, which would let two owners share it. */
    unsigned char was_set = _interlockedbittestandreset64(&disc_bitmap[w], bit);
    VMM_ASSERT(was_set, "free_disc_slot: DOUBLE FREE of slot %llu\n", slot);

    InterlockedIncrement64(&g_disc_free_count);
    t_disc_hint = w;   /* next alloc on this thread reuses a warm word */
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
