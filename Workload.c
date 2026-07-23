//
// Created by lilyk on 7/14/2026.
//
/* ============================================================================
 *  workload.c  --  the two access-pattern helpers.
 *
 *  full_virtual_memory_test_helper           : uniform random (legacy/compare)
 *  full_virtual_memory_test_helper_not_random: locality (linear runs + hot-spot
 *                                              revisits) -- exercises aging
 *
 *  These run-shape tunables are used only here. If you already define them in
 *  vmm.h, delete this block to avoid redefinition warnings.
 * ========================================================================== */

#include "Vmm.h"
#include <intrin.h>   /* __rdtsc */

#ifndef MIN_RUN_PAGES
#define MIN_RUN_PAGES   16

#endif
#ifndef MAX_RUN_PAGES
#define MAX_RUN_PAGES   512
#endif
#ifndef REVISIT_CHANCE
#define REVISIT_CHANCE  2
#endif
#ifndef HOT_SPOTS
#define HOT_SPOTS       8
#endif

// High-quality XOR shift generator from Noah Persily
ULONG64 GetNextRandom(THREAD_RNG_STATE* rng) {
    ULONG64 x = rng->state;

    // High-quality XOR shift with good statistical properties
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;

    rng->state = x;
    rng->counter++;

    // Occasionally reseed with fresh entropy to maintain non-determinism
    if ((rng->counter & 0xFFFF) == 0) {
        x ^= __rdtsc();  // Mix in fresh entropy periodically
        rng->state = x;
    }

    return x;
}


VOID
full_virtual_memory_test_helper(int thread_number) {

    THREAD_RNG_STATE thread_rng;
    thread_rng.state = __rdtsc();
    if (thread_rng.state == 0) {
        thread_rng.state = 1;
    }
    thread_rng.counter = 0;

    LARGE_INTEGER frequency, start_time, end_time;
    double elapsed_ms;

    QueryPerformanceFrequency(&frequency);
    printf("Starting virtual memory simulation workload...for thread %i\n", thread_number);
    QueryPerformanceCounter(&start_time);

    unsigned i = 0;
    PVOID   arbitrary_va;
    ULONG64 random_number;
    BOOL    page_faulted = TRUE;
    BOOL    resolved     = TRUE;
    ULONG64 runtime = (1 * (MB(1) / 1));

    while (i < runtime) {

        if (resolved) {
            random_number  = GetNextRandom(&thread_rng);
            random_number %= virtual_address_size_in_unsigned_chunks;
            random_number &= ~0x7ULL;
            arbitrary_va = (PVOID)((ULONG_PTR)va_start + random_number);
            resolved = FALSE;
        }

        page_faulted = FALSE;

        __try {
            *(PULONG_PTR)arbitrary_va = (ULONG_PTR)arbitrary_va;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            page_faulted = TRUE;
        }

        if (page_faulted) {
            handle_page_fault(arbitrary_va);
            continue;
        }
        else {
            PPTE pte = get_pte_from_va(arbitrary_va);

            // Ulong64 so we can actually pass it into interlocked
            ULONG64 old_val = *(ULONG64*)pte;

            // But we need the pte version so we can read it using the fields
            PTE snapshot;
            *(PULONG64)&snapshot = old_val;

            // Only do this if it is valid and nobody has increased the age yet
            if (snapshot.hardware.valid == 1 && snapshot.hardware.age == 0) {
                PTE updated = snapshot;
                updated.hardware.age = 1;
                InterlockedCompareExchange64(( LONG64*)pte,
                                             *(PLONG64)&updated,
                                             (LONG64)old_val);
            }
        }

        if (i > 0 && i % (runtime / 100) == 0) {
            DIAG_PRINT(".");
            fflush(stdout);
        }

        resolved = TRUE;
        i++;
    }

    QueryPerformanceCounter(&end_time);
    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("==============================================\n\n");
    printf("full_virtual_memory_test : %d finished accessing %u random virtual addresses.\n",
           thread_number, i);
}

/* One remembered hot spot: base AND length, so a revisit re-touches the
 * SAME pages. Revisiting a base with a fresh random length mostly touches
 * new pages, which fault hard -- that was the old behavior. */
typedef struct _HOT_SPOT {
    ULONG64 base;
    ULONG64 len;
} HOT_SPOT;

