//
// Created by lilyk on 7/14/2026.
//
/* ============================================================================
 *  fault.c  --  fault dispatch, soft/hard fault, standby rescue, staging slots.
 *  Lock hierarchy: region -> page -> list -> disc.
 * ========================================================================== */

#include "Vmm.h"

/* Per-thread staging state. TLS: each thread gets its own copy, so slot
 * selection needs no lock -- a thread only ever touches its own slice. */
__declspec(thread) int t_thread_number = 0;
__declspec(thread) int t_ring_pos      = 0;
static __declspec(thread) PVOID  t_deferred[RING_SLOTS_PER_THREAD];
static __declspec(thread) int    t_deferred_n    = 0;

static VOID          handle_soft_fault(PPTE pte, PVOID va, CRITICAL_SECTION* my_pte_lock);
static VOID          handle_hard_fault(PPTE pte, PVOID va, CRITICAL_SECTION* my_pte_lock);
static pfn_metadata* pull_from_standby_safely(CRITICAL_SECTION* my_pte_lock);

static void
flush_staging_ring(void) {
    if (t_deferred_n > 0) {
        MapUserPhysicalPagesScatter(t_deferred, t_deferred_n, NULL);  /* ONE unmap for all */
        t_deferred_n = 0;
        t_ring_pos   = 0;
    }
}

VOID staging_flush_current_thread(void) { flush_staging_ring(); }

/* claim next slot; flush the whole ring first if full, so we NEVER map into a
 * slot that still holds a deferred frame */
static PVOID
next_staging_slot(void) {
    if (t_ring_pos >= STAGING_SLOTS_PER_THREAD) {
        flush_staging_ring();
    }
    int slot = t_thread_number * STAGING_SLOTS_PER_THREAD + t_ring_pos;
    VMM_ASSERT(slot >= 0 && slot < STAGING_SLOTS,
               "BAD STAGING SLOT: slot=%d thread=%d\n", slot, t_thread_number);
    return (char*)staging_va_start + ((ULONG_PTR)slot * PAGE_SIZE);
}

VOID
handle_page_fault(PVOID arbitrary_va) {
    PPTE pte = get_pte_from_va(arbitrary_va);
    CRITICAL_SECTION* my_pte_lock = get_pte_lock_for_va(arbitrary_va);

    // in handle_page_fault, replace the bare EnterCriticalSection with a timed one:

    // LARGE_INTEGER t0, t1;
    // QueryPerformanceCounter(&t0);
    EnterCriticalSection(my_pte_lock);          // this is what blocks under contention
    // QueryPerformanceCounter(&t1);
    //DIAG_ADD(g_time_lock_wait, t1.QuadPart - t0.QuadPart);

    if (pte->hardware.valid == 1) {                 /* another thread beat us */
        LeaveCriticalSection(my_pte_lock);
        return;
    }
    if (pte->transition.transition == 1) {          /* check transition BEFORE disc */
        handle_soft_fault(pte, arbitrary_va, my_pte_lock);
    }
    else if (pte->disc.disc == 1) {                 /* on disc */
        handle_hard_fault(pte, arbitrary_va, my_pte_lock);
    }
    else if (pte->disc.disc == 0) {                 /* fresh: first-touch */
        handle_hard_fault(pte, arbitrary_va, my_pte_lock);
    }
    else {
        printf("CRITICAL: Unrecognized PTE state!\n");
        LeaveCriticalSection(my_pte_lock);
        DebugBreak();
    }
}

/* ============================================================================
 *  handle_hard_fault  --  clean version, no prefetch.
 *
 *  Called (via handle_page_fault dispatch) when a faulting VA is either on disc
 *  or a first-touch page. Two paths:
 *    - demand-zero (first touch): publish directly, ONE syscall, no staging.
 *    - disc-backed: fill through a private per-thread staging slot, then publish.
 *
 *  Holds my_pte_lock on entry; releases it on every exit.
 * ========================================================================== */

