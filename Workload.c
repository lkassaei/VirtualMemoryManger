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
#define REVISIT_CHANCE  4
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

            /* Age hint via CAS: a plain bitfield write is a full-qword RMW that
             * could resurrect a stale VALID pte over a concurrent trim's
             * TRANSITION stamp. CAS only succeeds if the PTE is still exactly
             * the valid value we read; otherwise skip -- it's just a hint. */
            ULONG64 old_val = *(volatile ULONG64*)pte;

            PTE snapshot;
            *(PULONG64)&snapshot = old_val;

            if (snapshot.hardware.valid == 1 && snapshot.hardware.age == 0) {
                PTE updated = snapshot;
                updated.hardware.age = 1;
                InterlockedCompareExchange64((volatile LONG64*)pte,
                                             *(PLONG64)&updated,
                                             (LONG64)old_val);
            }
        }

        if (i > 0 && i % (runtime / 100) == 0) {
            printf(".");
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


VOID
full_virtual_memory_test_helper_not_random(int thread_number) {

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

    /* Ring of recently-visited bases. Revisiting these is what creates the
     * hot/cold split that aging is supposed to detect. */
    ULONG64 hot[HOT_SPOTS] = { 0 };
    int     hot_next  = 0;
    int     hot_count = 0;

    ULONG64 base_page = GetNextRandom(&thread_rng) % total_pages;
    ULONG64 run_left  = 0;
    ULONG64 cur_page  = base_page;

    unsigned i = 0;
    PVOID arbitrary_va;
    BOOL  page_faulted;
    BOOL  resolved = TRUE;

    ULONG64 runtime = (10 * (MB(1) / 1));

    while (i < runtime) {

        if (resolved) {
            if (run_left == 0) {
                ULONG64 r = GetNextRandom(&thread_rng);

                if (hot_count > 0 && (r % REVISIT_CHANCE) == 0) {
                    /* Jump back to a recent base -- these pages should still be
                     * young; aging should keep them out of the high bins. */
                    base_page = hot[GetNextRandom(&thread_rng) % hot_count];
                } else {
                    /* Cold jump: somewhere new entirely. */
                    base_page = GetNextRandom(&thread_rng) % total_pages;
                    hot[hot_next] = base_page;
                    hot_next = (hot_next + 1) % HOT_SPOTS;
                    if (hot_count < HOT_SPOTS) hot_count++;
                }

                run_left = MIN_RUN_PAGES +
                           (GetNextRandom(&thread_rng) % (MAX_RUN_PAGES - MIN_RUN_PAGES));

                if (base_page + run_left > total_pages) {
                    run_left = total_pages - base_page;
                    if (run_left == 0) {
                        base_page = 0;
                        run_left  = MIN_RUN_PAGES;
                    }
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

        /* Age hint, same CAS discipline as above. */
        {
            PPTE pte = get_pte_from_va(arbitrary_va);
            ULONG64 old_val = *(volatile ULONG64*)pte;

            PTE snapshot;
            *(PULONG64)&snapshot = old_val;

            if (snapshot.hardware.valid == 1 && snapshot.hardware.age == 0) {
                PTE updated = snapshot;
                updated.hardware.age = 1;
                InterlockedCompareExchange64((volatile LONG64*)pte,
                                             *(PLONG64)&updated,
                                             (LONG64)old_val);
            }
        }

        cur_page++;
        run_left--;
        resolved = TRUE;

        if (i > 0 && i % (runtime / 100) == 0) {
            printf(".");
            fflush(stdout);
        }
        i++;
    }

    QueryPerformanceCounter(&end_time);
    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\nthread %d: %u accesses in %.2f ms\n", thread_number, i, elapsed_ms);
}