VOID
full_virtual_memory_test_helper_not_random(int thread_number) {
    t_thread_number = thread_number;
    t_ring_pos      = 0;

    THREAD_RNG_STATE thread_rng;
    thread_rng.state = __rdtsc();
    if (thread_rng.state == 0) thread_rng.state = 1;
    thread_rng.counter = 0;

    LARGE_INTEGER frequency, start_time, end_time;
    double elapsed_ms;

    QueryPerformanceFrequency(&frequency);
    printf("Starting virtual memory simulation workload...for thread %i\n", thread_number);
    QueryPerformanceCounter(&start_time);

    ULONG64 total_pages = virtual_address_size_in_unsigned_chunks
                          / (PAGE_SIZE / sizeof(ULONG_PTR));

    /* Confine cold jumps to a bounded span. This is the single biggest lever
     * on the soft/hard split: with the full span the working set is many times
     * physical, so every trimmed page is gone to disc before you return to it.
     * A tighter span means trimmed pages are still on standby when revisited. */
    ULONG64 cold_span = total_pages / WORKING_SET_DIVISOR;
    if (cold_span == 0) cold_span = total_pages;

    /* Give each thread its own slice of the span so the 8 workers aren't all
     * hammering the same pages (which would serialize on region locks) but
     * still overlap enough to share standby pressure. */
    ULONG64 slice     = cold_span / (NUM_WORKER_THREADS + 1);
    ULONG64 slice_lo  = (ULONG64)thread_number * slice;
    if (slice == 0) { slice = cold_span; slice_lo = 0; }

    HOT_SPOT hot[HOT_SPOTS];
    for (int h = 0; h < HOT_SPOTS; h++) { hot[h].base = 0; hot[h].len = 0; }
    int hot_next  = 0;
    int hot_count = 0;

    ULONG64 base_page = slice_lo + (GetNextRandom(&thread_rng) % slice);
    ULONG64 run_left  = 0;
    ULONG64 cur_page  = base_page;

    unsigned i = 0;
    PVOID arbitrary_va;
    BOOL  page_faulted;
    BOOL  resolved = TRUE;

    ULONG64 runtime = (1 * (MB(1) / 1));

    while (i < runtime) {

        if (resolved) {
            if (run_left == 0) {
                ULONG64 r = GetNextRandom(&thread_rng);

                if (hot_count > 0 && (r % REVISIT_CHANCE) == 0) {
                    /* REVISIT: re-touch the SAME pages we touched before, so
                     * they're either still valid (no fault) or on
                     * modified/standby (SOFT fault) -- not fresh pages that
                     * would fault hard. */
                    int pick;
                    if ((GetNextRandom(&thread_rng) % RECENT_BIAS) == 0) {
                        /* newest few spots: most likely still on standby */
                        int recent = (hot_count < 4) ? hot_count : 4;
                        int back   = 1 + (int)(GetNextRandom(&thread_rng) % recent);
                        pick = (hot_next - back + HOT_SPOTS) % HOT_SPOTS;
                    } else {
                        pick = (int)(GetNextRandom(&thread_rng) % hot_count);
                    }
                    base_page = hot[pick].base;
                    run_left  = hot[pick].len;      /* same span as before */

                    /* Re-stamp this spot as most-recent so repeated revisits
                     * keep a small set genuinely hot. */
                    hot[hot_next].base = base_page;
                    hot[hot_next].len  = run_left;
                    hot_next = (hot_next + 1) % HOT_SPOTS;
                    if (hot_count < HOT_SPOTS) hot_count++;

                } else {
                    /* COLD JUMP: new location, but inside this thread's slice
                     * of the bounded working set. */
                    base_page = slice_lo + (GetNextRandom(&thread_rng) % slice);
                    run_left  = MIN_RUN_PAGES +
                                (GetNextRandom(&thread_rng) % (MAX_RUN_PAGES - MIN_RUN_PAGES));

                    hot[hot_next].base = base_page;
                    hot[hot_next].len  = run_left;
                    hot_next = (hot_next + 1) % HOT_SPOTS;
                    if (hot_count < HOT_SPOTS) hot_count++;
                }

                /* clamp to the VA end */
                if (base_page >= total_pages) base_page = 0;
                if (base_page + run_left > total_pages) {
                    run_left = total_pages - base_page;
                    if (run_left == 0) { base_page = 0; run_left = MIN_RUN_PAGES; }
                }

                cur_page = base_page;
            }

            arbitrary_va = (PVOID)((ULONG_PTR)va_start + (cur_page * PAGE_SIZE));
            resolved = FALSE;
        }

        page_faulted = FALSE;

        __try {
            *(PULONG_PTR)arbitrary_va = (ULONG_PTR)arbitrary_va;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            page_faulted = TRUE;
        }

        if (page_faulted) {
            handle_page_fault(arbitrary_va);
            continue;   /* retry the SAME va -- do not advance */
        }

        {
            PPTE pte = get_pte_from_va(arbitrary_va);
            ULONG64 old_val = *(volatile ULONG64*)pte;
            PTE snapshot;
            *(PULONG64)&snapshot = old_val;

            if (snapshot.hardware.valid == 1 && snapshot.hardware.age == 0) {
                PTE updated = snapshot;
                updated.hardware.age = 1;
                InterlockedCompareExchange64((LONG64*)pte,
                                             *(PLONG64)&updated,
                                             (LONG64)old_val);
            }
        }

        cur_page++;
        run_left--;
        resolved = TRUE;

        if (i > 0 && i % (runtime / 100) == 0) {
            DIAG_PRINT(".");
            fflush(stdout);
        }
        i++;
    }

    staging_flush_current_thread();

    QueryPerformanceCounter(&end_time);
    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\nthread %d: %u accesses in %.2f ms\n", thread_number, i, elapsed_ms);
}