static VOID
handle_hard_fault(PPTE pte, PVOID arbitrary_va, CRITICAL_SECTION* my_pte_lock) {

    /* THIS PTE's disc slot -- the data we must read back. A frame pulled from
     * standby is a fresh frame for our VA; its previous owner's disc_index is
     * irrelevant (pull_from_standby_safely already repointed that owner). */
    BOOL    is_on_disc   = (pte->disc.disc == 1);
    ULONG64 my_disc_slot = is_on_disc ? pte->disc.disc_index : INVALID_DISC_SLOT;

    /* ---- acquire a frame: free list, else standby, else wait ---- */
    pfn_metadata* target_pfn = NULL;
    while (target_pfn == NULL) {
        PLIST_ENTRY free_entry = LockedRemoveHead(&pfn_free_list);
        if (free_entry != NULL) {
            target_pfn = GetPfnFromListEntry(free_entry);
        } else {
            target_pfn = pull_from_standby_safely(my_pte_lock);
            if (target_pfn != NULL) {
                unlock_pfn(target_pfn);   /* returned page-locked; release after handoff */
            }
        }

        if (target_pfn == NULL) {
            SetEvent(LowPagesEvent);
            // DIAG_COUNT(g_fault_stalls);      /* controller signal */
            LeaveCriticalSection(my_pte_lock);
            while (pfn_free_list.count == 0 && standby_total_count() == 0) {
                WaitForSingleObject(StandbyPageAvailableEvent, 10);   /* NOT INFINITE */
            }
            EnterCriticalSection(my_pte_lock);

            /* World changed while we slept -- another thread may have resolved
             * this VA, or trim converted it to transition. Bail and re-dispatch. */
            if (pte->hardware.valid == 1 || pte->transition.transition == 1) {
                LeaveCriticalSection(my_pte_lock);
                return;
            }
            is_on_disc   = (pte->disc.disc == 1);
            my_disc_slot = is_on_disc ? pte->disc.disc_index : INVALID_DISC_SLOT;
        }
    }

    if (pfn_free_list.count < LOWEST_PAGES) {
        SetEvent(LowPagesEvent);
    }

    /* The frame is now exclusively ours; it carries no disc slot. */
    lock_pfn(target_pfn);
    target_pfn->disc_index = INVALID_DISC_SLOT;
    unlock_pfn(target_pfn);

    PVOID va_aligned = (PVOID)((ULONG_PTR)arbitrary_va & ~((ULONG_PTR)PAGE_SIZE - 1));

    /* ================================================================
     *  FAST PATH: demand-zero (first touch). Fresh AWE frames are already
     *  zero, and zero IS the correct content, so there is no stale-data
     *  window -- publish directly at the real VA. ONE syscall, no staging.
     * ================================================================ */
    if (!is_on_disc || my_disc_slot == INVALID_DISC_SLOT) {
        DIAG_COUNT(g_hard_faults_zero);

        /* Prefer a pre-zeroed frame from the background thread: 1 syscall, no
         * inline zeroing. We return the frame we already acquired and use the
         * pre-zeroed one instead. */
        PLIST_ENTRY ze = LockedRemoveHead(&pfn_zeroed_list);
        if (ze != NULL) {
            DIAG_COUNT(g_dz_from_zerolist);
            /* hand back the frame we pulled at the top -- it may be dirty, that's
             * fine, it goes back to the free list for the zeroing thread to grab. */
            lock_pfn(target_pfn);
            target_pfn->isOccupied = 0;
            target_pfn->disc_index = INVALID_DISC_SLOT;
            unlock_pfn(target_pfn);
            target_pfn->pte = NULL;
            LockedInsertTail(&pfn_free_list, &target_pfn->list);

            /* switch to the pre-zeroed frame (is_zero == 1, off all lists) */
            target_pfn = GetPfnFromListEntry(ze);

            if (pfn_zeroed_list.count < ZEROED_LIST_LOW) {
                SetEvent(NeedZeroingEvent);
            }

            if (MapUserPhysicalPages(va_aligned, 1, &target_pfn->frame_number) == FALSE) {
                lock_pfn(target_pfn);
                target_pfn->isOccupied = 0;
                target_pfn->disc_index = INVALID_DISC_SLOT;
                unlock_pfn(target_pfn);
                target_pfn->pte = NULL;
                LockedInsertTail(&pfn_free_list, &target_pfn->list);
                LeaveCriticalSection(my_pte_lock);
                return;
            }
            goto publish_pte;
        }

        /* Zeroed list empty -- fall back to inline handling of the frame we have. */
        SetEvent(NeedZeroingEvent);   /* nudge: we're consuming faster than producing */

        if (target_pfn->is_zero == 1) {
            DIAG_COUNT(g_dz_pristine);
            /* pristine frame: publish direct */
            if (MapUserPhysicalPages(va_aligned, 1, &target_pfn->frame_number) == FALSE) {
                lock_pfn(target_pfn);
                target_pfn->isOccupied = 0;
                target_pfn->disc_index = INVALID_DISC_SLOT;
                unlock_pfn(target_pfn);
                target_pfn->pte = NULL;
                LockedInsertTail(&pfn_free_list, &target_pfn->list);
                LeaveCriticalSection(my_pte_lock);
                return;
            }
        } else {
            DIAG_COUNT(g_dz_dirty);
            /* dirty frame: zero via the DEDICATED per-thread slot (the one after the
             * ring). Immediate unmap. Never touches t_ring_pos / the deferred ring.
             * Nearly-dead path (zeroing thread keeps g_dz_dirty ~= 0). */
            int dz_slot = t_thread_number * STAGING_SLOTS_PER_THREAD + RING_SLOTS_PER_THREAD;
            VMM_ASSERT(dz_slot >= 0 && dz_slot < STAGING_SLOTS,
                       "BAD DZ SLOT: slot=%d thread=%d\n", dz_slot, t_thread_number);
            PVOID staging_va = (char*)staging_va_start + ((ULONG_PTR)dz_slot * PAGE_SIZE);

            if (MapUserPhysicalPages(staging_va, 1, &target_pfn->frame_number) == FALSE) {
                DIAG_PRINT("Failed to map staging (zero). Error: %lu\n", GetLastError());
                lock_pfn(target_pfn);
                target_pfn->isOccupied = 0;
                target_pfn->disc_index = INVALID_DISC_SLOT;
                unlock_pfn(target_pfn);
                target_pfn->pte = NULL;
                LockedInsertTail(&pfn_free_list, &target_pfn->list);
                LeaveCriticalSection(my_pte_lock);
                return;
            }
            memset(staging_va, 0, PAGE_SIZE);
            MapUserPhysicalPages(staging_va, 1, NULL);   /* immediate -- dedicated slot */

            if (MapUserPhysicalPages(va_aligned, 1, &target_pfn->frame_number) == FALSE) {
                lock_pfn(target_pfn);
                target_pfn->isOccupied = 0;
                target_pfn->disc_index = INVALID_DISC_SLOT;
                unlock_pfn(target_pfn);
                target_pfn->pte = NULL;
                LockedInsertTail(&pfn_free_list, &target_pfn->list);
                LeaveCriticalSection(my_pte_lock);
                return;
            }
        }
        goto publish_pte;
    }

    /* ================================================================
     *  DISC-BACKED PATH: must fill the frame from disc through a PRIVATE
     *  per-thread staging slot before publishing, so no other thread ever
     *  sees the real VA mapped with stale contents.
     * ================================================================ */
     DIAG_COUNT(g_hard_faults_disc);
    {
        /* Flush the WHOLE ring before reusing any slot -- never map into a slot
         * that still holds a deferred (mapped) frame. t_ring_pos counts up to
         * RING_SLOTS_PER_THREAD then we batch-unmap all and reset. NO modulo. */
        if (t_ring_pos >= RING_SLOTS_PER_THREAD) {
            flush_staging_ring();
        }

        int   slot = t_thread_number * STAGING_SLOTS_PER_THREAD + t_ring_pos;
        VMM_ASSERT(slot >= 0 && slot < STAGING_SLOTS,
                   "BAD RING SLOT: slot=%d thread=%d ring_pos=%d\n",
                   slot, t_thread_number, t_ring_pos);
        PVOID staging_va = (char*)staging_va_start + ((ULONG_PTR)slot * PAGE_SIZE);

        /* 1. map frame into MY private staging slot */
        if (MapUserPhysicalPages(staging_va, 1, &target_pfn->frame_number) == FALSE) {
            DIAG_PRINT("Failed to map staging. VA %p Error: %lu\n", va_aligned, GetLastError());
            lock_pfn(target_pfn);
            target_pfn->isOccupied = 0;
            target_pfn->disc_index = INVALID_DISC_SLOT;
            unlock_pfn(target_pfn);
            target_pfn->pte = NULL;
            LockedInsertTail(&pfn_free_list, &target_pfn->list);
            LeaveCriticalSection(my_pte_lock);
            return;
        }

        /* record as deferred AFTER a confirmed map, so t_deferred only holds
         * genuinely-mapped VAs (batch unmap never touches an unmapped VA) */
        t_deferred[t_ring_pos] = staging_va;
        t_ring_pos++;
        t_deferred_n = t_ring_pos;

        /* 2. fill from disc, free the disc slot */
        memcpy(staging_va, (char*)disc + (my_disc_slot * PAGE_SIZE), PAGE_SIZE);
        free_disc_slot(my_disc_slot);

        /* 3. NO unmap here -- deferred to flush_staging_ring(). */

        /* 4. publish at the real VA */
        if (MapUserPhysicalPages(va_aligned, 1, &target_pfn->frame_number) == FALSE) {
            lock_pfn(target_pfn);
            target_pfn->isOccupied = 0;
            target_pfn->disc_index = INVALID_DISC_SLOT;
            unlock_pfn(target_pfn);
            target_pfn->pte = NULL;
            LockedInsertTail(&pfn_free_list, &target_pfn->list);
            LeaveCriticalSection(my_pte_lock);
            return;
        }
    }

publish_pte:
    /* ---- common tail: make PTE valid FIRST, then activate + publish to age
     *      list, so no harvester ever sees occ=1 / valid=0. ---- */
    set_pte_valid(pte, target_pfn->frame_number);   /* atomic; includes age=0 */

    lock_pfn(target_pfn);
    target_pfn->isOccupied = 1;
    target_pfn->is_zero = 0;
    unlock_pfn(target_pfn);
    target_pfn->pte = pte;

    ULONG64 region_index = (pte - page_table) / PTE_REGION_SIZE;
    VMM_ASSERT(target_pfn->list.Flink == NULL && target_pfn->list.Blink == NULL,
           "DOUBLE INSERT: pfn=%p Flink=%p Blink=%p occ=%d pte=%p\n",
           target_pfn, target_pfn->list.Flink, target_pfn->list.Blink,
           (int)target_pfn->isOccupied, target_pfn->pte);
    InsertTailList(&pte_regions[region_index].active_age_lists[0], &target_pfn->list);

    LeaveCriticalSection(my_pte_lock);
}


