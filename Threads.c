/* ============================================================================
 *  threads.c  --  the three background workers plus the trim harvesters.
 *
 *  Lock hierarchy in force:  region -> page -> list -> disc
 *
 *  AgeThreadWorker  : sweeps regions, promotes/demotes pages between age bins.
 *  TrimThreadWorker : on low pages, ages then harvests victims to the mod list.
 *  DiscThreadWorker : writes the mod list to disc, publishes frames to standby.
 * ========================================================================== */

#include "vmm.h"

/* Forward declarations: these are defined below their callers, so they need
 * to be declared first. Both are file-local (static). */
static VOID get_unmap_candidates(int* batch_count, int batch_size);
static VOID harvest_one_bin(PPTE_REGION region, PLIST_ENTRY head,
                            int* batch_count, int batch_size);


DWORD WINAPI
AgeThreadWorker(LPVOID lpParam) {
    UNREFERENCED_PARAMETER(lpParam);
    HANDLE waitEvents[2] = { StartAgingEvent, ShutdownEvent };
    static ULONG64 clock_hand = 0;

    while (TRUE) {
        DWORD wait = WaitForMultipleObjects(2, waitEvents, FALSE, 50);
        if (wait == WAIT_OBJECT_0 + 1) break;   /* Shutdown */

        const ULONG64 SWEEP = num_pte_regions / 2;
        ULONG64 processed = 0;

        while (processed < SWEEP && processed < num_pte_regions) {
            PPTE_REGION region = &pte_regions[clock_hand];

            if (TryEnterCriticalSection(&region->lock)) {

                /* ---- Bins 7..1: descending is REQUIRED. We promote into bin
                 * (age+1), already walked. Ascending would re-walk promotions
                 * and loop forever. Each bin walks to its stable `head` sentinel,
                 * capturing `next` before any removal. ---- */
                for (int age = 7; age >= 1; age--) {
                    PLIST_ENTRY head = &region->active_age_lists[age];
                    if (IsListEmpty(head)) continue;

                    ULONG64 walked = 0;
                    PLIST_ENTRY cur = head->Flink;

                    while (cur != head) {

                        /* GUARD 1: bounded walk -- an age list can't hold more
                         * entries than there are physical frames; exceeding means
                         * a cycle that never reaches head. */
                        if (++walked > NUMBER_OF_PHYSICAL_PAGES) {
                            DIAG_PRINT("AGE LIST CYCLE: region=%p bin=%d walked=%llu cur=%p\n",
                                       region, age, walked, cur);
                            VMM_ASSERT(0, "age list cycle\n");
                            break;
                        }
                        /* GUARD 2: self-linked -- next == cur, walk never advances. */
                        if (cur->Flink == cur) {
                            DIAG_PRINT("SELF-LINKED PFN: entry=%p Flink=%p Blink=%p\n",
                                       cur, cur->Flink, cur->Blink);
                            VMM_ASSERT(0, "self-linked pfn\n");
                            break;
                        }
                        /* GUARD 3: half-unlinked -- removal nulls both links; a
                         * single NULL link is a node caught mid-op or corrupt. */
                        if (cur->Flink == NULL || cur->Blink == NULL) {
                            DIAG_PRINT("AGE: half-unlinked entry=%p Flink=%p Blink=%p\n",
                                       cur, cur->Flink, cur->Blink);
                            VMM_ASSERT(0, "half-unlinked age entry\n");
                            break;
                        }

                        PLIST_ENTRY next = cur->Flink;      /* capture BEFORE removal */
                        pfn_metadata* pfn = GetPfnFromListEntry(cur);
                        PPTE pte = pfn->pte;

                        if (pte == NULL) { cur = next; continue; }

                        /* Snapshot the PTE once; every check + the decision reads
                         * from this one captured value, so the whole decision is
                         * one consistent instant even though the workload can CAS
                         * the age bit without the lock. */
                        ULONG64 ov = *(volatile ULONG64*)pte;
                        PTE s; *(PULONG64)&s = ov;

                        if (pfn->isOccupied != 1 ||
                            get_pte_lock_from_pte_pointer(pte) != &region->lock ||
                            s.hardware.valid != 1) {
                            cur = next;
                            continue;
                        }

                        if (s.hardware.age == 1) {
                            /* Touched since last sweep: clear the ref bit with a
                             * single CAS (shares the bit with the lockless setter;
                             * a plain write could clobber a concurrent set). No
                             * loop: a failed CAS means the PTE changed and doing
                             * nothing is correct -- re-evaluated next sweep. */
                            PTE u = s;
                            u.hardware.age = 0;
                            InterlockedCompareExchange64((volatile LONG64*)pte,
                                                         *(PLONG64)&u, (LONG64)ov);
                        } else if (age < 7) {
                            /* Untouched -> promote toward eviction. */
                            RemoveEntryList(&pfn->list);
                            InsertTailList(&region->active_age_lists[age + 1], &pfn->list);
                        }

                        cur = next;
                    }
                }

                /* ---- Bin 0: promote-only, COUNT-bounded. We snapshot how many
                 * entries bin 0 holds pre-sweep and process at most that many via
                 * per-step `next` capture. This deliberately avoids a saved
                 * stop-POINTER into a list we mutate: promoting a node out of bin
                 * 0 removes it (nulls its links), and if that node had been the
                 * saved boundary, the walk would follow a nulled link. Nothing is
                 * promoted INTO bin 0 during this walk, so the count only shrinks
                 * and we visit exactly the original set, once each. ---- */
                {
                    ULONG64 bin0_count = 0;
                    for (PLIST_ENTRY p = region->active_age_lists[0].Flink;
                         p != &region->active_age_lists[0];
                         p = p->Flink) {
                        if (++bin0_count > NUMBER_OF_PHYSICAL_PAGES) {
                            DIAG_PRINT("AGE bin0 count cycle: region=%p\n", region);
                            VMM_ASSERT(0, "bin0 count cycle\n");
                            break;
                        }
                    }

                    PLIST_ENTRY bin0_head = &region->active_age_lists[0];
                    PLIST_ENTRY cur = bin0_head->Flink;
                    ULONG64 done = 0;

                    while (cur != bin0_head && done < bin0_count) {

                        if (cur->Flink == NULL || cur->Blink == NULL || cur->Flink == cur) {
                            DIAG_PRINT("corrupt bin0 link: cur=%p Flink=%p Blink=%p\n",
                                       cur, cur->Flink, cur->Blink);
                            VMM_ASSERT(0, "corrupt bin0 link\n");
                            break;
                        }

                        PLIST_ENTRY next = cur->Flink;      /* capture BEFORE removal */
                        pfn_metadata* pfn = GetPfnFromListEntry(cur);
                        PPTE pte = pfn->pte;

                        if (pte != NULL) {
                            ULONG64 ov = *(volatile ULONG64*)pte;
                            PTE s; *(PULONG64)&s = ov;

                            if (pfn->isOccupied == 1 &&
                                get_pte_lock_from_pte_pointer(pte) == &region->lock &&
                                s.hardware.valid == 1) {

                                if (s.hardware.age == 1) {
                                    /* Touched; clear ref bit, keep in bin 0. */
                                    PTE u = s;
                                    u.hardware.age = 0;
                                    InterlockedCompareExchange64((volatile LONG64*)pte,
                                                                 *(PLONG64)&u, (LONG64)ov);
                                } else {
                                    /* Untouched -> promote out of bin 0 to bin 1. */
                                    RemoveEntryList(&pfn->list);
                                    InsertTailList(&region->active_age_lists[1], &pfn->list);
                                }
                            }
                        }

                        cur = next;
                        done++;
                    }
                }

                LeaveCriticalSection(&region->lock);
            }

            clock_hand = (clock_hand + 1) % num_pte_regions;
            processed++;
        }

        if (wait == WAIT_OBJECT_0) {
            SetEvent(FinishedAgingEvent);
        }
    }
    return 0;
}


