/* ============================================================================
 *  main.c  --  privilege acquisition, program setup, the test driver, entry.
 *
 *  GetPrivilege and CreateSharedMemorySection are file-local (only set_up_program
 *  uses them). set_up_program builds all the state the workers assume; the
 *  driver full_virtual_memory_test creates the events, spawns the threads, runs
 *  the workload on the main thread too, then shuts down and tears everything
 *  down. main() just calls the driver.
 * ========================================================================== */

// DEBUGGER CHEATSHEET
// Left 8 digit number = start address, right = end address
// ? = equation/quick math
// lm = list module (lists all modules and where they are)
// ln = list near (give any address, and it will give which variable it belongs to)
// r = registers (shows all the cpu registers and the values within those registers and their variables/locations/instruction)
// kn = stack trace (current stack pointer (at top of VA space), return address (location of var it's returning to), and the function it is in (called call site))
// dv = dump variables (will show locations and values of all variables local to the function)
// bp = breakpoint (set breakpoint at function. ex: bp vmTest!main)
// g = go until breakpoint, completion, or crash
// u = unasemble (tells all the future instructions in your function)
// bl = lists all your breakpoints
// dd = Tries to show you the contents at an address
// .f+ go to next frame (who was the function before me?)
// dq = dump quad (dumps the values of 8 byte chunks at address specified)
// .logopen (opens text file with all the output of the debugger)
// .logclose
// gh = go ahead (degugger don't worry just continue)
// sxd av = (av = access violation) (tells debugger to stop breaking on this particular exception)
// !vprot = (tells you if its legit and gives you the address for the memory allocation and the state ex. MEM_RESERVE, etc.)
// q = quit process
// bd = remove breakpoint (ex. bd 1 removes breakpoint 1)
// ?? var_name = gives value of that variable
// x = see list of globals

// Performance trace cheat sheet
// xperf -on base -stackwalk profile
// Then run your program
// xperf -stop -d trace1.etl
// trace1.etl

#include "Vmm.h"

static BOOL
GetPrivilege(VOID) {
    struct {
        DWORD Count;
        LUID_AND_ATTRIBUTES Privilege[1];
    } Info;

    HANDLE hProcess;
    HANDLE Token;
    BOOL   Result;

    hProcess = GetCurrentProcess();

    Result = OpenProcessToken(hProcess, TOKEN_ADJUST_PRIVILEGES, &Token);
    if (Result == FALSE) {
        printf("Cannot open process token.\n");
        return FALSE;
    }

    Info.Count = 1;
    Info.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;

    Result = LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &(Info.Privilege[0].Luid));
    if (Result == FALSE) {
        printf("Cannot get privilege\n");
        return FALSE;
    }

    Result = AdjustTokenPrivileges(Token, FALSE,
                                   (PTOKEN_PRIVILEGES)&Info, 0, NULL, NULL);
    if (Result == FALSE) {
        printf("Cannot adjust token privileges %u\n", GetLastError());
        return FALSE;
    }

    if (GetLastError() != ERROR_SUCCESS) {
        printf("Cannot enable the SE_LOCK_MEMORY_NAME privilege - check local policy\n");
        return FALSE;
    }

    CloseHandle(Token);
    return TRUE;
}

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
static HANDLE
CreateSharedMemorySection(VOID) {
    HANDLE section;
    MEM_EXTENDED_PARAMETER parameter = { 0 };

    parameter.Type  = MemSectionExtendedParameterUserPhysicalFlags;
    parameter.ULong = 0;

    section = CreateFileMapping2(INVALID_HANDLE_VALUE,
                                 NULL,
                                 SECTION_MAP_READ | SECTION_MAP_WRITE,
                                 PAGE_READWRITE,
                                 SEC_RESERVE,
                                 0,
                                 NULL,
                                 &parameter,
                                 1);
    return section;
}
#endif


/* -------------------------------------------------------------------------
 *  set_up_program  --  all allocation and framework setup.
 * ------------------------------------------------------------------------- */
