//
// Created by lilyk on 7/14/2026.
//
/* ============================================================================
 *  threads.c  --  the three background workers plus the trim harvesters.
 *
 *  Lock hierarchy in force:  region -> page -> list -> disc
 *
 *  AgeThreadWorker  : sweeps regions, promotes/demotes pages between age bins.
 *  TrimThreadWorker : on low pages, ages then harvests victims to the mod list.
 *  DiscThreadWorker : writes the mod list to disc, publishes frames to standby.
 * ========================================================================== */

#include "Vmm.h"

/* Forward declarations: these are defined below their callers, so they need
 * to be declared first. Both are file-local (static). */
static VOID get_unmap_candidates(int* batch_count, int batch_size);
static VOID harvest_one_bin(PPTE_REGION region, PLIST_ENTRY head,
                            int* batch_count, int batch_size);


/* ==========================================================================
 *  AGE WORKER
 *  Bins 7..1 promote/demote; bin 0 is promote-only over a pre-sweep snapshot
 *  so pages demoted during THIS sweep aren't re-processed and bumped to bin 1.
 * ======================================================================== */
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
                PLIST_ENTRY bin0_head  = &region->active_age_lists[0];
                PLIST_ENTRY bin0_stop  = bin0_head->Blink;   /* last pre-existing entry */
                BOOL bin0_was_empty    = IsListEmpty(bin0_head);

                /* Descending is REQUIRED: we promote into bin (age+1), which we
                 * have already walked. Ascending would re-walk promoted pages
                 * and loop forever. */
                for (int age = 7; age >= 1; age--) {
                    PLIST_ENTRY head = &region->active_age_lists[age];
                    if (IsListEmpty(head)) continue;

                    ULONG64 walked = 0;
                    PLIST_ENTRY cur = head->Flink;

                    while (cur != head) {

                        /* GUARD 1: bounded walk. An age list can never hold more
                         * entries than there are physical frames. Exceeding that
                         * means the links form a cycle that never reaches head. */
                        if (++walked > NUMBER_OF_PHYSICAL_PAGES) {
                            printf("AGE LIST CYCLE: region=%p bin=%d walked=%llu cur=%p\n",
                                   region, age, walked, cur);
                            DebugBreak();
                            break;
                        }

                        /* GUARD 2: self-linked node. cur->Flink == cur means
                         * next == cur, so the walk never advances -- the exact
                         * hang we hit. Never follow it. */
                        if (cur->Flink == cur) {
                            printf("SELF-LINKED PFN: entry=%p Flink=%p Blink=%p\n",
                                   cur, cur->Flink, cur->Blink);
                            DebugBreak();
                            break;
                        }

                        /* GUARD 3: half-unlinked node. The removal helpers null
                         * both links; a node with one NULL link is caught mid-op
                         * or corrupt. Following it dereferences NULL. */
                        if (cur->Flink == NULL || cur->Blink == NULL) {
                            printf("AGE: half-unlinked entry=%p Flink=%p Blink=%p\n",
                                   cur, cur->Flink, cur->Blink);
                            DebugBreak();
                            break;
                        }

                        PLIST_ENTRY next = cur->Flink;
                        pfn_metadata* pfn = GetPfnFromListEntry(cur);
                        PPTE pte = pfn->pte;

                        /* Anything in this region's age list must be active and
                         * owned by this region. If not, it's a state we don't
                         * own -- leave it alone entirely. */
                        if (pfn->isOccupied != 1 || pte == NULL ||
                            get_pte_lock_from_pte_pointer(pte) != &region->lock ||
                            pte->hardware.valid != 1) {
                            cur = next;
                            continue;
                        }

                        if (pte->hardware.age == 1) {
                            pte->hardware.age = 0;
                        } else if (age < 7) {
                            RemoveEntryList(&pfn->list);
                            InsertTailList(&region->active_age_lists[age + 1], &pfn->list);
                        }

                        cur = next;
                    }
                }

                /* Bin 0: promote-only, bounded by the pre-sweep snapshot. */
                if (!bin0_was_empty) {
                    PLIST_ENTRY cur = bin0_head->Flink;
                    ULONG64 walked = 0;
                    for (;;) {
                        if (cur == bin0_head) break;
                        if (++walked > NUMBER_OF_PHYSICAL_PAGES) { DebugBreak(); break; }
                        if (cur->Flink == NULL || cur->Blink == NULL || cur->Flink == cur) { DebugBreak(); break; }

                        PLIST_ENTRY next    = cur->Flink;
                        BOOL        is_last = (cur == bin0_stop);   /* check BEFORE we move it */

                        pfn_metadata* pfn = GetPfnFromListEntry(cur);
                        PPTE pte = pfn->pte;

                        if (pfn->isOccupied == 1 && pte != NULL &&
                            get_pte_lock_from_pte_pointer(pte) == &region->lock &&
                            pte->hardware.valid == 1) {

                            if (pte->hardware.age == 1) {
                                pte->hardware.age = 0;              /* touched; stays in bin 0 */
                            } else {
                                RemoveEntryList(&pfn->list);
                                InsertTailList(&region->active_age_lists[1], &pfn->list);
                            }
                        }

                        if (is_last) break;                        /* snapshot boundary */
                        cur = next;
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
DWORD WINAPI
TrimThreadWorker(LPVOID lpParam) {
    UNREFERENCED_PARAMETER(lpParam);
    HANDLE waitEvents[2] = { LowPagesEvent, ShutdownEvent };

    while (TRUE) {
        DWORD waitResult = WaitForMultipleObjects(2, waitEvents, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0 + 1) break;   /* Shutdown */

        SetEvent(StartAgingEvent);
        WaitForSingleObject(FinishedAgingEvent, 100);

        while (pfn_free_list.count + pfn_standby_list.count < HIGH_WATERMARK) {
            if (WaitForSingleObject(ShutdownEvent, 0) == WAIT_OBJECT_0) return 0;

            /* Don't outrun the disc thread -- but don't stop working either. */
            if (pfn_modified_list.count >= MAX_IN_FLIGHT) {
                SetEvent(PagesReadyForDiscEvent);
                Sleep(0);                 /* yield; let disc drain */
                continue;
            }

            int batch_count = 0;
            get_unmap_candidates(&batch_count, MAX_TRIM_PAGES);

            if (batch_count == 0) {
                break;   /* genuinely nothing harvestable; retry on next wake */
            }

            SetEvent(PagesReadyForDiscEvent);
        }
    }
    return 0;
}


/* ==========================================================================
 *  DISC WORKER
 *  Phase 1: claim pages off the modified list (pop -> lock -> re-check occ==2).
 *  Phase 2: one batched write to disc.
 *  Phase 3: publish to standby, or reclaim the slot if the page was poached.
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

                /* ---- PHASE 1: harvest and allocate ---- */
                while (batch_count < DISC_BATCH) {
                    PLIST_ENTRY mod_entry = LockedRemoveHead(&pfn_modified_list);
                    if (mod_entry == NULL) break;

                    pfn_metadata* c = GetPfnFromListEntry(mod_entry);
                    lock_pfn(c);

                    if (c->isOccupied != 2) {          /* soft fault rescued it in the pop gap */
                        unlock_pfn(c);
                        continue;
                    }

                    c->isBeingWrittenToDisc = 1;

                    if (c->disc_index == INVALID_DISC_SLOT) {
                        c->disc_index = find_free_disc_slot();
                        if (c->disc_index == INVALID_DISC_SLOT) {
                            c->isBeingWrittenToDisc = 0;
                            unlock_pfn(c);
                            LockedInsertTail(&pfn_modified_list, &c->list);   /* PUT IT BACK */
                            break;
                        }
                    }

                    unlock_pfn(c);
                    batch[batch_count++] = c;
                }

                if (batch_count == 0) {
                    break;
                }

                /* ---- PHASE 2: the I/O gap ---- */
                write_to_disc(batch, batch_count);

                /* ---- PHASE 3: the aftermath ---- */
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

                    BOOL poached = (c->isBeingWrittenToDisc == 0);
                    c->isBeingWrittenToDisc = 0;

                    if (poached || c->pte != evict_pte) {
                        slot_to_free  = c->disc_index;
                        c->disc_index = INVALID_DISC_SLOT;
                        unlock_pfn(c);
                        LeaveCriticalSection(region_lock);
                        goto free_slot;
                    }

                    c->isOccupied = 3;
                    LockedInsertTail(&pfn_standby_list, &c->list);
                    SetEvent(StandbyPageAvailableEvent);

                    unlock_pfn(c);
                    LeaveCriticalSection(region_lock);
                    continue;

                free_slot:
                    if (slot_to_free != INVALID_DISC_SLOT) {
                        EnterCriticalSection(&disc_lock);
                        disc_metadata[slot_to_free].isOccupied = FALSE;
                        LeaveCriticalSection(&disc_lock);
                        LockedInsertTail(&disc_free_list, &disc_metadata[slot_to_free].list);
                    }
                }
            }
        }
    }
    return 0;
}