/* ==========================================================================
 *  TRIM WORKER
 *  On LowPagesEvent: run a full aging sweep (and WAIT for it), then harvest
 *  victims until we're back above HIGH_WATERMARK or there's nothing to take.
 * ======================================================================== */

DWORD WINAPI TrimThreadWorker(LPVOID lpParam) {
    HANDLE waitEvents[2] = { LowPagesEvent, ShutdownEvent };
    g_trim_target = HIGH_WATERMARK;   /* start at your old fixed value */
    static LONG64 g_last_stall_snapshot = 0;

    while (TRUE) {
        DWORD w = WaitForMultipleObjects(2, waitEvents, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 + 1) break;

        /* --- CONTROLLER: adjust target based on stalls since last wake --- */
        LONG64 now    = g_fault_stalls;
        LONG64 stalls = now - g_last_stall_snapshot;
        g_last_stall_snapshot = now;

        if (stalls > STALL_HIGH) {
            /* faulting threads are starving -> trim more aggressively */
            g_trim_target += TRIM_TARGET_STEP;
            if (g_trim_target > TRIM_TARGET_MAX) g_trim_target = TRIM_TARGET_MAX;
        } else if (stalls < STALL_LOW) {
            /* trimmer is ahead -> back off so we stop over-evicting hot pages */
            if (g_trim_target > TRIM_TARGET_MIN + TRIM_TARGET_STEP)
                g_trim_target -= TRIM_TARGET_STEP;
            else
                g_trim_target = TRIM_TARGET_MIN;
        }
        //
        // printf("[trim] stalls=%lld target=%llu free=%lld standby=%lld\n",
        //        stalls, g_trim_target, pfn_free_list.count, pfn_standby_list.count);
        /* between STALL_LOW and STALL_HIGH: dead zone, change nothing */

        SetEvent(StartAgingEvent);
        WaitForSingleObject(FinishedAgingEvent, 100);

        /* harvest toward the DYNAMIC target instead of the fixed watermark */
        while (pfn_free_list.count + standby_total_count() < g_trim_target) {
            if (WaitForSingleObject(ShutdownEvent, 0) == WAIT_OBJECT_0) return 0;

            if (pfn_modified_list.count >= MAX_IN_FLIGHT) {
                SetEvent(PagesReadyForDiscEvent);
                Sleep(0);
                continue;
            }

            int batch_count = 0;
            get_unmap_candidates(&batch_count, MAX_TRIM_PAGES);
            if (batch_count == 0) break;
            SetEvent(PagesReadyForDiscEvent);
        }
    }
    return 0;
}