static VOID
handle_soft_fault(PPTE pte, PVOID arbitrary_va, CRITICAL_SECTION* my_pte_lock) {
    DIAG_COUNT(g_soft_faults);

    ULONG64 frame_number = pte->transition.frame_number;
    pfn_metadata* target_pfn = find_pfn_from_frame_number(frame_number);

    if (target_pfn == NULL) {
        printf("SOFT FAULT: no PFN for frame 0x%llx pte=%p\n", frame_number, pte);
        DebugBreak();
        LeaveCriticalSection(my_pte_lock);
        return;
    }

    lock_pfn(target_pfn);                              /* region -> page */

    if (target_pfn->pte != pte) {
        unlock_pfn(target_pfn);
        LeaveCriticalSection(my_pte_lock);
        return;
    }

    ULONG64 slot_to_free = INVALID_DISC_SLOT;

    if (target_pfn->isBeingWrittenToDisc == 1) {
        /* Poach from the disc writer. Deliberately KEEP disc_index: the disc
         * thread's Phase 3 detects the poach (isBeingWrittenToDisc==0) and
         * reclaims the slot there. Do NOT free it here -- double free. */
        target_pfn->isBeingWrittenToDisc = 0;
    }

    target_pfn->isBeingWrittenToDisc = 0;      /* unconditional poach claim (idempotent) */

    if (target_pfn->isOccupied == 2) {
        LockedTryRemoveEntry(&pfn_modified_list, &target_pfn->list);
    } else if (target_pfn->isOccupied == 3) {
        int sh = standby_shard_of(target_pfn->frame_number);
        LockedTryRemoveEntry(&pfn_standby_shards[sh], &target_pfn->list);
    }

    target_pfn->isOccupied = 1;
    unlock_pfn(target_pfn);

    if (slot_to_free != INVALID_DISC_SLOT) {
        free_disc_slot(slot_to_free);
    }

    PVOID va_aligned = (PVOID)((ULONG_PTR)arbitrary_va & ~((ULONG_PTR)PAGE_SIZE - 1));
    if (MapUserPhysicalPages(va_aligned, 1, &target_pfn->frame_number) == FALSE) {
        printf("CRITICAL: soft fault remap failed. Error: %lu\n", GetLastError());
        DebugBreak();
        lock_pfn(target_pfn);
        target_pfn->disc_index = INVALID_DISC_SLOT;
        target_pfn->isOccupied = 0;
        unlock_pfn(target_pfn);
        target_pfn->pte = NULL;
        LockedInsertTail(&pfn_free_list, &target_pfn->list);
        LeaveCriticalSection(my_pte_lock);
        return;
    }

    *(PULONG64)pte = 0;
    set_pte_valid(pte, target_pfn->frame_number);
    pte->hardware.age = 0;

    target_pfn->pte = pte;

    ULONG64 region_index = (pte - page_table) / PTE_REGION_SIZE;
    if (target_pfn->list.Flink != NULL &&
        target_pfn->list.Flink != &target_pfn->list) {
        printf("DOUBLE INSERT: pfn=%p Flink=%p Blink=%p occ=%d pte=%p\n",
               target_pfn, target_pfn->list.Flink, target_pfn->list.Blink,
               (int)target_pfn->isOccupied, target_pfn->pte);
        DebugBreak();
    }
    VMM_ASSERT(target_pfn->list.Flink == NULL && target_pfn->list.Blink == NULL,
           "soft age-insert still-linked: pfn=%p Flink=%p occ=%d\n",
           target_pfn, target_pfn->list.Flink, (int)target_pfn->isOccupied);
    InsertTailList(&pte_regions[region_index].active_age_lists[0], &target_pfn->list);

    LeaveCriticalSection(my_pte_lock);
}

