
#if 0
VOID
malloc_test (
    VOID
    )
{
    unsigned i;
    PULONG_PTR p;
    unsigned random_number;

    // Malloc = memory allocation and is a function that makes the operating system do everything under the hood
    // This gets is Virtual Address space
    p = malloc (VIRTUAL_ADDRESS_SIZE);

    if (p == NULL) {
        printf ("malloc_test : could not malloc memory\n");
        return;
    }

    for (i = 0; i < MB (1); i += 1) {

        //
        // Randomly access different portions of the virtual address
        // space we obtained above.
        //
        // If we have never accessed the surrounding page size (4K)
        // portion, the operating system will receive a page fault
        // from the CPU and proceed to obtain a physical page and
        // install a PTE to map it - thus connecting the end-to-end
        // virtual address translation.  Then the operating system
        // will tell the CPU to repeat the instruction that accessed
        // the virtual address and this time, the CPU will see the
        // valid PTE and proceed to obtain the physical contents
        // (without faulting to the operating system again).
        //

        random_number = rand ();

        random_number %= VIRTUAL_ADDRESS_SIZE_IN_UNSIGNED_CHUNKS;

        //
        // Write the virtual address into each page.  If we need to
        // debug anything, we'll be able to see these in the pages.
        //

        *(p + random_number) = (ULONG_PTR) p;
    }

    printf ("malloc_test : finished accessing %u random virtual addresses\n", i);

    //
    // Now that we're done with our memory we can be a good
    // citizen and free it.
    //

    free (p);

    return;
}

VOID
commit_at_fault_time_test (
    VOID
    )
{
    unsigned i;
    PULONG_PTR p;
    PULONG_PTR committed_va;
    unsigned random_number;
    BOOL page_faulted;

    p = VirtualAlloc (NULL,
                      VIRTUAL_ADDRESS_SIZE,
                      MEM_RESERVE, // MEM_RESERVE just lets us reserve VIRTUAL SPCE, but we CANNOT access
                      PAGE_NOACCESS);

    if (p == NULL) {
        printf ("commit_at_fault_time_test : could not reserve memory\n");
        return;
    }

    for (i = 0; i < MB (1); i += 1) {

        //
        // Randomly access different portions of the virtual address
        // space we obtained above.
        //
        // If we have never accessed the surrounding page size (4K)
        // portion, the operating system will receive a page fault
        // from the CPU and proceed to obtain a physical page and
        // install a PTE to map it - thus connecting the end-to-end
        // virtual address translation.  Then the operating system
        // will tell the CPU to repeat the instruction that accessed
        // the virtual address and this time, the CPU will see the
        // valid PTE and proceed to obtain the physical contents
        // (without faulting to the operating system again).
        //

        random_number = rand ();

        random_number %= VIRTUAL_ADDRESS_SIZE_IN_UNSIGNED_CHUNKS;

        //
        // Write the virtual address into each page.  If we need to
        // debug anything, we'll be able to see these in the pages.
        //

        page_faulted = FALSE;

        __try {

            *(p + random_number) = (ULONG_PTR) p;

        } __except (EXCEPTION_EXECUTE_HANDLER) {

            page_faulted = TRUE;
        }

        if (page_faulted) {

            //
            // Commit the virtual address now - if that succeeds then
            // we'll be able to access it from now on.
            //

            committed_va = p + random_number;

            // VirtualAlloc is what malloc calls and lets us handle the pages we are requesting
            committed_va = VirtualAlloc (committed_va,
                                         sizeof (ULONG_PTR),
                                         MEM_COMMIT, // MEM_COMMIT is like "paying" for our reservation so we can use our VIRTUAL pages
                                         PAGE_READWRITE);

            if (committed_va == NULL) {
                printf ("commit_at_fault_time_test : could not commit memory\n");
                return;
            }

            //
            // No exception handler needed now since we are guaranteed
            // by virtue of our commit that the operating system will
            // honor our access.
            //

            *committed_va = (ULONG_PTR) committed_va;
        }
    }

    printf ("commit_at_fault_time_test : finished accessing %u random virtual addresses\n", i);

    //
    // Now that we're done with our memory we can be a good
    // citizen and free it.
    //

    VirtualFree (p, 0, MEM_RELEASE);

    return;
}
#endif


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
// Once in the trace, click Trace and then Load Symbols

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <intrin.h>

#define SUPPORT_MULTIPLE_VA_TO_SAME_PAGE 1

#pragma comment(lib, "advapi32.lib")

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
#pragma comment(lib, "onecore.lib")
#endif

// Sizing constants
#define PAGE_SIZE 4096
#define MB(x) ((x) * 1024 * 1024)
#define KB(x) ((x) * 1024)

#define MAX_AGE_BITS 8

#define NUMBER_OF_PHYSICAL_PAGES (MB(1024) / PAGE_SIZE)
//#define NUMBER_OF_PHYSICAL_PAGES 4
#define NUM_PTEs (VIRTUAL_ADDRESS_SIZE / PAGE_SIZE)
#define NUM_DISC_PAGES (5 * NUMBER_OF_PHYSICAL_PAGES)
#define MAX_DISC_PTE_BITS 40
#define MAX_DISC_SIZE ((ULONG64) 1 << MAX_DISC_PTE_BITS)
#define INVALID_DISC_SLOT ((1ULL << MAX_DISC_PTE_BITS) - 1)
#define DISC_BATCH 64
#define MAX_TRIM_PAGES 64

#define PTE_REGION_SIZE 512
// How few pages we tolerate before waking the background Trim Thread
#define LOWEST_PAGES 200
#define LOW_WATERMARK  (LOWEST_PAGES)          // start trimming below this
#define HIGH_WATERMARK (LOWEST_PAGES * 4)      // trim until we reach this

#define DEBUG 1
#if defined(DEBUG)
#define ASSERT(condition) \
if ((condition)== FALSE) { \
DebugBreak(); \
}
#else
#define ASSERT(condition)
#endif

VOID
InitializeListHead (
    PLIST_ENTRY ListHead
)
{
    ListHead->Flink = ListHead->Blink = ListHead;
    return;
}

BOOLEAN
IsListEmpty (
    PLIST_ENTRY ListHead
)
{
    return (BOOLEAN) (ListHead->Flink == ListHead);
}

VOID
InsertTailList (
    PLIST_ENTRY ListHead,
    PLIST_ENTRY Entry
)
{
    PLIST_ENTRY Blink;
    Blink = ListHead->Blink;
    Entry->Flink = ListHead;
    Entry->Blink = Blink;
    Blink->Flink = Entry;
    ListHead->Blink = Entry;
    return;
}