/* ==========================================================================
 *  TRIM HARVESTERS  (file-local)
 * ======================================================================== */

static VOID
get_unmap_candidates(int* batch_count, int batch_size) {
    static ULONG64 current_trim_region = 0;
    ULONG64 regions_checked = 0;

    /* Pass 1: bins 7..1 across the sweep. */
    while (*batch_count < batch_size && regions_checked < num_pte_regions) {
        PPTE_REGION region = &pte_regions[current_trim_region];

        if (TryEnterCriticalSection(&region->lock)) {
            for (int age = 7; age >= 1 && *batch_count < batch_size; age--) {
                harvest_one_bin(region, &region->active_age_lists[age],
                                batch_count, batch_size);
            }
            LeaveCriticalSection(&region->lock);
        }

        current_trim_region = (current_trim_region + 1) % num_pte_regions;
        regions_checked++;
    }

    /* Pass 2 (last resort): the full sweep found NOTHING in bins 7..1. Take
     * bin 0 from any region we can lock, rather than starving. If this fires
     * often, aging isn't keeping up -- raise the age SWEEP size first. */
    if (*batch_count == 0) {
        regions_checked = 0;
        while (*batch_count < batch_size && regions_checked < num_pte_regions) {
            PPTE_REGION region = &pte_regions[current_trim_region];

            if (TryEnterCriticalSection(&region->lock)) {
                harvest_one_bin(region, &region->active_age_lists[0],
                                batch_count, batch_size);
                LeaveCriticalSection(&region->lock);
            }

            current_trim_region = (current_trim_region + 1) % num_pte_regions;
            regions_checked++;
        }
    }
}