BOOL
set_up_program(VOID) {
    InitializeLockedList(&pfn_free_list);
    InitializeLockedList(&pfn_modified_list);
    InitializeLockedList(&disc_free_list);
    InitializeLockedList(&pfn_zeroed_list);
    for (int i = 0; i < STANDBY_SHARDS; i++) {
        InitializeLockedList(&pfn_standby_shards[i]);   /* whatever your init is */
    }

    if (GetPrivilege() == FALSE) {
        printf("full_virtual_memory_test : could not get privilege\n");
        return FALSE;
    }

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
    HANDLE physical_page_handle = CreateSharedMemorySection();
    if (physical_page_handle == NULL) {
        printf("CreateFileMapping2 failed, error %#x\n", GetLastError());
        return FALSE;
    }
#else
    HANDLE physical_page_handle = GetCurrentProcess();
#endif

    physical_page_count   = NUMBER_OF_PHYSICAL_PAGES;
    physical_page_numbers = malloc(physical_page_count * sizeof(ULONG_PTR));
    if (physical_page_numbers == NULL) {
        printf("full_virtual_memory_test : could not allocate physical page number array\n");
        return FALSE;
    }

    BOOL allocated = AllocateUserPhysicalPages(physical_page_handle,
                                               &physical_page_count,
                                               physical_page_numbers);
    if (allocated == FALSE) {
        printf("full_virtual_memory_test : could not allocate physical pages\n");
        free(physical_page_numbers);
        return FALSE;
    }

    if (physical_page_count != NUMBER_OF_PHYSICAL_PAGES) {
        printf("full_virtual_memory_test : allocated only %llu of %u requested\n",
               physical_page_count, NUMBER_OF_PHYSICAL_PAGES);
        if (physical_page_count == 0) {
            printf("Received no pages\n");
            free(physical_page_numbers);
            return FALSE;
        }
    }

    setup_pfn_metadata(physical_page_count, physical_page_numbers);


    /* ---- disc ---- */
    disc_page_count = NUM_DISC_PAGES;
    disc = create_page_file(&disc_page_count);
    if (disc == NULL) {
        printf("CRITICAL: could not create page file\n");
        return FALSE;
    }

    /* ---- disc I/O scratch window + hard-fault staging window ----
     * Both must respect the same build mode. */
#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
    MEM_EXTENDED_PARAMETER temp_parameter = { 0 };
    temp_parameter.Type   = MemExtendedParameterUserPhysicalHandle;
    temp_parameter.Handle = physical_page_handle;

    temp_disc_va_start = VirtualAlloc2(NULL, NULL, DISC_BATCH * PAGE_SIZE,
                                       MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE,
                                       &temp_parameter, 1);

    staging_va_start   = VirtualAlloc2(NULL, NULL, STAGING_SLOTS * PAGE_SIZE,
                                       MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE,
                                       &temp_parameter, 1);
#else
    temp_disc_va_start = VirtualAlloc(NULL, DISC_BATCH * PAGE_SIZE,
                                      MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
    staging_va_start   = VirtualAlloc(NULL, STAGING_SLOTS * PAGE_SIZE,
                                      MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
#endif

    /* in set_up_program, next to the other VirtualAlloc2/VirtualAlloc reservations */
#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
    zero_va_start = VirtualAlloc2(NULL, NULL, ZERO_BATCH * PAGE_SIZE,
                                  MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE,
                                  &temp_parameter, 1);
#else
    zero_va_start = VirtualAlloc(NULL, ZERO_BATCH * PAGE_SIZE,
                                 MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
#endif
    if (zero_va_start == NULL) {
        printf("CRITICAL: could not reserve zeroing window. Error: %lu\n", GetLastError());
        return FALSE;
    }

    if (temp_disc_va_start == NULL) {
        printf("CRITICAL: could not reserve disc I/O scratch. Error: %lu\n", GetLastError());
        return FALSE;
    }
    if (staging_va_start == NULL) {
        printf("CRITICAL: could not reserve staging window. Error: %lu\n", GetLastError());
        return FALSE;
    }

    /* ---- size the VA space from physical + disc, minus slack ----
     * Assigns the GLOBAL virtual_address_size (no local shadow). */
    ULONG_PTR slack_pages = (2 * HIGH_WATERMARK) + MAX_TRIM_PAGES + DISC_BATCH;
    virtual_address_size  = (physical_page_count + disc_page_count - slack_pages) * PAGE_SIZE;

    virtual_address_size &= ~((ULONG_PTR)PAGE_SIZE - 1);          /* page align (low 12 bits) */
    virtual_address_size_in_unsigned_chunks = virtual_address_size / sizeof(ULONG_PTR);

    /* ---- PTE array ---- */
    ULONG64 actual_num_ptes = virtual_address_size / PAGE_SIZE;
    page_table = zero_malloc(actual_num_ptes * sizeof(PTE));

    /* ---- PTE regions ---- */
    num_pte_regions = (actual_num_ptes + (PTE_REGION_SIZE - 1)) / PTE_REGION_SIZE;
    pte_regions = zero_malloc(num_pte_regions * sizeof(PTE_REGION));
    if (pte_regions == NULL) {
        printf("CRITICAL: Failed to allocate PTE regions\n");
        return FALSE;
    }

    for (ULONG64 i = 0; i < num_pte_regions; i++) {
        InitializeCriticalSectionAndSpinCount(&pte_regions[i].lock, 0x00FFFFFF);
        for (int age = 0; age < 8; age++) {
            InitializeListHead(&pte_regions[i].active_age_lists[age]);
        }
    }

    /* ---- the user VA space ---- */
#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
    MEM_EXTENDED_PARAMETER parameter = { 0 };
    parameter.Type   = MemExtendedParameterUserPhysicalHandle;
    parameter.Handle = physical_page_handle;

    va_start = VirtualAlloc2(NULL, NULL, virtual_address_size,
                             MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE,
                             &parameter, 1);
#else
    va_start = VirtualAlloc(NULL, virtual_address_size,
                            MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
#endif

    if (va_start == NULL) {
        printf("full_virtual_memory_test : could not reserve VA space %x\n", GetLastError());
        return FALSE;
    }

    SetEvent(NeedZeroingEvent);

    return TRUE;
}


/* -------------------------------------------------------------------------
 *  full_virtual_memory_test  --  the driver: events, threads, run, teardown.
 * ------------------------------------------------------------------------- */
VOID
full_virtual_memory_test(VOID) {
    LARGE_INTEGER frequency, start_time, end_time;
    double elapsed_ms;

    QueryPerformanceFrequency(&frequency);
    printf("Starting virtual memory simulation workload...for general\n");
    QueryPerformanceCounter(&start_time);

    InitializeCriticalSectionAndSpinCount(&disc_lock, 0x00FFFFFF);

    if (set_up_program() == FALSE) {
        return;
    }

    DIAG_PRINT("max_frame=%llu  slots=%llu  table=%llu MB\n",
           max_frame_number, max_frame_number + 1,
           ((max_frame_number + 1) * sizeof(pfn_metadata)) / MB(1));

    /* Auto-reset events reset themselves after one waiter wakes.
     * StandbyPageAvailableEvent and ShutdownEvent are manual-reset so all
     * threads observe them at once. */
    LowPagesEvent             = CreateEvent(NULL, FALSE, FALSE, NULL);
    PagesReadyForDiscEvent    = CreateEvent(NULL, FALSE, FALSE, NULL);
    StandbyPageAvailableEvent = CreateEvent(NULL, TRUE,  FALSE, NULL);
    ShutdownEvent             = CreateEvent(NULL, TRUE,  FALSE, NULL);
    StartAgingEvent           = CreateEvent(NULL, FALSE, FALSE, NULL);
    FinishedAgingEvent        = CreateEvent(NULL, FALSE, FALSE, NULL);
    NeedZeroingEvent          = CreateEvent(NULL, FALSE, FALSE, NULL);   /* auto-reset */

    /* Workers at 0..NUM_WORKER_THREADS-1; background threads in the top slots. */
    HANDLE threads[NUM_WORKER_THREADS + 4] = { NULL };

    threads[NUM_WORKER_THREADS + 0] = CreateThread(NULL, 0, TrimThreadWorker, NULL, 0, NULL);
    threads[NUM_WORKER_THREADS + 1] = CreateThread(NULL, 0, DiscThreadWorker, NULL, 0, NULL);
    threads[NUM_WORKER_THREADS + 2] = CreateThread(NULL, 0, AgeThreadWorker,  NULL, 0, NULL);
    threads[NUM_WORKER_THREADS + 3] = CreateThread(NULL, 0, ZeroThreadWorker, NULL, 0, NULL);

    /* Main runs helper(0) inline, so created workers take VA-thread-numbers 1..N. */
    for (int i = 0; i < NUM_WORKER_THREADS; i++) {
        threads[i] = CreateThread(NULL, 0,
                                  (LPTHREAD_START_ROUTINE)full_virtual_memory_test_helper_not_random,
                                  (PVOID)(ULONG_PTR)(i + 1), 0, NULL);
    }

    full_virtual_memory_test_helper_not_random(0);

    /* Wait for all workers, then signal + join the background threads. */
    WaitForMultipleObjects(NUM_WORKER_THREADS, &threads[0], TRUE, INFINITE);
    SetEvent(ShutdownEvent);
    WaitForMultipleObjects(4, &threads[NUM_WORKER_THREADS], TRUE, INFINITE);

    for (int i = 0; i < NUM_WORKER_THREADS + 4; i++) {
        if (threads[i]) CloseHandle(threads[i]);
    }
    CloseHandle(LowPagesEvent);
    CloseHandle(PagesReadyForDiscEvent);
    CloseHandle(StandbyPageAvailableEvent);
    CloseHandle(ShutdownEvent);
    CloseHandle(StartAgingEvent);
    CloseHandle(FinishedAgingEvent);
    CloseHandle(NeedZeroingEvent);

    QueryPerformanceCounter(&end_time);
    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("==============================================\n\n");

    DIAG_PRINT("faults: hard_disc=%lld hard_zero=%lld soft=%lld | trim_unmaps=%lld\n",
       g_hard_faults_disc, g_hard_faults_zero, g_soft_faults, g_trim_unmaps);

    DIAG_PRINT("zeroed: frames=%lld  zeroed_list_depth=%lld\n",
           g_frames_zeroed, pfn_zeroed_list.count);

    DIAG_PRINT("demand-zero: total=%lld | from_zerolist=%lld pristine=%lld dirty=%lld\n",
           g_hard_faults_zero, g_dz_from_zerolist, g_dz_pristine, g_dz_dirty);

    // DIAG_PRINT("phys=%u free=%lld standby=%lld modified=%lld active(est)=%lld\n",
    //    NUMBER_OF_PHYSICAL_PAGES,
    //    pfn_free_list.count, pfn_standby_list.count, pfn_modified_list.count,
    //    (LONG64)NUMBER_OF_PHYSICAL_PAGES - pfn_free_list.count
    //        - pfn_standby_list.count - pfn_modified_list.count);
    //
    // LARGE_INTEGER freq;
    // QueryPerformanceFrequency(&freq);
    // DIAG_PRINT("lock_wait total: %.1f ms\n",
    //        (double)g_time_lock_wait * 1000.0 / freq.QuadPart);

    /* ---- teardown ---- */
    for (ULONG64 i = 0; i < num_pte_regions; i++) {
        DeleteCriticalSection(&pte_regions[i].lock);
    }
    for (ULONG64 f = 0; f <= max_frame_number; f++) {
        if (FRAME_IS_VALID(f)) {
            DeleteCriticalSection(&frame_to_pfn_table[f].lock);
        }
    }
    free(frame_valid_bitmap);
    DeleteCriticalSection(&disc_lock);

    VirtualFree(va_start, 0, MEM_RELEASE);
}


int
main(void) {
    full_virtual_memory_test();
    return 0;
}