/* ==========================================================================
 *  DISC WORKER
 *  Phase 1: claim modified pages, allocate disc slots.
 *  Phase 2: one batched write to disc.
 *  Phase 3: publish survivors to standby in ONE splice (was per-frame insert).
 * ======================================================================== */
DWORD WINAPI
DiscThreadWorker(LPVOID lpParam) {
    UNREFERENCED_PARAMETER(lpParam);
    HANDLE waitEvents[2] = { PagesReadyForDiscEvent, ShutdownEvent };

    while (TRUE) {
        DWORD waitResult = WaitForMultipleObjects(2, waitEvents, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0 + 1) break;

        if (waitResult == WAIT_OBJECT_0) {
            while (TRUE) {
                if (WaitForSingleObject(ShutdownEvent, 0) == WAIT_OBJECT_0) return 0;

                pfn_metadata* batch[DISC_BATCH];
                int batch_count = 0;

                /* ---- PHASE 1: harvest + allocate ---- */
                while (batch_count < DISC_BATCH) {
                    PLIST_ENTRY mod_entry = LockedRemoveHead(&pfn_modified_list);
                    if (mod_entry == NULL) break;

                    pfn_metadata* c = GetPfnFromListEntry(mod_entry);
                    lock_pfn(c);

                    if (c->isOccupied != 2) {          /* soft-faulted in the pop gap */
                        unlock_pfn(c);
                        continue;
                    }
                    c->isBeingWrittenToDisc = 1;

                    if (c->disc_index == INVALID_DISC_SLOT) {
                        c->disc_index = find_free_disc_slot();
                        if (c->disc_index == INVALID_DISC_SLOT) {
                            c->isBeingWrittenToDisc = 0;
                            unlock_pfn(c);
                            LockedInsertTail(&pfn_modified_list, &c->list);
                            break;
                        }
                    }
                    unlock_pfn(c);
                    batch[batch_count++] = c;
                }

                if (batch_count == 0) break;

                /* ---- PHASE 2: batched write ---- */
                write_to_disc(batch, batch_count);

                /* ---- PHASE 3: aftermath. Publish survivors to standby ATOMICALLY (occ=3 and
                 *      the standby insert together under page+region lock), so no soft fault
                 *      can catch a frame in an occ=3 / not-on-list gap. ---- */
                BOOL any_to_standby = FALSE;

                for (int i = 0; i < batch_count; i++) {
                    pfn_metadata* c = batch[i];
                    ULONG64 slot_to_free = INVALID_DISC_SLOT;

                    lock_pfn(c);
                    PPTE evict_pte = c->pte;
                    unlock_pfn(c);

                    if (evict_pte == NULL) {
                        lock_pfn(c);
                        c->isBeingWrittenToDisc = 0;
                        slot_to_free  = c->disc_index;
                        c->disc_index = INVALID_DISC_SLOT;
                        unlock_pfn(c);
                        goto free_slot;
                    }

                    CRITICAL_SECTION* region_lock = get_pte_lock_from_pte_pointer(evict_pte);
                    EnterCriticalSection(region_lock);
                    lock_pfn(c);

                    /* Poach detection: a soft fault that rescued this frame cleared
                     * isBeingWrittenToDisc and/or set occ != 2. Either means back off. */
                    BOOL poached = (c->isBeingWrittenToDisc == 0) || (c->isOccupied != 2);
                    c->isBeingWrittenToDisc = 0;

                    if (poached || c->pte != evict_pte) {
                        slot_to_free  = c->disc_index;
                        c->disc_index = INVALID_DISC_SLOT;
                        unlock_pfn(c);
                        LeaveCriticalSection(region_lock);
                        goto free_slot;
                    }

                    /* Not poached: publish to standby ATOMICALLY under page+region lock.
                     * occ=3 and list membership are set together -- no gap. The standby list
                     * lock nests correctly inside region->page (order region -> page -> list). */
                    c->isOccupied = 3;
                    int sh = standby_shard_of(c->frame_number);
                    LockedInsertTail(&pfn_standby_shards[sh], &c->list);
                    any_to_standby = TRUE;

                    unlock_pfn(c);
                    LeaveCriticalSection(region_lock);
                    continue;

                free_slot:
                    if (slot_to_free != INVALID_DISC_SLOT) {
                        free_disc_slot(slot_to_free);
                    }
                }

                /* one wake for the whole batch */
                if (any_to_standby) {
                    SetEvent(StandbyPageAvailableEvent);
                }
            }
        }
    }
    return 0;
}