/* Caller must hold region->lock. */
static VOID
harvest_one_bin(PPTE_REGION region, PLIST_ENTRY head,
                int* batch_count, int batch_size) {
    UNREFERENCED_PARAMETER(region);

    pfn_metadata* candidates[MAX_TRIM_PAGES];
    PVOID         unmap_vas[MAX_TRIM_PAGES];
    ULONG64       saved_frames[MAX_TRIM_PAGES];
    int           n = 0;

    /* ---- PASS 1: collect. Do NOT touch PTEs or unmap yet. ---- */
    while (!IsListEmpty(head) && (*batch_count + n) < batch_size && n < MAX_TRIM_PAGES) {
        PLIST_ENTRY entry = RemoveHeadList(head);
        entry->Flink = NULL;          /* keep the unlinked-marker discipline */
        entry->Blink = NULL;

        pfn_metadata* candidate = GetPfnFromListEntry(entry);
        PPTE evict_pte = candidate->pte;

        /* Guard: only harvest genuinely active pages with valid PTEs. */
        if (evict_pte == NULL ||
            evict_pte->hardware.valid != 1 ||
            candidate->isOccupied != 1) {
            printf("HARVEST STALE: pfn=%p occ=%d pte=%p valid=%d frame=0x%llx\n",
                   candidate, (int)candidate->isOccupied, evict_pte,
                   evict_pte ? (int)evict_pte->hardware.valid : -1,
                   (ULONG64)candidate->frame_number);
            DebugBreak();
            continue;   /* already unlinked; drop it */
        }

        candidates[n]   = candidate;
        unmap_vas[n]    = get_va_from_pte(evict_pte);
        saved_frames[n] = candidate->frame_number;
        n++;
    }

    if (n == 0) {
        return;
    }

    /* ---- PASS 2: ONE batched unmap. All mappings die here, together.
     * Safe: every VA belongs to THIS region, whose lock we hold, so no
     * faulting thread can be mid-access on any of them. ---- */
    if (MapUserPhysicalPagesScatter(unmap_vas, n, NULL) == FALSE) {
        printf("CRITICAL: Batch unmap failed. n=%d Error: %lu\n", n, GetLastError());
        DebugBreak();
        /* Do NOT stamp PTEs if the unmap failed -- the mappings are still live.
         * Put the pages back on the age list so we don't leak them. */
        for (int i = 0; i < n; i++) {
            InsertTailList(head, &candidates[i]->list);
        }
        return;
    }

    /* ---- PASS 3: nothing is mapped now -- stamp PTEs and publish. ---- */
    for (int i = 0; i < n; i++) {
        pfn_metadata* candidate = candidates[i];
        PPTE evict_pte = candidate->pte;

        *(PULONG64)evict_pte = 0;
        evict_pte->transition.valid        = 0;
        evict_pte->transition.transition   = 1;
        evict_pte->transition.frame_number = saved_frames[i];

        lock_pfn(candidate);
        candidate->isOccupied = 2;        /* set BEFORE the insert */
        unlock_pfn(candidate);
        LockedInsertTail(&pfn_modified_list, &candidate->list);
    }

    *batch_count += n;
}