PLIST_ENTRY
RemoveHeadList (
    PLIST_ENTRY ListHead
)
{
    PLIST_ENTRY Flink;
    PLIST_ENTRY Entry;

    Entry = ListHead->Flink;
    Flink = Entry->Flink;
    ListHead->Flink = Flink;
    Flink->Blink = ListHead;

    return Entry;
}

BOOLEAN
RemoveEntryList (
    PLIST_ENTRY Entry
)
{
    PLIST_ENTRY Blink;
    PLIST_ENTRY Flink;

    Flink = Entry->Flink;
    Blink = Entry->Blink;
    Blink->Flink = Flink;
    Flink->Blink = Blink;

    return (BOOLEAN) (Flink == Blink);
}

// Struct for our PTEs
typedef struct {
    ULONG64 valid: 1;
    ULONG64 frame_number: 40;
    ULONG64 age: 3;
    ULONG64 reserved: 20;
} VALID_PTE, *PVALID_PTE;

typedef struct {
    ULONG64 valid: 1; // = 0
    ULONG64 transition: 1; // = 1
    ULONG64 frame_number: 40;
    ULONG64 reserved: 22;
} TRANSITION_PTE, *PTRANSITION_PTE;

typedef struct {
    ULONG64 valid: 1; // = 0
    ULONG64 transition: 1; // = 0
    ULONG64 disc: 1; // = 1
    ULONG64 disc_index: MAX_DISC_PTE_BITS;
    ULONG64 reserved: 64 - MAX_DISC_PTE_BITS - 3;
} DISC_PTE, *PDISC_PTE;

typedef struct {
    union {
        VALID_PTE hardware;
        TRANSITION_PTE transition;
        DISC_PTE disc;
    };
} PTE, *PPTE;

PPTE page_table;

typedef struct _PTE_REGION {
    CRITICAL_SECTION lock;
    LIST_ENTRY active_age_lists[MAX_AGE_BITS];
    ULONG64 active_page_count; // Tracks how many active pages are in this region
} PTE_REGION, *PPTE_REGION;

PPTE_REGION pte_regions;

// Struct for our PFNs
typedef struct {
    LIST_ENTRY list;
    ULONG64 frame_number;
    PPTE pte;
    ULONG64 disc_index: MAX_DISC_PTE_BITS;
    ULONG64 isOccupied: 2; // 00 = free, 01 = active, 10 = modified, 11 = standby
    ULONG64 isBeingWrittenToDisc: 1; // 0 = no 1 = yes
} pfn_metadata;

typedef struct _LOCKED_LIST {
    LIST_ENTRY head;          // flink/blink
    CRITICAL_SECTION lock;    // guards this list AND its count
    volatile LONG64 count;    // list length, updated only under lock
} LOCKED_LIST, *PLOCKED_LIST;

LOCKED_LIST pfn_free_list;
LOCKED_LIST pfn_modified_list;
LOCKED_LIST pfn_standby_list;
LOCKED_LIST disc_free_list;

typedef struct _DISC_METADATA {
    LIST_ENTRY list;
    ULONG64 index;
    BOOL isOccupied;
} DISC_METADATA, *PDISC_METADATA;

PDISC_METADATA disc_metadata;

PULONG_PTR physical_page_numbers;

// Global tracking variables to preserve value states completely across functions
ULONG_PTR physical_page_count;
ULONG64 disc_page_count;
PVOID disc;
ULONG_PTR virtual_address_size_in_unsigned_chunks;
PULONG_PTR va_start;

pfn_metadata* frame_to_pfn_table = NULL;
ULONG64 max_frame_number = 0;

// Temporary VA when we write unmapped pages to disc cause the before VA cannot be accessed since it is unmapepd
PVOID temp_disc_va_start = NULL;

CRITICAL_SECTION cs;
ULONG64 num_pte_regions;

CRITICAL_SECTION pfn_lock;
CRITICAL_SECTION disc_lock;

CONDITION_VARIABLE wait_for_disc_write;

// Global Event Handles
HANDLE LowPagesEvent;
HANDLE PagesReadyForDiscEvent;
HANDLE StandbyPageAvailableEvent;
HANDLE ShutdownEvent;
HANDLE StartAgingEvent;      // auto-reset: trim asks for a pass
HANDLE FinishedAgingEvent;   // auto-reset: aging reports a completed sweep


// Get the metadata from a node on a list
pfn_metadata*
GetPfnFromListEntry(PLIST_ENTRY entry) {
    if (entry == NULL) return NULL;
    return CONTAINING_RECORD(entry, pfn_metadata, list);
}

VOID InitializeLockedList(PLOCKED_LIST list) {
    InitializeListHead(&list->head);
    InitializeCriticalSectionAndSpinCount(&list->lock, 0x00FFFFFF);
    list->count = 0;
}

// Pop head entry; NULL if empty. Atomic check+pop+count under the list lock.
PLIST_ENTRY LockedRemoveHead(PLOCKED_LIST list) {
    PLIST_ENTRY entry = NULL;
    EnterCriticalSection(&list->lock);
    if (!IsListEmpty(&list->head)) {
        entry = RemoveHeadList(&list->head);
        list->count--;
    }
    LeaveCriticalSection(&list->lock);
    return entry;
}

VOID LockedInsertTail(PLOCKED_LIST list, PLIST_ENTRY entry) {
    EnterCriticalSection(&list->lock);
    InsertTailList(&list->head, entry);
    list->count++;
    LeaveCriticalSection(&list->lock);
}

// For rescue paths: caller identifies the entry; we unlink it safely.
VOID LockedRemoveEntry(PLOCKED_LIST list, PLIST_ENTRY entry) {
    EnterCriticalSection(&list->lock);
    RemoveEntryList(entry);
    list->count--;
    LeaveCriticalSection(&list->lock);
}