/* ==========================================================================
 *  TRIM HARVESTERS  (file-local)
 * ======================================================================== */

/* Collect harvestable pages from one bin into the caller's arrays.
 * Does NOT unmap or stamp -- caller does that once per region, under the
 * region lock. Caller must hold region->lock. Returns with *n advanced. */
static VOID
collect_from_bin(PPTE_REGION region, PLIST_ENTRY head,
                 pfn_metadata** candidates, PVOID* unmap_vas,
                 ULONG64* saved_frames, int* n, int batch_size) {
    while (!IsListEmpty(head) && *n < batch_size) {
        PLIST_ENTRY entry = RemoveHeadList(head);
        entry->Flink = NULL; entry->Blink = NULL;
        pfn_metadata* candidate = GetPfnFromListEntry(entry);

        lock_pfn(candidate);                              /* read state consistently */
        PPTE evict_pte = candidate->pte;
        BOOL harvestable = (evict_pte != NULL &&
                            evict_pte->hardware.valid == 1 &&
                            candidate->isOccupied == 1 &&
                            candidate->isBeingWrittenToDisc == 0 &&      /* NOT in disc pipeline */
                            get_pte_lock_from_pte_pointer(evict_pte) == &region->lock);
        unlock_pfn(candidate);

        if (!harvestable) {
            continue;   /* already unlinked; drop -- disc worker or someone else owns it */
        }

        /* only harvest genuinely active pages owned by THIS region */
        if (evict_pte == NULL ||
            evict_pte->hardware.valid != 1 ||
            candidate->isOccupied != 1 ||
            get_pte_lock_from_pte_pointer(evict_pte) != &region->lock) {
            printf("HARVEST STALE: pfn=%p occ=%d pte=%p\n",
                   candidate, (int)candidate->isOccupied, evict_pte);
            DebugBreak();
            continue;   /* already unlinked; drop it */
            }

        candidates[*n]   = candidate;
        unmap_vas[*n]    = get_va_from_pte(evict_pte);
        saved_frames[*n] = candidate->frame_number;
        (*n)++;
    }
}