/* Returns a frame exclusively the caller's, PAGE-LOCKED (lock_pfn held) so no
 * soft fault can touch it during handoff. Caller must unlock_pfn(). NULL if no
 * viable standby frame right now.
 *
 * Lock hierarchy region -> page -> list is preserved. Uses TWO standby
 * acquisitions by design:
 *   - acquisition 1: peek the head candidate, snapshot its owner, release.
 *   - then take region -> page locks (impossible while holding the list lock).
 *   - acquisition 2: remove from the list, now that the higher locks are held.
 *
 * EVERY path that holds pfn_standby_list.lock releases it before continue/return
 * and before taking any other lock. That is the invariant; the deadlock came
 * from a path that skipped the release. */
static pfn_metadata*
pull_from_standby_safely(CRITICAL_SECTION* my_pte_lock) {
    ULONG64 attempts = 0;
    int start = t_thread_number & (STANDBY_SHARDS - 1);   /* spread threads across shards */

    for (;;) {
        if (++attempts > 32) return NULL;

        /* try each shard once, starting at our own, before giving up */
        pfn_metadata* cand = NULL;
        PPTE owner_pte = NULL;
        LOCKED_LIST* shard = NULL;

        for (int k = 0; k < STANDBY_SHARDS; k++) {
            LOCKED_LIST* s = &pfn_standby_shards[(start + k) & (STANDBY_SHARDS - 1)];

            /* lock-free peek: skip empty shards without acquiring */
            if (s->head.Flink == &s->head) continue;

            EnterCriticalSection(&s->lock);
            PLIST_ENTRY e = s->head.Flink;
            if (e == &s->head) { LeaveCriticalSection(&s->lock); continue; }
            cand      = GetPfnFromListEntry(e);
            owner_pte = cand->pte;
            BOOL viable = (cand->isOccupied == 3 && cand->isBeingWrittenToDisc == 0);
            LeaveCriticalSection(&s->lock);

            if (!viable) { cand = NULL; continue; }
            shard = s;
            break;   /* found a candidate in this shard */
        }

        if (cand == NULL) return NULL;   /* all shards empty/unviable this pass */

        /* ---- NO-OWNER PATH ---- */
        if (owner_pte == NULL) {
            EnterCriticalSection(&shard->lock);
            if (cand->isOccupied != 3 || cand->isBeingWrittenToDisc != 0 ||
                cand->pte != NULL || cand->list.Flink == NULL) {
                LeaveCriticalSection(&shard->lock);
                continue;
            }
            RemoveEntryList(&cand->list);
            cand->list.Flink = cand->list.Blink = NULL;
            shard->count--;
            LeaveCriticalSection(&shard->lock);

            lock_pfn(cand);
            cand->disc_index = INVALID_DISC_SLOT;
            cand->isOccupied = 1;
            return cand;
        }

        /* ---- OWNER PATH ---- */
        CRITICAL_SECTION* owner_lock = get_pte_lock_from_pte_pointer(owner_pte);
        if (owner_lock != my_pte_lock) {
            if (!TryEnterCriticalSection(owner_lock)) continue;
        }
        lock_pfn(cand);
        if (cand->isOccupied != 3 || cand->isBeingWrittenToDisc != 0 ||
            cand->pte != owner_pte) {
            unlock_pfn(cand);
            if (owner_lock != my_pte_lock) LeaveCriticalSection(owner_lock);
            continue;
        }
        if (!LockedTryRemoveEntry(shard, &cand->list)) {   /* remove from the SAME shard */
            unlock_pfn(cand);
            if (owner_lock != my_pte_lock) LeaveCriticalSection(owner_lock);
            continue;
        }
        set_to_disc_state(owner_pte, cand);
        cand->disc_index = INVALID_DISC_SLOT;
        cand->isOccupied = 1;
        cand->pte = NULL;
        if (owner_lock != my_pte_lock) LeaveCriticalSection(owner_lock);
        return cand;
    }
}