// Handles if we straddle two pages in our metadata array
VOID
ensure_metadata_slot_is_committed(pfn_metadata* table_base, ULONG64 frame_number) {
    ULONG_PTR struct_start_va = (ULONG_PTR)&table_base[frame_number];
    ULONG_PTR struct_end_va = struct_start_va + sizeof(pfn_metadata) - 1;

    ULONG_PTR start_page = struct_start_va / PAGE_SIZE;
    ULONG_PTR end_page = struct_end_va / PAGE_SIZE;

    ULONG_PTR pages_to_commit = (end_page - start_page) + 1;
    ULONG_PTR bytes_to_commit = pages_to_commit * PAGE_SIZE;

    PVOID page_aligned_address = (PVOID)(struct_start_va & ~(PAGE_SIZE - 1));

    if (VirtualAlloc(page_aligned_address, bytes_to_commit, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        printf("CRITICAL: Failed to commit metadata pages for frame %llu. Error: %lu\n",
               frame_number, GetLastError());
        DebugBreak();
    }
}

// New 0(1) way to get the metadata from a frame number
pfn_metadata*
find_pfn_from_frame_number(ULONG64 frame_number) {
    if (frame_number <= max_frame_number) {
        return &frame_to_pfn_table[frame_number];
    }
    return NULL;
}

// Malloc and zero out new data
PVOID
zero_malloc(size_t num_bytes) {
    PVOID p = malloc(num_bytes);
    if (p == NULL) {
        DebugBreak();
    }
    memset(p, 0, num_bytes);
    return p;
}

// Create our new pfn metadata sparse array
VOID
setup_pfn_metadata(ULONG_PTR physical_page_count, PULONG_PTR physical_page_numbers) {
    // Find the max frame number we were given
    for (int j = 0; j < physical_page_count; j++) {
        if (physical_page_numbers[j] > max_frame_number) {
            max_frame_number = physical_page_numbers[j];
        }
    }

    // Size the array based on our largest frame number
    ULONG_PTR table_reserve_size = (max_frame_number + 1) * sizeof(pfn_metadata);

    // LK was lazy and mem commited everything, go back and only commit the places with the frame numbers
    frame_to_pfn_table = (pfn_metadata*)VirtualAlloc(
        NULL,
        table_reserve_size,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE
    );

    if (frame_to_pfn_table == NULL) {
        printf("CRITICAL: Failed to reserve and commit tracking table. Error: %lu\n", GetLastError());
        DebugBreak();
        return;
    }

    // Set up the pfn metadata and add everything to the free list initially
    for (int j = 0; j < physical_page_count; j++) {
        ULONG64 frame = physical_page_numbers[j];

        frame_to_pfn_table[frame].frame_number = frame;
        frame_to_pfn_table[frame].isOccupied = 0;

        frame_to_pfn_table[frame].disc_index = INVALID_DISC_SLOT;
        frame_to_pfn_table[frame].isBeingWrittenToDisc = 0;

        InitializeListHead(&frame_to_pfn_table[frame].list);
        LockedInsertTail(&pfn_free_list, &frame_to_pfn_table[frame].list);
    }
}

typedef struct _THREAD_RNG_STATE {
    ULONG64 state;
    unsigned int counter;
} THREAD_RNG_STATE;

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

PPTE
get_pte_from_va(PULONG_PTR arbitrary_va) {
    ULONG64 byte_distance = (ULONG64)arbitrary_va - (ULONG64)va_start;
    ULONG64 index = byte_distance / PAGE_SIZE;

    // Return the actual PTE in the page table
    return &page_table[index];
}

PULONG_PTR
get_va_from_pte(PPTE pte) {
    ULONG64 pte_index = pte - page_table;
    ULONG_PTR byte_offset = pte_index * PAGE_SIZE;

    // Return the actual Virtual Address
    return (PULONG_PTR)((ULONG_PTR)va_start + byte_offset);
}

VOID
set_pte_valid(PPTE pte, ULONG64 pfn) {
    pte->hardware.valid = TRUE;
    pte->hardware.frame_number = pfn;
}

VOID
set_pte_invalid(PPTE pte) {
    pte->hardware.valid = FALSE;
}

pfn_metadata*
get_free_page() {

    PLIST_ENTRY entry = LockedRemoveHead(&pfn_free_list);
    return GetPfnFromListEntry(entry);

}

VOID
set_to_disc_state(PPTE old_owner_pte, pfn_metadata* target_pfn) {
    // 1. MUST wipe the entire 64-bit struct to clear the transition bits!
    *(PULONG64)old_owner_pte = 0;

    // 2. Now it is safe to set the disc state
    old_owner_pte->disc.disc = 1;
    old_owner_pte->disc.disc_index = target_pfn->disc_index;
}

PDISC_METADATA disc_metadata;

// Create our disc
PVOID create_page_file(PULONG64 number_of_pages) {
    PVOID p;
    ULONG64 num_bytes;

    // Resize to max disc size
    if (*number_of_pages > MAX_DISC_SIZE) {
        *number_of_pages = MAX_DISC_SIZE;
    }
    num_bytes = *number_of_pages * PAGE_SIZE;

    // Allocate with strict PAGE_SIZE alignment
    p = _aligned_malloc(num_bytes, PAGE_SIZE);
    while (p == NULL) {
        num_bytes /= 2;
        p = _aligned_malloc(num_bytes, PAGE_SIZE);
    }
    *number_of_pages = num_bytes / PAGE_SIZE;

    disc_metadata = malloc(*number_of_pages * sizeof(DISC_METADATA));
    if (disc_metadata == NULL) {
        printf("could not allocate disc_metadata and fake disc\n");
        _aligned_free(p); // Make sure to use _aligned_free!
        return NULL;
    }
    memset(disc_metadata, 0, *number_of_pages * sizeof(DISC_METADATA));

    // Wire up the metadata and insert into the free list
    for (ULONG64 i = 0; i < *number_of_pages; i++) {
        disc_metadata[i].index = i;
        disc_metadata[i].isOccupied = FALSE;
        LockedInsertTail(&disc_free_list, &disc_metadata[i].list);
    }

    // Return how much we could actually allocate for disc
    return p;
}

ULONG64 find_free_disc_slot() {
    PLIST_ENTRY e = LockedRemoveHead(&disc_free_list);   // atomic check+pop
    if (e == NULL) return INVALID_DISC_SLOT;
    PDISC_METADATA meta = CONTAINING_RECORD(e, DISC_METADATA, list);
    meta->isOccupied = TRUE;                              // fine: we exclusively own this slot now
    return meta->index;
}

BOOL
GetPrivilege (
    VOID
)
{
    struct {
        DWORD Count;
        LUID_AND_ATTRIBUTES Privilege [1];
    } Info;

    HANDLE hProcess;
    HANDLE Token;
    BOOL Result;

    hProcess = GetCurrentProcess ();

    Result = OpenProcessToken (hProcess,
                               TOKEN_ADJUST_PRIVILEGES,
                               &Token);

    if (Result == FALSE) {
        printf ("Cannot open process token.\n");
        return FALSE;
    }

    Info.Count = 1;
    Info.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;

    Result = LookupPrivilegeValue (NULL,
                                   SE_LOCK_MEMORY_NAME,
                                   &(Info.Privilege[0].Luid));

    if (Result == FALSE) {
        printf ("Cannot get privilege\n");
        return FALSE;
    }

    Result = AdjustTokenPrivileges (Token,
                                    FALSE,
                                    (PTOKEN_PRIVILEGES) &Info,
                                    0,
                                    NULL,
                                    NULL);

    if (Result == FALSE) {
        printf ("Cannot adjust token privileges %u\n", GetLastError ());
        return FALSE;
    }

    if (GetLastError () != ERROR_SUCCESS) {
        printf ("Cannot enable the SE_LOCK_MEMORY_NAME privilege - check local policy\n");
        return FALSE;
    }

    CloseHandle (Token);
    return TRUE;
}

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
HANDLE
CreateSharedMemorySection (
    VOID
)
{
    HANDLE section;
    MEM_EXTENDED_PARAMETER parameter = { 0 };

    parameter.Type = MemSectionExtendedParameterUserPhysicalFlags;
    parameter.ULong = 0;

    section = CreateFileMapping2 (INVALID_HANDLE_VALUE,
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

CRITICAL_SECTION* get_pte_lock_for_va(PVOID arbitrary_va) {
    // 1. Get the raw PTE index
    ULONG_PTR offset = (ULONG_PTR)arbitrary_va - (ULONG_PTR)va_start;
    ULONG64 pte_index = offset / PAGE_SIZE;

    // 2. Divide by 512 to find the region block
    ULONG64 region_index = pte_index / PTE_REGION_SIZE;

    // 3. Return the address of the lock inside that specific region struct
    return &pte_regions[region_index].lock;
}

CRITICAL_SECTION* get_pte_lock_from_pte_pointer(PPTE target_pte) {
    // 1. Calculate the PTE index via pointer arithmetic
    ULONG64 pte_index = (ULONG64)(target_pte - page_table);

    // 2. Divide by 512 to find the region block
    ULONG64 region_index = pte_index / PTE_REGION_SIZE;

    // 3. Return the address of the lock inside that specific region struct
    return &pte_regions[region_index].lock;
}

VOID write_to_disc(pfn_metadata** candidates, ULONG64 batch_count) {
    ULONG_PTR frame_array[DISC_BATCH];
    ULONG64 disc_slots[DISC_BATCH];


    // Prepare the localized I/O batch arrays
    for (int i = 0; i < batch_count; i++) {
        disc_slots[i] = candidates[i]->disc_index;
        frame_array[i] = candidates[i]->frame_number;
    }


    // 2. Extract frame numbers for mapping
    for (int i = 0; i < batch_count; i++) {
        frame_array[i] = candidates[i]->frame_number;
    }

    // 3. Map the entire batch of physical frames into our contiguous scratch space
    if (MapUserPhysicalPages(temp_disc_va_start, batch_count, frame_array) == FALSE) {
        printf("CRITICAL: Failed to map batch scratch space! Error: %lu\n", GetLastError());
        DebugBreak();
        return;
    }


    // 4. Copy each frame to its assigned slot on the simulated disc
    for (int i = 0; i < batch_count; i++) {
        if (disc_slots[i] >= NUM_DISC_PAGES) {
            printf("CRITICAL: Invalid disc slot index detected! i: %zu, slot: 0x%llx\n", i, disc_slots[i]);
            DebugBreak();
            return;
        }

        if (disc_slots[i] != INVALID_DISC_SLOT) {
            void* source_va = (char*)temp_disc_va_start + (i * PAGE_SIZE);

            // 3. Cast back to a pointer for memcpy
            void* disc_address = (char*)disc + (disc_slots[i] * PAGE_SIZE);

            if (((ULONG_PTR)source_va & (PAGE_SIZE - 1)) != 0 ||
            ((ULONG_PTR)disc_address & (PAGE_SIZE - 1)) != 0) {

                printf("CRITICAL: Alignment fault before memcpy! source_va: %p, disc_address: %p\n",
                       source_va, disc_address);
                DebugBreak(); // Halt execution immediately for debugging
                return;       // Prevents the unaligned/corrupt memcpy from executing
            }

            memcpy(disc_address, source_va, PAGE_SIZE);
        } else {
            printf("CRITICAL: Ran out of disc space during batch write!\n");
            DebugBreak();
        }
    }

    // 5. Unmap the scratch space in one call
    MapUserPhysicalPages(temp_disc_va_start, batch_count, NULL);


}

// Caller must hold region->lock.
static VOID harvest_one_bin(PPTE_REGION region, PLIST_ENTRY head,
                            int* batch_count, int batch_size) {
    while (!IsListEmpty(head) && *batch_count < batch_size) {
        PLIST_ENTRY entry = RemoveHeadList(head);
        pfn_metadata* candidate = GetPfnFromListEntry(entry);
        PPTE evict_pte = candidate->pte;

        ULONG64 saved_frame = candidate->frame_number;
        PVOID target_va = get_va_from_pte(evict_pte);

        if (MapUserPhysicalPages(target_va, 1, NULL) == FALSE) {
            printf("Failed to individually unmap VA %p. Error: %lu\n",
                   target_va, GetLastError());
        }

        *(PULONG64)evict_pte = 0;
        evict_pte->transition.valid = 0;
        evict_pte->transition.transition = 1;
        evict_pte->transition.frame_number = saved_frame;

        (*batch_count)++;
        candidate->isOccupied = 2;

        LockedInsertTail(&pfn_modified_list, &candidate->list);
    }
}
VOID
get_unmap_candidates(int* batch_count, int batch_size) {
    static ULONG64 current_trim_region = 0;
    ULONG64 regions_checked = 0;

    // Pass 1: bins 7..1 across the sweep
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

    // Pass 2 (last resort): the full sweep found NOTHING in bins 7..1.
    // Take bin 0 from any region we can lock, rather than starving.
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

// Handles allocation setups and underlying framework requirements
BOOL set_up_program() {
    // Initialize our lists
    InitializeLockedList(&pfn_free_list);
    InitializeLockedList(&pfn_modified_list);
    InitializeLockedList(&pfn_standby_list);
    InitializeLockedList(&disc_free_list);

    BOOL privilege = GetPrivilege();
    if (privilege == FALSE) {
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

    // malloc for our page numbers
    physical_page_count = NUMBER_OF_PHYSICAL_PAGES;
    physical_page_numbers = malloc(physical_page_count * sizeof(ULONG_PTR));

    if (physical_page_numbers == NULL) {
        printf("full_virtual_memory_test : could not allocate array to hold physical page numbers\n");
        return FALSE;
    }

    // Put our frame numbers in the physical_page_numbers array
    BOOL allocated = AllocateUserPhysicalPages(physical_page_handle,
                                               &physical_page_count,
                                               physical_page_numbers);

    if (allocated == FALSE) {
        printf("full_virtual_memory_test : could not allocate physical pages\n");
        free(physical_page_numbers);
        return FALSE;
    }

    if (physical_page_count != NUMBER_OF_PHYSICAL_PAGES) {
        printf("full_virtual_memory_test : allocated only %llu pages out of %u pages requested\n",
               physical_page_count,
               NUMBER_OF_PHYSICAL_PAGES);

        if (physical_page_count == 0) {
            printf("Received no pages\n");
            free(physical_page_numbers);
            return FALSE;
        }
    }

    // Setup our pfn metadata
    setup_pfn_metadata(physical_page_count, physical_page_numbers);

    // Set up disc
    disc_page_count = NUM_DISC_PAGES;
    disc = create_page_file(&disc_page_count);

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
    MEM_EXTENDED_PARAMETER temp_parameter = { 0 };
    temp_parameter.Type = MemExtendedParameterUserPhysicalHandle;
    temp_parameter.Handle = physical_page_handle;

    temp_disc_va_start = VirtualAlloc2(NULL,
                                 NULL,
                                 DISC_BATCH * PAGE_SIZE,
                                 MEM_RESERVE | MEM_PHYSICAL,
                                 PAGE_READWRITE,
                                 &temp_parameter,
                                 1);
#else
    temp_disc_va_start = VirtualAlloc(NULL, DISC_BATCH * PAGE_SIZE, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
#endif
    if (temp_disc_va_start == NULL) {
        printf("CRITICAL: Could not reserve batched memory for disc I/O scratch space. Error: %lu\n", GetLastError());
        return FALSE;
    }

    ULONG_PTR slack_pages = (2 * HIGH_WATERMARK) + MAX_TRIM_PAGES + DISC_BATCH;
    // Set virtual address size based on our physical and virtual page counts
    ULONG_PTR virtual_address_size = (physical_page_count + disc_page_count - slack_pages) * PAGE_SIZE;

    // LK VA space is 1 less than it should be
    virtual_address_size &= ~PAGE_SIZE;
    virtual_address_size_in_unsigned_chunks = virtual_address_size / sizeof(ULONG_PTR);

    // Set up our page table for our ptes
    ULONG64 actual_num_ptes = virtual_address_size / PAGE_SIZE;
    page_table = zero_malloc(actual_num_ptes * sizeof(PTE));

    num_pte_regions = (actual_num_ptes + (PTE_REGION_SIZE - 1)) / PTE_REGION_SIZE;
    pte_regions = zero_malloc(num_pte_regions * sizeof(PTE_REGION));

    if (pte_regions == NULL) {
        printf("CRITICAL: Failed to allocate PTE regions\n");
        return FALSE;
    }

    // Initialize locks and list heads for every single region
    for (ULONG64 i = 0; i < num_pte_regions; i++) {
        // 1. Initialize the region lock
        InitializeCriticalSectionAndSpinCount(&pte_regions[i].lock, 0x00FFFFFF);

        // 2. FIX: Initialize every age list head in this region (assuming 8 age tiers, 0-7)
        for (int age = 0; age < 8; age++) {
            InitializeListHead(&pte_regions[i].active_age_lists[age]);
        }
    }

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
    MEM_EXTENDED_PARAMETER parameter = { 0 };
    parameter.Type = MemExtendedParameterUserPhysicalHandle;
    parameter.Handle = physical_page_handle;

    va_start = VirtualAlloc2(NULL,
                             NULL,
                             virtual_address_size,
                             MEM_RESERVE | MEM_PHYSICAL,
                             PAGE_READWRITE,
                             &parameter,
                             1);
#else
    va_start = VirtualAlloc(NULL,
                            virtual_address_size,
                            MEM_RESERVE | MEM_PHYSICAL,
                            PAGE_READWRITE);
#endif

    if (va_start == NULL) {
        printf("full_virtual_memory_test : could not reserve memory %x\n", GetLastError());
        return FALSE;
    }

    return TRUE;
}

pfn_metadata* pull_from_standby_safely(CRITICAL_SECTION* my_pte_lock) {
    pfn_metadata* result = NULL;

    EnterCriticalSection(&pfn_standby_list.lock);

    PLIST_ENTRY standby_entry = pfn_standby_list.head.Flink;

    while (standby_entry != &pfn_standby_list.head) {
        pfn_metadata* target_pfn = GetPfnFromListEntry(standby_entry);
        PPTE old_owner_pte = target_pfn->pte;

        if (old_owner_pte != NULL) {
            CRITICAL_SECTION* old_region_lock = get_pte_lock_from_pte_pointer(old_owner_pte);

            if (old_region_lock == my_pte_lock) {
                RemoveEntryList(&target_pfn->list);
                pfn_standby_list.count--;
                set_to_disc_state(old_owner_pte, target_pfn);
                target_pfn->disc_index = INVALID_DISC_SLOT;
                result = target_pfn;
                break;
            }
            // NOTE: TryEnter is REQUIRED here. We hold a list lock and are
            // acquiring a region lock — reverse of the region→list hierarchy.
            // A blocking Enter here deadlocks against any faulting thread.
            else if (TryEnterCriticalSection(old_region_lock)) {
                RemoveEntryList(&target_pfn->list);
                pfn_standby_list.count--;
                set_to_disc_state(old_owner_pte, target_pfn);
                target_pfn->disc_index = INVALID_DISC_SLOT;
                LeaveCriticalSection(old_region_lock);
                result = target_pfn;
                break;
            }
        } else {
            RemoveEntryList(&target_pfn->list);
            pfn_standby_list.count--;
            target_pfn->disc_index = INVALID_DISC_SLOT;
            result = target_pfn;
            break;
        }
        standby_entry = standby_entry->Flink;
    }

    LeaveCriticalSection(&pfn_standby_list.lock);
    return result;
}

VOID handle_hard_fault(PPTE pte, PVOID arbitrary_va, CRITICAL_SECTION* my_pte_lock) {

    // If we are on disc then get disc index else set to -1
    BOOL is_on_disc = pte->disc.disc;
    ULONG64 old_disc_slot = is_on_disc ? pte->disc.disc_index : INVALID_DISC_SLOT;

    pfn_metadata* target_pfn = NULL;
    while (target_pfn == NULL) {

        PLIST_ENTRY free_entry = LockedRemoveHead(&pfn_free_list);
        if (free_entry != NULL) {
            target_pfn = GetPfnFromListEntry(free_entry);
        }
        else {
            target_pfn = pull_from_standby_safely(my_pte_lock);
        }

        if (target_pfn == NULL) {
            SetEvent(LowPagesEvent);

            LeaveCriticalSection(my_pte_lock);

            WaitForSingleObject(StandbyPageAvailableEvent, INFINITE);

            EnterCriticalSection(my_pte_lock);


            // Count reads for heuristics are fine lock-free
            if (pfn_free_list.count == 0 && pfn_standby_list.count == 0) {
                ResetEvent(StandbyPageAvailableEvent);
            }

            if (pte->hardware.valid == 1 || pte->transition.transition == 1) {
                LeaveCriticalSection(my_pte_lock);
                return;
            }
            // (hard fault only: re-read is_on_disc / old_disc_slot here)
            is_on_disc = (pte->transition.transition == 0) && (pte->disc.disc == 1);
            old_disc_slot = is_on_disc ? pte->disc.disc_index : INVALID_DISC_SLOT;
        }
    }

    // If we got a page
    if (target_pfn != NULL) {
        target_pfn->disc_index = INVALID_DISC_SLOT;
        // Inside handle_page_fault...
        if (pfn_free_list.count < LOWEST_PAGES) {
            // Wake up the background TrimThread!
            SetEvent(LowPagesEvent);
        }

        // Make sure our va is aligned so we don't straddle two pages
        PVOID va_aligned = (PVOID)((ULONG_PTR)arbitrary_va & ~((ULONG_PTR)PAGE_SIZE - 1));

        // Map the page to handle the fault
        if (MapUserPhysicalPages(va_aligned, 1, &target_pfn->frame_number) == FALSE) {
            printf("Failed to map target window. Error: %lu\n", GetLastError());
            LeaveCriticalSection(my_pte_lock);
            return;
        }

        // If we were on disc
        if (is_on_disc && old_disc_slot != INVALID_DISC_SLOT) {
            void* disc_address = (char*)disc + (old_disc_slot * PAGE_SIZE);
            memcpy(va_aligned, disc_address, PAGE_SIZE);

            EnterCriticalSection(&disc_lock);

            disc_metadata[old_disc_slot].isOccupied = FALSE;
            LeaveCriticalSection(&disc_lock);
            LockedInsertTail(&disc_free_list, &disc_metadata[old_disc_slot].list);


        }
        // If we didn't have data we needed to transfer to new page, clear it
        else {
            memset(va_aligned, 0, PAGE_SIZE);
        }

        // Set to active state
        target_pfn->isOccupied = 1;

        target_pfn->pte = pte;

        // NEW WAY: Put it in Age Bin 0 of this region
        ULONG64 region_index = (pte - page_table) / PTE_REGION_SIZE;
        InsertTailList(&pte_regions[region_index].active_age_lists[0], &target_pfn->list);

        // Wipe old pte before so no old contents from union are there
        *(PULONG64)pte = 0;

        set_pte_valid(pte, target_pfn->frame_number);
        pte->hardware.age = 0;

        LeaveCriticalSection(my_pte_lock);

    }
    // If we never got a page
    else {
        printf("CRITICAL: Out of physical memory and swap space!\n");
        DebugBreak();
    }
}

VOID
handle_soft_fault(PPTE pte, PVOID arbitrary_va, CRITICAL_SECTION* my_pte_lock) {
    // Rescue
    pfn_metadata *target_pfn = NULL;

        // 2. Safe to check transition state now
    ULONG64 frame_number = pte->transition.frame_number;
    target_pfn = find_pfn_from_frame_number(frame_number);


    if (target_pfn != NULL) {
        BOOL poached = FALSE;
        EnterCriticalSection(&pfn_lock);
        if (target_pfn->isBeingWrittenToDisc == 1) {
            // Flag to let disc writer know we poached
            target_pfn->isBeingWrittenToDisc = 0;
            poached = TRUE;

        }
        LeaveCriticalSection(&pfn_lock);
        if (!poached) {
            // Normal rescue
            if (target_pfn->isOccupied == 3) {
                EnterCriticalSection(&disc_lock);
                if (target_pfn->disc_index != INVALID_DISC_SLOT) {
                    disc_metadata[target_pfn->disc_index].isOccupied = FALSE;
                    LockedInsertTail(&disc_free_list, &disc_metadata[target_pfn->disc_index].list);

                    target_pfn->disc_index = INVALID_DISC_SLOT;
                }
                LeaveCriticalSection(&disc_lock);
                LockedRemoveEntry(&pfn_standby_list, &target_pfn->list);
            }
            else if (target_pfn->isOccupied == 2) {
                LockedRemoveEntry(&pfn_modified_list, &target_pfn->list);
            }
        }

        // 2. Mark it as actively occupied again
        target_pfn->isOccupied = 1;

        // 3. Put it back on the active list
        ULONG64 region_index = (pte - page_table) / PTE_REGION_SIZE;
        InsertTailList(&pte_regions[region_index].active_age_lists[0], &target_pfn->list);

        // 4. Align the Virtual Address
        PVOID va_aligned = (PVOID)((ULONG_PTR)arbitrary_va & ~((ULONG_PTR)PAGE_SIZE - 1));

        // 5. Map the physical frame back into user space
        if (MapUserPhysicalPages(va_aligned, 1, &target_pfn->frame_number) == FALSE) {
            printf("CRITICAL: Failed to remap rescued page. Error: %lu\n", GetLastError());
            DebugBreak();
        }

        // 6. Update the PTE: wipe the transition state and mark it valid
        *(PULONG64)pte = 0;
        set_pte_valid(pte, target_pfn->frame_number);
        pte->hardware.age = 0;

        LeaveCriticalSection(my_pte_lock);

        // Rescue complete!
        return;
    }
    else {
        pfn_metadata* target_pfn = NULL;
        while (target_pfn == NULL) {

            PLIST_ENTRY free_entry = LockedRemoveHead(&pfn_free_list);
            if (free_entry != NULL) {
                target_pfn = GetPfnFromListEntry(free_entry);
            }
            else {
                target_pfn = pull_from_standby_safely(my_pte_lock);
            }

            if (target_pfn == NULL) {
                SetEvent(LowPagesEvent);

                LeaveCriticalSection(my_pte_lock);

                WaitForSingleObject(StandbyPageAvailableEvent, INFINITE);

                EnterCriticalSection(my_pte_lock);

                // Count reads for heuristics are fine lock-free
                if (pfn_free_list.count == 0 && pfn_standby_list.count == 0) {
                    ResetEvent(StandbyPageAvailableEvent);
                }

                if (pte->hardware.valid == 1 || pte->transition.transition == 1) {
                    LeaveCriticalSection(my_pte_lock);
                    return;
                }
            }
        }

        // Trigger background trim if we are getting dangerously low on free memory
        if (pfn_free_list.count < LOWEST_PAGES) {
            SetEvent(LowPagesEvent);
        }

        PVOID va_aligned = (PVOID)((ULONG_PTR)arbitrary_va & ~((ULONG_PTR)PAGE_SIZE - 1));

        // Map our brand-new fallback page
        if (MapUserPhysicalPages(va_aligned, 1, &target_pfn->frame_number) == FALSE) {
            printf("Failed to map fallback target window. Error: %lu\n", GetLastError());
            LeaveCriticalSection(my_pte_lock);
            return;
        }

        // CRITICAL: The old transition data is gone forever,
        // so we must zero out this page completely to maintain memory safety.
        memset(va_aligned, 0, PAGE_SIZE);

        // Set up the newly acquired PFN metadata
        target_pfn->isOccupied = 1;
        target_pfn->pte = pte;

        ULONG64 region_index = (pte - page_table) / PTE_REGION_SIZE;
        InsertTailList(&pte_regions[region_index].active_age_lists[0], &target_pfn->list);

        // Wipe out old transition status elements from the entry safely
        *(PULONG64)pte = 0;
        set_pte_valid(pte, target_pfn->frame_number);
        pte->hardware.age = 0;

        LeaveCriticalSection(my_pte_lock);
    }
}

// Handle the page fault
VOID handle_page_fault(PVOID arbitrary_va) {
    // Get pte from our va
    PPTE pte = get_pte_from_va(arbitrary_va);

    CRITICAL_SECTION* my_pte_lock = get_pte_lock_for_va(arbitrary_va);

    EnterCriticalSection(my_pte_lock);

    // Another thread beat us to it
    if (pte->hardware.valid == 1) {
        LeaveCriticalSection(my_pte_lock);
        return;
    }

    // 2. Check transition state next (Ignore disc completely here)
    if (pte->transition.transition == 1) {
        handle_soft_fault(pte, arbitrary_va, my_pte_lock);
    }
    // 3. If transition is 0, it is safe to read the disc bit payload
    else if (pte->disc.disc == 1) {
        handle_hard_fault(pte, arbitrary_va, my_pte_lock);
    }
    // 4. If valid=0, transition=0, and disc=0, it's a fresh allocation
    else if (pte->disc.disc == 0) {
        // Handle demand-zero fault if you have one, or handle as hard fault
        handle_hard_fault(pte, arbitrary_va, my_pte_lock);
    }
    else {
        printf("CRITICAL: Unrecognized PTE state!\n");
        LeaveCriticalSection(my_pte_lock);
        DebugBreak();
    }
}


VOID
full_virtual_memory_test_helper(int thread_number) {

    // Create the RNG state for this specific thread
    THREAD_RNG_STATE thread_rng;

    // Seed it using the CPU cycle count (or GetTickCount64() + ThreadId)
    thread_rng.state = __rdtsc();

    // Safety check: force to 1 if the seed somehow ended up exactly 0
    if (thread_rng.state == 0) {
        thread_rng.state = 1;
    }

    // Initialize the counter
    thread_rng.counter = 0;

    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    QueryPerformanceFrequency(&frequency);
    printf("Starting virtual memory simulation workload...for thread %i\n", thread_number);
    QueryPerformanceCounter(&start_time);

    unsigned i = 0;
    PVOID arbitrary_va;
    ULONG64 random_number;
    BOOL page_faulted = TRUE;
    BOOL resolved = TRUE;

    // Now perform random access
    while (i < (5 * (MB(1) / 1))) {

        if (resolved) {
            // Get random number
            random_number = GetNextRandom(&thread_rng);
            random_number %= virtual_address_size_in_unsigned_chunks;

            random_number &= ~0x7;
            arbitrary_va = va_start + random_number;

            resolved = FALSE;
        }

        page_faulted = FALSE;


        __try {
            ULONG_PTR current_value = *(volatile PULONG_PTR)arbitrary_va;

            // If the page isn't blank (0), it MUST match our VA
            if (current_value != 0 && current_value != (ULONG_PTR)arbitrary_va) {
                printf("CRITICAL: Data corruption! VA %p was overwritten with %p\n",
                       arbitrary_va, (PVOID)current_value);
                DebugBreak();
            }
            *(PULONG_PTR)arbitrary_va = (ULONG_PTR) arbitrary_va;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            page_faulted = TRUE;
        }

        // If faulted, handle it
        if (page_faulted) {
            handle_page_fault(arbitrary_va);
            continue;
        }
        else {
            CRITICAL_SECTION* my_pte_lock = get_pte_lock_for_va(arbitrary_va);

            PPTE pte = get_pte_from_va(arbitrary_va);

            if (pte->hardware.valid == 1) {
                pte->hardware.age = 1;
            }
        }


        resolved = TRUE;
        i++;
    }


    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("==============================================\n\n");

    printf ("full_virtual_memory_test : %d finished accessing %u random virtual addresses.\n", thread_number, i);
}

DWORD WINAPI AgeThreadWorker(LPVOID lpParam) {
    HANDLE waitEvents[2] = { StartAgingEvent, ShutdownEvent };
    static ULONG64 clock_hand = 0;

    while (TRUE) {
        DWORD wait = WaitForMultipleObjects(2, waitEvents, FALSE, 50);
        if (wait == WAIT_OBJECT_0 + 1) break;   // Shutdown

        // Sweep a healthy chunk. Large enough that pages age faster than
        // the workload dirties them; small enough not to hog region locks.
        const ULONG64 SWEEP = 256;
        ULONG64 processed = 0;

        while (processed < SWEEP && processed < num_pte_regions) {
            PPTE_REGION region = &pte_regions[clock_hand];

            if (TryEnterCriticalSection(&region->lock)) {
                for (int age = 7; age >= 0; age--) {
                    PLIST_ENTRY head = &region->active_age_lists[age];
                    if (IsListEmpty(head)) continue;

                    PLIST_ENTRY cur = head->Flink;
                    while (cur != head) {
                        PLIST_ENTRY next = cur->Flink;
                        pfn_metadata* pfn = GetPfnFromListEntry(cur);
                        PPTE pte = pfn->pte;

                        if (pte != NULL && pte->hardware.valid == 1) {
                            if (pte->hardware.age == 1) {
                                pte->hardware.age = 0;
                                if (age != 0) {
                                    RemoveEntryList(&pfn->list);
                                    InsertTailList(&region->active_age_lists[0], &pfn->list);
                                }
                            }
                            else if (age < 7) {
                                RemoveEntryList(&pfn->list);
                                InsertTailList(&region->active_age_lists[age + 1], &pfn->list);
                            }
                        }
                        cur = next;
                    }
                }
                LeaveCriticalSection(&region->lock);
            }

            clock_hand = (clock_hand + 1) % num_pte_regions;
            processed++;
        }

        // Only signal completion for an explicitly requested pass;
        // a timeout-driven sweep has no waiter.
        if (wait == WAIT_OBJECT_0) {
            SetEvent(FinishedAgingEvent);
        }
    }
    return 0;
}

DWORD WINAPI TrimThreadWorker(LPVOID lpParam) {
    HANDLE waitEvents[2] = { LowPagesEvent, ShutdownEvent };

    while (TRUE) {
        DWORD waitResult = WaitForMultipleObjects(2, waitEvents, FALSE, 100);

        if (waitResult == WAIT_OBJECT_0 + 1) {
            break; // Shutdown
        }

        SetEvent(StartAgingEvent);


        while (pfn_free_list.count + pfn_standby_list.count
               + pfn_modified_list.count < HIGH_WATERMARK) {

            if (WaitForSingleObject(ShutdownEvent, 0) == WAIT_OBJECT_0) {
                return 0;
            }

            int batch_count = 0;
            get_unmap_candidates(&batch_count, MAX_TRIM_PAGES);

            if (batch_count == 0) {
                break; // retry on next wake, ≤100ms away
            }

            SetEvent(PagesReadyForDiscEvent);
               }
    }
    return 0;
}

DWORD WINAPI DiscThreadWorker(LPVOID lpParam) {
    HANDLE waitEvents[2] = { PagesReadyForDiscEvent, ShutdownEvent };

    while (TRUE) {
        DWORD waitResult = WaitForMultipleObjects(2, waitEvents, FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0 + 1) break;

        if (waitResult == WAIT_OBJECT_0) {
            while (TRUE) {
                if (WaitForSingleObject(ShutdownEvent, 0) == WAIT_OBJECT_0) return 0;


                pfn_metadata* batch[DISC_BATCH];
                int batch_count = 0;

                // Phase 1: Harvest and Allocate
                while (batch_count < DISC_BATCH) {

                    PLIST_ENTRY mod_entry = LockedRemoveHead(&pfn_modified_list);

                    if (mod_entry == NULL) break;

                    EnterCriticalSection(&pfn_lock);
                    pfn_metadata* candidate = GetPfnFromListEntry(mod_entry);

                    candidate->isBeingWrittenToDisc = 1;
                    LeaveCriticalSection(&pfn_lock);

                    // THE FIX: Allocate the disc slot safely while protected by the PFN lock
                    if (candidate->disc_index == INVALID_DISC_SLOT) {
                        candidate->disc_index = find_free_disc_slot();
                        if (candidate->disc_index == INVALID_DISC_SLOT) {
                            // CATASTROPHIC FAILURE: We are completely out of disc space!
                            printf("CRITICAL: Out of disc space inside DiscThreadWorker!\n");
                            DebugBreak();
                        }

                    }

                    batch[batch_count] = candidate;
                    batch_count++;


                }



                if (batch_count == 0) {
                    break;
                }

                // Phase 2: The I/O Gap
                write_to_disc(batch, batch_count);

                // Phase 3: The Aftermath
                for (int i = 0; i < batch_count; i++) {
                    pfn_metadata* candidate = batch[i];
                    PPTE evict_pte = candidate->pte;
                    CRITICAL_SECTION* region_lock = get_pte_lock_from_pte_pointer(evict_pte);

                    EnterCriticalSection(region_lock);
                    EnterCriticalSection(&pfn_lock);
                    BOOL poached = (candidate->isBeingWrittenToDisc == 0);
                    candidate->isBeingWrittenToDisc = 0;

                    if (!poached) {
                        // Stamp the PTE to disc state while we hold the region lock
                        candidate->isOccupied = 3;
                        *(PULONG64)evict_pte = 0;
                        evict_pte->disc.disc = 1;
                        evict_pte->disc.disc_index = candidate->disc_index;
                    }
                    LeaveCriticalSection(&pfn_lock);
                    LeaveCriticalSection(region_lock);      // <-- release BEFORE any list lock

                    // Now, holding NO region lock, do the list work.
                    if (poached) {
                        EnterCriticalSection(&disc_lock);
                        disc_metadata[candidate->disc_index].isOccupied = FALSE;
                        LeaveCriticalSection(&disc_lock);
                        LockedInsertTail(&disc_free_list, &disc_metadata[candidate->disc_index].list);
                        candidate->disc_index = INVALID_DISC_SLOT;
                    } else {
                        LockedInsertTail(&pfn_standby_list, &candidate->list);
                        SetEvent(StandbyPageAvailableEvent);
                    }
                }
            }
        }
    }
    return 0;
}

VOID
full_virtual_memory_test (
    VOID
) {


    LARGE_INTEGER frequency;
    LARGE_INTEGER start_time;
    LARGE_INTEGER end_time;
    double elapsed_ms;

    QueryPerformanceFrequency(&frequency);
    printf("Starting virtual memory simulation workload...for general\n");
    QueryPerformanceCounter(&start_time);

    InitializeCriticalSectionAndSpinCount(&cs, 0x00FFFFFF);
    InitializeCriticalSectionAndSpinCount(&pfn_lock, 0x00FFFFFF);
    InitializeCriticalSectionAndSpinCount(&disc_lock, 0x00FFFFFF);



    if (set_up_program() == FALSE) {
        return;
    }

    // Create auto-reset events (they automatically reset to non-signaled after a thread wakes up)
    LowPagesEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    PagesReadyForDiscEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    StandbyPageAvailableEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    // Create a manual-reset event for shutdown so all threads can see it at once
    ShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    StartAgingEvent    = CreateEvent(NULL, FALSE, FALSE, NULL);
    FinishedAgingEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    HANDLE threads[4] = { NULL };
    threads[0] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE) full_virtual_memory_test_helper, (PVOID)1, 0, NULL);
    threads[1] = CreateThread(NULL, 0, TrimThreadWorker, NULL, 0, NULL);
    threads[2] = CreateThread(NULL, 0, DiscThreadWorker, NULL, 0, NULL);
    threads[3] = CreateThread(NULL, 0, AgeThreadWorker,  NULL, 0, NULL);

    full_virtual_memory_test_helper(0);

    WaitForSingleObject(threads[0], INFINITE);

    // 5. Now that the workload is done, tell the background threads to pack it up
    SetEvent(ShutdownEvent);

    WaitForMultipleObjects(3, &threads[1], TRUE, INFINITE);

    // Close all handles
    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
    CloseHandle(threads[2]);
    CloseHandle(threads[3]);
    CloseHandle(LowPagesEvent);
    CloseHandle(PagesReadyForDiscEvent);
    CloseHandle(StandbyPageAvailableEvent);
    CloseHandle(ShutdownEvent);
    CloseHandle(StartAgingEvent);
    CloseHandle(FinishedAgingEvent);

    // Stop timer and print result
    QueryPerformanceCounter(&end_time);

    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\n==============================================\n");
    printf("WORKLOAD COMPLETE\n");
    printf("Total execution time: %.2f ms\n", elapsed_ms);
    printf("==============================================\n\n");


    DeleteCriticalSection(&cs);
    for (ULONG64 i = 0; i < num_pte_regions; i++) {
        DeleteCriticalSection(&pte_regions[i].lock); // <- Updated target
    }
    DeleteCriticalSection(&pfn_lock);
    DeleteCriticalSection(&disc_lock);

    VirtualFree (va_start, 0, MEM_RELEASE);
    return;
}

VOID
main (
    int argc,
    char** argv
)
{
    full_virtual_memory_test ();
    return;
}