static VOID
get_unmap_candidates(int* batch_count, int batch_size) {
    static ULONG64 current_trim_region = 0;
    ULONG64 regions_checked = 0;
    *batch_count = 0;

    /* ---- PASS 1: bins 7..1, per-region batched harvest + unmap ---- */
    while (*batch_count < batch_size && regions_checked < num_pte_regions) {
        PPTE_REGION region = &pte_regions[current_trim_region];

        if (TryEnterCriticalSection(&region->lock)) {

            pfn_metadata* candidates[MAX_TRIM_PAGES];
            PVOID         unmap_vas[MAX_TRIM_PAGES];
            ULONG64       saved_frames[MAX_TRIM_PAGES];
            int           n = 0;

            for (int age = 7; age >= 1 && n < batch_size; age--) {
                collect_from_bin(region, &region->active_age_lists[age],
                                 candidates, unmap_vas, saved_frames, &n, batch_size);
            }

            if (n > 0) {
                if (MapUserPhysicalPagesScatter(unmap_vas, n, NULL) == FALSE) {
                    DIAG_PRINT("CRITICAL: batch unmap failed n=%d err=%lu\n", n, GetLastError());
                    VMM_ASSERT(0, "batch unmap failed\n");
                    /* mappings still live -- put pages back on bin 0, stamp nothing */
                    for (int i = 0; i < n; i++) {
                        lock_pfn(candidates[i]);
                        InsertTailList(&region->active_age_lists[0], &candidates[i]->list);
                        unlock_pfn(candidates[i]);
                    }
                    n = 0;
                }
                DIAG_ADD(g_trim_unmaps, n);

                /* stamp PTEs to TRANSITION + publish to modified list */
                for (int i = 0; i < n; i++) {
                    pfn_metadata* c = candidates[i];

                    lock_pfn(c);
                    /* Recheck under the page lock: collect_from_bin checked
                     * bwd/occ earlier then RELEASED the lock. The disc worker or
                     * a rescue may have claimed the frame in that window. If so,
                     * it's off the age list already -- don't stamp, don't insert. */
                    if (c->isBeingWrittenToDisc != 0 || c->isOccupied != 1) {
                        unlock_pfn(c);
                        continue;
                    }

                    /* stamp the PTE to transition (frame still ours) */
                    PPTE evict_pte = c->pte;
                    *(PULONG64)evict_pte = 0;
                    evict_pte->transition.valid        = 0;
                    evict_pte->transition.transition   = 1;
                    evict_pte->transition.frame_number = saved_frames[i];

                    /* real double-insert guard: BEFORE the insert, Flink must be NULL */
                    VMM_ASSERT(c->list.Flink == NULL && c->list.Blink == NULL,
                               "modified insert: frame ALREADY linked! pfn=%p Flink=%p occ=%d bwd=%d\n",
                               c, c->list.Flink, (int)c->isOccupied, (int)c->isBeingWrittenToDisc);

                    /* occ=2 and the insert are ATOMIC under the page lock */
                    c->isOccupied = 2;
                    LockedInsertTail(&pfn_modified_list, &c->list);
                    unlock_pfn(c);
                }

                *batch_count += n;
            }
            LeaveCriticalSection(&region->lock);
        }

        current_trim_region = (current_trim_region + 1) % num_pte_regions;
        regions_checked++;
    }

    /* ---- PASS 2 (last resort): bin 0, only if bins 7..1 yielded nothing ---- */
    if (*batch_count == 0) {
        regions_checked = 0;
        while (*batch_count < batch_size && regions_checked < num_pte_regions) {
            PPTE_REGION region = &pte_regions[current_trim_region];

            if (TryEnterCriticalSection(&region->lock)) {

                pfn_metadata* candidates[MAX_TRIM_PAGES];
                PVOID         unmap_vas[MAX_TRIM_PAGES];
                ULONG64       saved_frames[MAX_TRIM_PAGES];
                int           n = 0;

                collect_from_bin(region, &region->active_age_lists[0],
                                 candidates, unmap_vas, saved_frames, &n, batch_size);

                if (n > 0) {
                    if (MapUserPhysicalPagesScatter(unmap_vas, n, NULL) == FALSE) {
                        DIAG_PRINT("CRITICAL: batch unmap failed (bin0) n=%d err=%lu\n", n, GetLastError());
                        VMM_ASSERT(0, "batch unmap failed bin0\n");
                        for (int i = 0; i < n; i++) {
                            lock_pfn(candidates[i]);
                            InsertTailList(&region->active_age_lists[0], &candidates[i]->list);
                            unlock_pfn(candidates[i]);
                        }
                        n = 0;
                    }
                    DIAG_ADD(g_trim_unmaps, n);

                    for (int i = 0; i < n; i++) {
                        pfn_metadata* c = candidates[i];

                        lock_pfn(c);
                        if (c->isBeingWrittenToDisc != 0 || c->isOccupied != 1) {
                            unlock_pfn(c);
                            continue;
                        }

                        PPTE evict_pte = c->pte;
                        *(PULONG64)evict_pte = 0;
                        evict_pte->transition.valid        = 0;
                        evict_pte->transition.transition   = 1;
                        evict_pte->transition.frame_number = saved_frames[i];

                        VMM_ASSERT(c->list.Flink == NULL && c->list.Blink == NULL,
                                   "modified insert (bin0): frame ALREADY linked! pfn=%p Flink=%p occ=%d bwd=%d\n",
                                   c, c->list.Flink, (int)c->isOccupied, (int)c->isBeingWrittenToDisc);

                        c->isOccupied = 2;
                        LockedInsertTail(&pfn_modified_list, &c->list);
                        unlock_pfn(c);
                    }

                    *batch_count += n;
                }
                LeaveCriticalSection(&region->lock);
            }

            current_trim_region = (current_trim_region + 1) % num_pte_regions;
            regions_checked++;
        }
    }
}


