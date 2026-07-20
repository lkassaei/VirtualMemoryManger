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
        }

        if (target_pfn == NULL) {
            SetEvent(LowPagesEvent);
            DIAG_COUNT(g_fault_stalls);      /* controller signal */
            LeaveCriticalSection(my_pte_lock);
            while (pfn_free_list.count == 0 && pfn_standby_list.count == 0) {
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
        EnterCriticalSection(&disc_lock);
        disc_metadata[my_disc_slot].isOccupied = FALSE;
        LeaveCriticalSection(&disc_lock);
        LockedInsertTail(&disc_free_list, &disc_metadata[my_disc_slot].list);

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
    else if (target_pfn->isOccupied == 3) {
        if (LockedTryRemoveEntry(&pfn_standby_list, &target_pfn->list)) {
            slot_to_free = target_pfn->disc_index;
            target_pfn->disc_index = INVALID_DISC_SLOT;
        }
    }
    else if (target_pfn->isOccupied == 2) {
        LockedTryRemoveEntry(&pfn_modified_list, &target_pfn->list);
    }

    target_pfn->isOccupied = 1;                        /* signal disc Phase 1 re-checks */
    unlock_pfn(target_pfn);

    if (slot_to_free != INVALID_DISC_SLOT) {
        EnterCriticalSection(&disc_lock);
        disc_metadata[slot_to_free].isOccupied = FALSE;
        LeaveCriticalSection(&disc_lock);
        LockedInsertTail(&disc_free_list, &disc_metadata[slot_to_free].list);
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
    InsertTailList(&pte_regions[region_index].active_age_lists[0], &target_pfn->list);

    LeaveCriticalSection(my_pte_lock);
}


static pfn_metadata*
pull_from_standby_safely(CRITICAL_SECTION* my_pte_lock) {
    ULONG64 attempts = 0;
    for (;;) {
        if (++attempts > 32) return NULL;

        /* Grab the head entry under the lock and GET OUT. No walking, no
         * skip-scan -- just take the first entry's identity and release.
         * Everything else happens outside the standby lock. */
        pfn_metadata* cand = NULL;
        PPTE owner_pte = NULL;

        EnterCriticalSection(&pfn_standby_list.lock);
        PLIST_ENTRY e = pfn_standby_list.head.Flink;
        if (e != &pfn_standby_list.head) {
            cand = GetPfnFromListEntry(e);
            owner_pte = cand->pte;                 /* snapshot under lock */
        }
        LeaveCriticalSection(&pfn_standby_list.lock);   /* OUT immediately */

        if (cand == NULL) return NULL;   /* standby empty */

        /* viability check OUTSIDE the standby lock */
        if (cand->isOccupied != 3 || cand->isBeingWrittenToDisc != 0) {
            continue;   /* not viable; retry grabs a different head next time */
        }

        if (owner_pte == NULL) {
            /* no owner -- claim under the lock, re-verify */
            EnterCriticalSection(&pfn_standby_list.lock);
            if (cand->isOccupied != 3 || cand->isBeingWrittenToDisc != 0 ||
                cand->pte != NULL || cand->list.Flink == NULL) {
                LeaveCriticalSection(&pfn_standby_list.lock);
                continue;
            }
            RemoveEntryList(&cand->list);
            cand->list.Flink = cand->list.Blink = NULL;
            pfn_standby_list.count--;
            LeaveCriticalSection(&pfn_standby_list.lock);

            cand->disc_index = INVALID_DISC_SLOT;
            cand->isOccupied = 1;
            return cand;
        }

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

        if (!LockedTryRemoveEntry(&pfn_standby_list, &cand->list)) {
            unlock_pfn(cand);
            if (owner_lock != my_pte_lock) LeaveCriticalSection(owner_lock);
            continue;
        }

        set_to_disc_state(owner_pte, cand);
        cand->disc_index = INVALID_DISC_SLOT;
        cand->isOccupied = 1;
        unlock_pfn(cand);

        cand->pte = NULL;
        if (owner_lock != my_pte_lock) LeaveCriticalSection(owner_lock);
        return cand;
    }
}



VOID
handle_page_fault_run(PVOID base_va, ULONG64 run_ahead) {
    PPTE base_pte = get_pte_from_va(base_va);
    CRITICAL_SECTION* my_pte_lock = get_pte_lock_for_va(base_va);

    /* Clamp the batch so it never crosses this PTE's region boundary
     * (crossing would need a second region lock -> ABBA risk). */
    ULONG64 pte_index      = (ULONG64)(base_pte - page_table);
    ULONG64 region_end     = ((pte_index / PTE_REGION_SIZE) + 1) * PTE_REGION_SIZE;
    ULONG64 room_in_region = region_end - pte_index;               /* PTEs left in region */
    ULONG64 want = run_ahead + 1;                                  /* this page + prefetch */
    if (want > room_in_region) want = room_in_region;
    if (want > MAX_PREFETCH + 1) want = MAX_PREFETCH + 1;

    EnterCriticalSection(my_pte_lock);

    /* Collect the contiguous run of pages that are FAULTS OF THE SAME KIND
     * starting at base. Stop at the first page that's valid / transition /
     * different-state -- the batch only covers uniform demand-zero (or uniform
     * disc, but disc needs per-page disc_index, so start with demand-zero). */
    PPTE   pte_batch[MAX_PREFETCH + 1];
    PVOID  va_batch[MAX_PREFETCH + 1];
    int    m = 0;

    for (ULONG64 i = 0; i < want; i++) {
        PPTE p = base_pte + i;
        if (p->hardware.valid == 1) break;          /* already mapped: stop the run */
        if (p->transition.transition == 1) break;   /* soft-fault case: handle singly */
        /* demand-zero only for the batch fast path: */
        if (p->disc.disc == 1) break;               /* on disc: needs per-page read, stop */

        pte_batch[m] = p;
        va_batch[m]  = (PVOID)((ULONG_PTR)base_va + i * PAGE_SIZE);
        m++;
    }

    if (m == 0) {
        /* base page wasn't a demand-zero fault -- fall back to the single path,
         * still under the lock we hold. */
        LeaveCriticalSection(my_pte_lock);
        handle_page_fault(base_va);   /* re-dispatches; it re-takes the lock */
        return;
    }

    /* Acquire up to m frames. Take what we can; don't stall for all m. */
    pfn_metadata* frames[MAX_PREFETCH + 1];
    int got = 0;
    for (; got < m; got++) {
        PLIST_ENTRY e = LockedRemoveHead(&pfn_free_list);
        if (e == NULL) {
            pfn_metadata* p = pull_from_standby_safely(my_pte_lock);
            if (p == NULL) break;                   /* out of frames; publish what we have */
            frames[got] = p;
        } else {
            frames[got] = GetPfnFromListEntry(e);
        }
    }

    if (got == 0) {
        /* couldn't get even one frame -- fall back to single path which has the
         * proper wait-for-page loop. */
        LeaveCriticalSection(my_pte_lock);
        handle_page_fault(base_va);
        return;
    }

    /* Demand-zero: frames are already zero (fresh AWE) -- no fill needed.
     * Publish all `got` frames at their real VAs in ONE scatter. */
    ULONG_PTR frame_numbers[MAX_PREFETCH + 1];
    for (int i = 0; i < got; i++) frame_numbers[i] = frames[i]->frame_number;

    if (MapUserPhysicalPagesScatter(va_batch, got, frame_numbers) == FALSE) {
        /* scatter failed: return frames, fall back */
        for (int i = 0; i < got; i++) {
            frames[i]->isOccupied = 0;
            LockedInsertTail(&pfn_free_list, &frames[i]->list);
        }
        LeaveCriticalSection(my_pte_lock);
        handle_page_fault(base_va);
        return;
    }

    /* Stamp PTEs valid + publish pfns to the age list. */
    ULONG64 region_index = pte_index / PTE_REGION_SIZE;
    for (int i = 0; i < got; i++) {
        set_pte_valid(pte_batch[i], frames[i]->frame_number);   /* atomic, age=0 */

        lock_pfn(frames[i]);
        frames[i]->isOccupied = 1;
        unlock_pfn(frames[i]);
        frames[i]->pte = pte_batch[i];

        InsertTailList(&pte_regions[region_index].active_age_lists[0], &frames[i]->list);
    }

    if (pfn_free_list.count < LOWEST_PAGES) SetEvent(LowPagesEvent);

    LeaveCriticalSection(my_pte_lock);
}