/* ==========================================================================
 *  ZERO WORKER
 *  Bulk-grab cold standby frames (one acquisition), stamp owners to disc
 *  OUTSIDE the standby lock, batch-map + memset + batch-unmap, splice the
 *  zeroed frames onto pfn_zeroed_list in one acquisition.
 * ======================================================================== */

/* Remove up to `want` coldest standby frames in ONE acquisition. Owners are
 * snapshotted; the caller stamps them to disc outside the standby lock. */
static int
grab_cold_standby_batch(pfn_metadata** out, PPTE* owners, int want) {
    int n = 0;
    /* round-robin the shards so we don't hammer shard 0 and so we spread
     * the frames we steal across shards */
    for (int sh = 0; sh < STANDBY_SHARDS && n < want; sh++) {
        LOCKED_LIST* s = &pfn_standby_shards[sh];
        if (s->head.Flink == &s->head) continue;      /* peek: skip empty */

        EnterCriticalSection(&s->lock);
        PLIST_ENTRY e = s->head.Blink;                 /* tail = coldest */
        while (e != &s->head && n < want) {
            PLIST_ENTRY prev = e->Blink;
            pfn_metadata* p = GetPfnFromListEntry(e);
            if (p->isOccupied == 3 && p->isBeingWrittenToDisc == 0) {
                RemoveEntryList(&p->list);
                p->list.Flink = p->list.Blink = NULL;
                s->count--;
                out[n]    = p;
                owners[n] = p->pte;
                n++;
            }
            e = prev;
        }
        LeaveCriticalSection(&s->lock);
    }
    return n;
}

DWORD WINAPI
ZeroThreadWorker(LPVOID lpParam) {
    UNREFERENCED_PARAMETER(lpParam);
    HANDLE waitEvents[2] = { NeedZeroingEvent, ShutdownEvent };

    while (TRUE) {
        DWORD w = WaitForMultipleObjects(2, waitEvents, FALSE, 50);
        if (w == WAIT_OBJECT_0 + 1) break;

        while (pfn_zeroed_list.count < ZEROED_LIST_HIGH) {
            if (WaitForSingleObject(ShutdownEvent, 0) == WAIT_OBJECT_0) return 0;

            pfn_metadata* src[ZERO_BATCH];
            PPTE          owners[ZERO_BATCH];
            int n = 0;

            /* free list first (cheap, uncontended), per-frame */
            while (n < ZERO_BATCH) {
                PLIST_ENTRY fe = LockedRemoveHead(&pfn_free_list);
                if (fe == NULL) break;
                src[n]    = GetPfnFromListEntry(fe);
                owners[n] = NULL;
                n++;
            }
            /* bulk-grab cold standby for the remainder, ONE acquisition */
            if (n < ZERO_BATCH) {
                n += grab_cold_standby_batch(&src[n], &owners[n], ZERO_BATCH - n);
            }
            if (n == 0) break;

            /* stamp standby owners to disc OUTSIDE the standby lock; compact
             * survivors into zbatch[] */
            pfn_metadata* zbatch[ZERO_BATCH];
            ULONG_PTR     znums[ZERO_BATCH];
            PVOID         zvas[ZERO_BATCH];
            int live = 0;

            for (int i = 0; i < n; i++) {
                if (owners[i] == NULL) {
                    /* from free list -- no owner, genuinely ours */
                    lock_pfn(src[i]);
                    src[i]->disc_index = INVALID_DISC_SLOT;
                    src[i]->isOccupied = 0;
                    unlock_pfn(src[i]);
                    zbatch[live++] = src[i];
                    continue;
                }

                CRITICAL_SECTION* ol = get_pte_lock_from_pte_pointer(owners[i]);
                EnterCriticalSection(ol);
                lock_pfn(src[i]);

                /* Re-verify under region+page lock. A soft fault that rescued this frame
                 * KEEPS the same pte but sets occ 3->1. So check occ, not just pte. The
                 * frame we grabbed from cold standby must still be occ==3 (standby) and
                 * still owned by owners[i] to be safe to repurpose for zeroing. */
                if (src[i]->pte != owners[i] || src[i]->isOccupied != 3) {
                    /* Rescued or changed out from under us. The rescuer now owns this frame
                     * and has it on an age list. DO NOT touch its list membership, DO NOT
                     * return it to free -- just abandon it and drop from our batch. */
                    unlock_pfn(src[i]);
                    LeaveCriticalSection(ol);
                    continue;
                }

                /* Still a viable standby frame owned by owners[i]. Repurpose it: stamp the
                 * old owner to disc, claim the frame for zeroing. */
                set_to_disc_state(owners[i], src[i]);
                src[i]->disc_index = INVALID_DISC_SLOT;
                src[i]->isOccupied = 0;
                unlock_pfn(src[i]);
                src[i]->pte = NULL;
                LeaveCriticalSection(ol);
                zbatch[live++] = src[i];
            }

            if (live == 0) break;

            for (int i = 0; i < live; i++) {
                znums[i] = zbatch[i]->frame_number;
                zvas[i]  = (char*)zero_va_start + ((ULONG_PTR)i * PAGE_SIZE);
            }

            /* ONE scatter map, memset all, ONE scatter unmap */
            if (MapUserPhysicalPagesScatter(zvas, live, znums) == FALSE) {
                DIAG_PRINT("ZERO: batch map failed n=%d err=%lu\n", live, GetLastError());
                for (int i = 0; i < live; i++) {
                    lock_pfn(zbatch[i]);
                    zbatch[i]->isOccupied = 0;
                    zbatch[i]->is_zero    = 0;
                    unlock_pfn(zbatch[i]);
                    zbatch[i]->pte = NULL;
                }
                LockedInsertTailBatch(&pfn_free_list,
                    /* build a PLIST_ENTRY* array... simplest: per-frame here since rare */
                    NULL, 0);
                for (int i = 0; i < live; i++)
                    LockedInsertTail(&pfn_free_list, &zbatch[i]->list);
                break;
            }

            for (int i = 0; i < live; i++) memset(zvas[i], 0, PAGE_SIZE);
            MapUserPhysicalPagesScatter(zvas, live, NULL);

            /* mark zeroed, splice onto zeroed list in ONE acquisition */
            PLIST_ENTRY to_zeroed[ZERO_BATCH];
            for (int i = 0; i < live; i++) {
                lock_pfn(zbatch[i]);
                zbatch[i]->is_zero    = 1;
                zbatch[i]->isOccupied = 0;
                zbatch[i]->disc_index = INVALID_DISC_SLOT;
                unlock_pfn(zbatch[i]);
                zbatch[i]->pte = NULL;
                to_zeroed[i] = &zbatch[i]->list;
            }
            LockedInsertTailBatch(&pfn_zeroed_list, to_zeroed, live);
            DIAG_ADD(g_frames_zeroed, live);
        }
    }
    return 0;
}