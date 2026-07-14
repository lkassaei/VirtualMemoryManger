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

#define NUMBER_OF_PHYSICAL_PAGES (1 * (MB(1024) / PAGE_SIZE))
//#define NUMBER_OF_PHYSICAL_PAGES 4
#define NUM_PTEs (VIRTUAL_ADDRESS_SIZE / PAGE_SIZE)
#define NUM_DISC_PAGES (5 * NUMBER_OF_PHYSICAL_PAGES)
#define MAX_DISC_PTE_BITS 40
#define MAX_DISC_SIZE ((ULONG64) 1 << MAX_DISC_PTE_BITS)
#define INVALID_DISC_SLOT ((1ULL << MAX_DISC_PTE_BITS) - 1)

#define DISC_BATCH 256                       // was 64 — amortize the map/unmap syscalls
#define MAX_TRIM_PAGES 256                   // match DISC_BATCH so one harvest = one disc batch
#define LOWEST_PAGES (NUMBER_OF_PHYSICAL_PAGES / 64)     // ~4,096: wake trim earlier
#define HIGH_WATERMARK (LOWEST_PAGES * 8)                // ~32,768: deeper buffer
#define MAX_IN_FLIGHT (4 * DISC_BATCH)                   // 1,024 — auto-scales with DISC_BATCH

#define PTE_REGION_SIZE 512

#define PFN_LOCK_STRIPES 256
#define PFN_LOCK(p) (&pfn_lock_stripes[(p)->frame_number & (PFN_LOCK_STRIPES - 1)])

#define STAGING_SLOTS (2 * (NUM_WORKER_THREADS + 1))   // 14 for 7 faulting threads

#define NUM_WORKER_THREADS 6

#define MIN_RUN_PAGES   16      // shortest linear run
#define MAX_RUN_PAGES   512     // longest linear run
#define REVISIT_CHANCE  4       // 1-in-N chance we jump BACK to a recent hot spot
#define HOT_SPOTS       8       // how many recent bases we remember


#define FRAME_IS_VALID(f) \
(((f) <= max_frame_number) && \
((frame_valid_bitmap[(f) >> 6] >> ((f) & 63)) & 1ULL))

#define DEBUG 1
#if DEBUG
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
    ULONG64 lock_bit: 1;              // reserved: future home of the 1-bit lock
    ULONG64 reserved: 20;
    CRITICAL_SECTION lock;            // delete once lock_bit works
} pfn_metadata;

static __forceinline VOID lock_pfn(pfn_metadata* p)   { EnterCriticalSection(&p->lock); }
static __forceinline VOID unlock_pfn(pfn_metadata* p) { LeaveCriticalSection(&p->lock); }

typedef struct _LOCKED_LIST {
    LIST_ENTRY head;          // flink/blink
    CRITICAL_SECTION lock;    // guards this list AND its count
    volatile LONG64 count;    // list length, updated only under lock
} LOCKED_LIST, *PLOCKED_LIST;

LOCKED_LIST pfn_free_list;
LOCKED_LIST pfn_modified_list;
LOCKED_LIST pfn_standby_list;
LOCKED_LIST disc_free_list;

CRITICAL_SECTION pfn_lock_stripes[PFN_LOCK_STRIPES];

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

PVOID staging_va_start;
volatile LONG staging_in_use[STAGING_SLOTS];

ULONG64* frame_valid_bitmap = NULL;


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

PLIST_ENTRY LockedRemoveHead(PLOCKED_LIST list) {
    PLIST_ENTRY entry = NULL;
    EnterCriticalSection(&list->lock);
    if (!IsListEmpty(&list->head)) {
        entry = RemoveHeadList(&list->head);
        list->count--;
        entry->Flink = NULL;      // mark unlinked
        entry->Blink = NULL;
    }
    LeaveCriticalSection(&list->lock);
    return entry;
}

VOID LockedRemoveEntry(PLOCKED_LIST list, PLIST_ENTRY entry) {
    EnterCriticalSection(&list->lock);

    // Guard: a node already removed has NULL links. Removing it again
    // dereferences NULL (Flink->Blink = ... → write to 0x8) and crashes.
    if (entry->Flink == NULL && entry->Blink == NULL) {
        printf("DOUBLE REMOVE: entry=%p already unlinked\n", entry);
        DebugBreak();
        LeaveCriticalSection(&list->lock);
        return;
    }

    RemoveEntryList(entry);
    list->count--;
    entry->Flink = NULL;
    entry->Blink = NULL;
    LeaveCriticalSection(&list->lock);
}

// Returns TRUE if we unlinked it; FALSE if it was already off the list.
BOOL LockedTryRemoveEntry(PLOCKED_LIST list, PLIST_ENTRY entry) {
    BOOL removed = FALSE;
    EnterCriticalSection(&list->lock);
    if (entry->Flink != NULL) {
        RemoveEntryList(entry);
        list->count--;
        entry->Flink = NULL;
        entry->Blink = NULL;
        removed = TRUE;
    }
    LeaveCriticalSection(&list->lock);
    return removed;
}

VOID LockedInsertTail(PLOCKED_LIST list, PLIST_ENTRY entry) {
    EnterCriticalSection(&list->lock);
    InsertTailList(&list->head, entry);
    list->count++;
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

pfn_metadata*
find_pfn_from_frame_number(ULONG64 frame_number) {
    if (!FRAME_IS_VALID(frame_number)) return NULL;
    return &frame_to_pfn_table[frame_number];
}

// Create our new pfn metadata sparse array
VOID
setup_pfn_metadata(ULONG_PTR physical_page_count, PULONG_PTR physical_page_numbers) {
    for (ULONG_PTR j = 0; j < physical_page_count; j++) {
        if (physical_page_numbers[j] > max_frame_number) {
            max_frame_number = physical_page_numbers[j];
        }
    }

    ULONG_PTR table_reserve_size = (max_frame_number + 1) * sizeof(pfn_metadata);

    frame_to_pfn_table = (pfn_metadata*)VirtualAlloc(
        NULL,
        table_reserve_size,
        MEM_RESERVE,
        PAGE_READWRITE
    );

    if (frame_to_pfn_table == NULL) {
        printf("CRITICAL: Failed to reserve tracking table. Error: %lu\n", GetLastError());
        DebugBreak();
        return;
    }

    // Validity bitmap — one bit per possible frame. ~600KB at max_frame=4.9M.
    ULONG_PTR bitmap_bytes = ((max_frame_number / 64) + 1) * sizeof(ULONG64);
    frame_valid_bitmap = zero_malloc(bitmap_bytes);

    for (ULONG_PTR j = 0; j < physical_page_count; j++) {
        ULONG64 frame = physical_page_numbers[j];

        ensure_metadata_slot_is_committed(frame_to_pfn_table, frame);

        pfn_metadata* p = &frame_to_pfn_table[frame];

        p->frame_number         = frame;
        p->pte                  = NULL;
        p->isOccupied           = 0;
        p->disc_index           = INVALID_DISC_SLOT;
        p->isBeingWrittenToDisc = 0;

        InitializeCriticalSectionAndSpinCount(&p->lock, 4000);

        frame_valid_bitmap[frame >> 6] |= (1ULL << (frame & 63));

        InitializeListHead(&p->list);
        LockedInsertTail(&pfn_free_list, &p->list);
    }

    MEMORY_BASIC_INFORMATION mbi;
    ULONG_PTR committed = 0;
    PCHAR addr = (PCHAR)frame_to_pfn_table;
    PCHAR end  = addr + table_reserve_size;
    while (addr < end && VirtualQuery(addr, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT) committed += mbi.RegionSize;
        addr += mbi.RegionSize;
    }
    printf("PFN table: committed %llu MB of %llu MB reserved (%llu frames)\n",
           (ULONG64)committed / MB(1),
           (ULONG64)table_reserve_size / MB(1),
           (ULONG64)physical_page_count);
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
    PLIST_ENTRY e = LockedRemoveHead(&disc_free_list);
    if (e == NULL) return INVALID_DISC_SLOT;
    PDISC_METADATA meta = CONTAINING_RECORD(e, DISC_METADATA, list);
    meta->isOccupied = TRUE;
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

int claim_staging_slot() {
    for (;;) {
        for (int i = 0; i < STAGING_SLOTS; i++) {
            if (InterlockedCompareExchange(&staging_in_use[i], 1, 0) == 0) {
                return i;
            }
        }
        YieldProcessor();   // all busy — brief spin; with 8 slots this is rare
    }
}
void release_staging_slot(int i) {
    InterlockedExchange(&staging_in_use[i], 0);
}

VOID write_to_disc(pfn_metadata** candidates, ULONG64 batch_count) {
    ULONG_PTR frame_array[DISC_BATCH];
    ULONG64 disc_slots[DISC_BATCH];
    // Prepare the localized I/O batch arrays — fill EVERY index, no skips.
    // A poached page carries INVALID_DISC_SLOT and that's fine; the copy
    // loop will skip it. frame_array must be fully populated regardless,
    // because the batch map needs a valid frame at every position.
    for (int i = 0; i < batch_count; i++) {
        disc_slots[i]  = candidates[i]->disc_index;
        frame_array[i] = candidates[i]->frame_number;
    }

    // Map the entire batch (frames are valid even for poached pages)
    if (MapUserPhysicalPages(temp_disc_va_start, batch_count, frame_array) == FALSE) {
        printf("CRITICAL: Failed to map batch scratch space! Error: %lu\n", GetLastError());
        DebugBreak();
        return;
    }

    // Copy each frame to its slot — HERE is where poached pages are skipped
    for (int i = 0; i < batch_count; i++) {
        if (disc_slots[i] == INVALID_DISC_SLOT) {
            continue;   // poached mid-batch; soft fault kept the memory copy
        }

        if (disc_slots[i] >= NUM_DISC_PAGES) {
            printf("CRITICAL: Invalid disc slot index detected! i: %d, slot: 0x%llx\n",
                   i, disc_slots[i]);
            DebugBreak();
            return;
        }

        void* source_va    = (char*)temp_disc_va_start + (i * PAGE_SIZE);
        void* disc_address = (char*)disc + (disc_slots[i] * PAGE_SIZE);
        memcpy(disc_address, source_va, PAGE_SIZE);
    }

    // Unmap scratch
    MapUserPhysicalPages(temp_disc_va_start, batch_count, NULL);


}

// Caller must hold region->lock.
static VOID harvest_one_bin(PPTE_REGION region, PLIST_ENTRY head,
                            int* batch_count, int batch_size) {

    // Locals for this bin's batch. Sized to the max we could ever take.
    pfn_metadata* candidates[MAX_TRIM_PAGES];
    PVOID          unmap_vas[MAX_TRIM_PAGES];
    ULONG64        saved_frames[MAX_TRIM_PAGES];
    int            n = 0;

    // ---- PASS 1: collect. Do NOT touch PTEs or unmap yet. ----
    while (!IsListEmpty(head) && (*batch_count + n) < batch_size && n < MAX_TRIM_PAGES) {
        PLIST_ENTRY entry = RemoveHeadList(head);
        entry->Flink = NULL;          // keep the unlinked-marker discipline
        entry->Blink = NULL;

        pfn_metadata* candidate = GetPfnFromListEntry(entry);
        PPTE evict_pte = candidate->pte;

        // Guard: only harvest genuinely active pages with valid PTEs.
        if (evict_pte == NULL ||
            evict_pte->hardware.valid != 1 ||
            candidate->isOccupied != 1) {
            printf("HARVEST STALE: pfn=%p occ=%d pte=%p valid=%d frame=0x%llx\n",
                   candidate, candidate->isOccupied, evict_pte,
                   evict_pte ? evict_pte->hardware.valid : -1,
                   (ULONG64)candidate->frame_number);
            DebugBreak();
            continue;   // already unlinked; drop it
        }

        candidates[n]   = candidate;
        unmap_vas[n]    = get_va_from_pte(evict_pte);
        saved_frames[n] = candidate->frame_number;
        n++;
    }

    if (n == 0) {
        return;
    }

    // ---- PASS 2: ONE batched unmap. All mappings die here, together. ----
    // Safe: every VA belongs to THIS region, whose lock we hold, so no
    // faulting thread can be mid-access on any of them.
    if (MapUserPhysicalPagesScatter(unmap_vas, n, NULL) == FALSE) {
        printf("CRITICAL: Batch unmap failed. n=%d Error: %lu\n", n, GetLastError());
        DebugBreak();
        // Do NOT stamp PTEs if the unmap failed — the mappings are still live.
        // Put the pages back on the age list so we don't leak them.
        for (int i = 0; i < n; i++) {
            InsertTailList(head, &candidates[i]->list);
        }
        return;
    }

    // ---- PASS 3: now that nothing is mapped, stamp PTEs and publish. ----
    for (int i = 0; i < n; i++) {
        pfn_metadata* candidate = candidates[i];
        PPTE evict_pte = candidate->pte;

        *(PULONG64)evict_pte = 0;
        evict_pte->transition.valid = 0;
        evict_pte->transition.transition = 1;
        evict_pte->transition.frame_number = saved_frames[i];

        lock_pfn(candidate);
        candidate->isOccupied = 2;        // set BEFORE the insert
        unlock_pfn(candidate);
        LockedInsertTail(&pfn_modified_list, &candidate->list);
    }

    *batch_count += n;
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

    staging_va_start = VirtualAlloc2(NULL, NULL,
                                 STAGING_SLOTS * PAGE_SIZE,
                                 MEM_RESERVE | MEM_PHYSICAL,
                                 PAGE_READWRITE,
                                 &temp_parameter, 1);   // same physical_page_handle!

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

    // Initialize the striped PFN locks (replaces the single global pfn_lock)
    for (int i = 0; i < PFN_LOCK_STRIPES; i++) {
        InitializeCriticalSectionAndSpinCount(&pfn_lock_stripes[i], 0x00FFFFFF);
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
    ULONG64 attempts = 0;
    for (;;) {
        if (++attempts > 16) return NULL;   // give up; caller waits/retries

        pfn_metadata* cand = NULL;
        PPTE owner_pte = NULL;
        int skip = attempts - 1;            // skip the ones we already failed on

        EnterCriticalSection(&pfn_standby_list.lock);
        PLIST_ENTRY e = pfn_standby_list.head.Flink;
        while (e != &pfn_standby_list.head) {
            pfn_metadata* p = GetPfnFromListEntry(e);
            if (p->isOccupied == 3 && p->isBeingWrittenToDisc == 0) {
                if (skip > 0) { skip--; }   // walk past previously-contended ones
                else { cand = p; owner_pte = p->pte; break; }
            }
            e = e->Flink;
        }
        LeaveCriticalSection(&pfn_standby_list.lock);

        if (cand == NULL) return NULL;

        // No owner PTE — nothing to stamp, just claim it.
        if (owner_pte == NULL) {
            EnterCriticalSection(&pfn_standby_list.lock);
            if (cand->isOccupied != 3 || cand->isBeingWrittenToDisc != 0 ||
                cand->pte != NULL) {
                LeaveCriticalSection(&pfn_standby_list.lock);
                continue;                      // changed under us; retry
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

        // Acquire the owner's region lock FIRST (no other locks held).
        // If it's our own lock, we already hold it — don't re-enter blindly;
        // CRITICAL_SECTION is recursive, but the re-verify below is what matters.
        if (owner_lock != my_pte_lock) {
            if (!TryEnterCriticalSection(owner_lock)) continue;
        }

        lock_pfn(cand);                        // region -> page

        if (cand->isOccupied != 3 || cand->isBeingWrittenToDisc != 0 ||
            cand->pte != owner_pte) {
            unlock_pfn(cand);
            if (owner_lock != my_pte_lock) LeaveCriticalSection(owner_lock);
            continue;
            }

        if (!LockedTryRemoveEntry(&pfn_standby_list, &cand->list)) {   // page -> list
            unlock_pfn(cand);
            if (owner_lock != my_pte_lock) LeaveCriticalSection(owner_lock);
            continue;
        }

        set_to_disc_state(owner_pte, cand);    // reads disc_index — BEFORE clearing it
        cand->disc_index = INVALID_DISC_SLOT;
        cand->isOccupied = 1;
        unlock_pfn(cand);

        cand->pte = NULL;                      // region-lock protected; we hold owner_lock
        if (owner_lock != my_pte_lock) LeaveCriticalSection(owner_lock);
        return cand;
    }
}

VOID
handle_hard_fault(PPTE pte, PVOID arbitrary_va, CRITICAL_SECTION* my_pte_lock) {
    // Already holding my_pte_lock on entry.
    // THIS PTE's disc slot — the data WE must read back. A frame pulled from
    // standby is a FRESH frame for our VA; its own old disc_index is irrelevant
    // to us (pull_from_standby_safely already repointed its previous owner).
    BOOL is_on_disc = (pte->disc.disc == 1);
    ULONG64 my_disc_slot = is_on_disc ? pte->disc.disc_index : INVALID_DISC_SLOT;

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
            //WaitForSingleObject(StandbyPageAvailableEvent, INFINITE);
            while (pfn_free_list.count == 0 && pfn_standby_list.count == 0) {
                WaitForSingleObject(StandbyPageAvailableEvent, 10);
            }
            EnterCriticalSection(my_pte_lock);

            if (pfn_free_list.count == 0 && pfn_standby_list.count == 0) {
                ResetEvent(StandbyPageAvailableEvent);
            }

            // The world changed while we slept. If another thread resolved
            // this VA, or trim converted it to transition, bail and let the
            // re-fault re-dispatch through handle_page_fault.
            if (pte->hardware.valid == 1 || pte->transition.transition == 1) {
                LeaveCriticalSection(my_pte_lock);
                return;
            }

            // Re-read THIS PTE's disc state — the slot may have changed.
            is_on_disc = (pte->disc.disc == 1);
            my_disc_slot = is_on_disc ? pte->disc.disc_index : INVALID_DISC_SLOT;
        }
    }

    // Wake trim if we just drained the free list low.
    if (pfn_free_list.count < LOWEST_PAGES) {
        SetEvent(LowPagesEvent);
    }

    // The frame we got is now exclusively ours; it should carry no disc slot.
    lock_pfn(target_pfn);
    target_pfn->disc_index = INVALID_DISC_SLOT;
    unlock_pfn(target_pfn);

    int slot = claim_staging_slot();
    PVOID staging_va = (char*)staging_va_start + (slot * PAGE_SIZE);

    PVOID va_aligned = (PVOID)((ULONG_PTR)arbitrary_va & ~((ULONG_PTR)PAGE_SIZE - 1));


    // 1. Map the frame at the PRIVATE staging VA — invisible to other threads.
    if (MapUserPhysicalPages(staging_va, 1, &target_pfn->frame_number) == FALSE) {
        release_staging_slot(slot);
        printf("Failed to map target window. VA %p Error: %lu\n", va_aligned, GetLastError());
        // P is half-pulled (occ=1, off all lists, not valid). Return it to the free
        // list cleanly rather than leaking it in a corrupt state.
        lock_pfn(target_pfn);
        target_pfn->isOccupied = 0;      // free
        target_pfn->disc_index = INVALID_DISC_SLOT;
        unlock_pfn(target_pfn);
        target_pfn->pte = NULL;

        LockedInsertTail(&pfn_free_list, &target_pfn->list);
        LeaveCriticalSection(my_pte_lock);
        return;
    }

    // 2. Fill it while it's private.
    if (is_on_disc && my_disc_slot != INVALID_DISC_SLOT) {
        memcpy(staging_va, (char*)disc + (my_disc_slot * PAGE_SIZE), PAGE_SIZE);
        EnterCriticalSection(&disc_lock);
        disc_metadata[my_disc_slot].isOccupied = FALSE;
        LeaveCriticalSection(&disc_lock);
        LockedInsertTail(&disc_free_list, &disc_metadata[my_disc_slot].list);
    } else {
        memset(staging_va, 0, PAGE_SIZE);
    }

    // 3. Unmap from staging.
    MapUserPhysicalPages(staging_va, 1, NULL);
    release_staging_slot(slot);

    // 4. NOW publish: map at the real VA. The first moment any other thread can
    //    see this frame at this VA, it already holds the correct data.
    if (MapUserPhysicalPages(va_aligned, 1, &target_pfn->frame_number) == FALSE) {
        lock_pfn(target_pfn);
        target_pfn->isOccupied = 0;      // free
        target_pfn->disc_index = INVALID_DISC_SLOT;
        unlock_pfn(target_pfn);
        target_pfn->pte = NULL;

        LockedInsertTail(&pfn_free_list, &target_pfn->list);
        LeaveCriticalSection(my_pte_lock);
        return;
    }

    // 5. PTE valid, age list, as before.

    // Make the PTE valid FIRST.
    *(PULONG64)pte = 0;
    set_pte_valid(pte, target_pfn->frame_number);
    pte->hardware.age = 0;

    // THEN activate and publish to the age list. Now any harvester that
    // walks this list sees occ=1 AND valid=1 — invariant holds.
    lock_pfn(target_pfn);
    target_pfn->isOccupied = 1;
    unlock_pfn(target_pfn);

    target_pfn->pte = pte;

    ULONG64 region_index = (pte - page_table) / PTE_REGION_SIZE;
    if (target_pfn->list.Flink != NULL || target_pfn->list.Blink != NULL) {
        printf("DOUBLE INSERT: pfn=%p Flink=%p Blink=%p occ=%d pte=%p\n", target_pfn, target_pfn->list.Flink, target_pfn->list.Blink,
               target_pfn->isOccupied, target_pfn->pte);
        DebugBreak();
    }
    InsertTailList(&pte_regions[region_index].active_age_lists[0], &target_pfn->list);

    LeaveCriticalSection(my_pte_lock);
}

VOID
handle_soft_fault(PPTE pte, PVOID arbitrary_va, CRITICAL_SECTION* my_pte_lock) {
    ULONG64 frame_number = pte->transition.frame_number;
    pfn_metadata* target_pfn = find_pfn_from_frame_number(frame_number);

    if (target_pfn == NULL) {
        printf("SOFT FAULT: no PFN for frame 0x%llx pte=%p\n", frame_number, pte);
        DebugBreak();
        LeaveCriticalSection(my_pte_lock);
        return;
    }

    lock_pfn(target_pfn);                              // region -> page

    if (target_pfn->pte != pte) {
        unlock_pfn(target_pfn);
        LeaveCriticalSection(my_pte_lock);
        return;
    }

    ULONG64 slot_to_free = INVALID_DISC_SLOT;

    if (target_pfn->isBeingWrittenToDisc == 1) {
        target_pfn->isBeingWrittenToDisc = 0;          // poach
        slot_to_free  = target_pfn->disc_index;
        target_pfn->disc_index = INVALID_DISC_SLOT;
    }
    else if (target_pfn->isOccupied == 3) {
        if (LockedTryRemoveEntry(&pfn_standby_list, &target_pfn->list)) {
            slot_to_free  = target_pfn->disc_index;
            target_pfn->disc_index = INVALID_DISC_SLOT;
        }
    }
    else if (target_pfn->isOccupied == 2) {
        LockedTryRemoveEntry(&pfn_modified_list, &target_pfn->list);
    }

    target_pfn->isOccupied = 1;                        // the signal disc Phase 1 re-checks
    unlock_pfn(target_pfn);

    if (slot_to_free != INVALID_DISC_SLOT) {
        EnterCriticalSection(&disc_lock);
        disc_metadata[slot_to_free].isOccupied = FALSE;
        LeaveCriticalSection(&disc_lock);
        LockedInsertTail(&disc_free_list, &disc_metadata[slot_to_free].list);
    }

    // Remap the frame and validate the PTE. Order: make PTE valid BEFORE
    // publishing to the age list, so no harvester can see occ=1 / valid=0.
    PVOID va_aligned = (PVOID)((ULONG_PTR)arbitrary_va & ~((ULONG_PTR)PAGE_SIZE - 1));

    if (MapUserPhysicalPages(va_aligned, 1, &target_pfn->frame_number) == FALSE) {
        printf("CRITICAL: soft fault remap failed. Error: %lu\n", GetLastError());
        DebugBreak();
        // Return the page cleanly rather than leaking it half-activated.
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

    //EnterCriticalSection(&pfn_lock);
    target_pfn->pte = pte;
    //LeaveCriticalSection(&pfn_lock);

    ULONG64 region_index = (pte - page_table) / PTE_REGION_SIZE;
    if (target_pfn->list.Flink != NULL &&
        target_pfn->list.Flink != &target_pfn->list) {
        // Entry appears already linked into a list — double insert!
        printf("DOUBLE INSERT: pfn=%p Flink=%p Blink=%p occ=%d pte=%p\n",
               target_pfn, target_pfn->list.Flink, target_pfn->list.Blink,
               target_pfn->isOccupied, target_pfn->pte);
        DebugBreak();
        }
    InsertTailList(&pte_regions[region_index].active_age_lists[0], &target_pfn->list);

    LeaveCriticalSection(my_pte_lock);
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
    ULONG64 runtime = (1 * (MB(1) / 1));

    // Now perform random access
    while (i < runtime) {

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
            *(PULONG_PTR)arbitrary_va = (ULONG_PTR) arbitrary_va;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
          ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
              page_faulted = TRUE;
        }

        // If faulted, handle it
        if (page_faulted) {
            handle_page_fault(arbitrary_va);
            continue;
        }
        else {
            PPTE pte = get_pte_from_va(arbitrary_va);

            // Set the age hint bit atomically. A plain bitfield write is a
            // full-qword RMW that can resurrect a stale VALID pte over a
            // concurrent trim's TRANSITION stamp. CAS only succeeds if the
            // PTE is still the exact valid value we read; otherwise skip —
            // it's just a hint.
            ULONG64 old_val = *(volatile ULONG64*)pte;

            PTE snapshot;
            *(PULONG64)&snapshot = old_val;

            if (snapshot.hardware.valid == 1 && snapshot.hardware.age == 0) {
                PTE updated = snapshot;
                updated.hardware.age = 1;

                InterlockedCompareExchange64(
                    (volatile LONG64*)pte,
                    *(PLONG64)&updated,
                    (LONG64)old_val);
                // Failure is fine: the PTE changed under us (trimmed, or another
                // access already set the bit). Never retry — just move on.
            }
        }

        if (i> 0 && i % (runtime / 100) == 0) {
            printf(".");
            fflush(stdout);
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

    // Ring of recently-visited bases. Revisiting these is what creates the
    // hot/cold split that aging is supposed to detect.
    ULONG64 hot[HOT_SPOTS] = { 0 };
    int      hot_next = 0;
    int      hot_count = 0;

    ULONG64 base_page   = GetNextRandom(&thread_rng) % total_pages;
    ULONG64 run_left    = 0;
    ULONG64 cur_page    = base_page;

    unsigned i = 0;
    PVOID arbitrary_va;
    BOOL page_faulted;
    BOOL resolved = TRUE;

    ULONG64 runtime = (10 * (MB(1) / 1));

    while (i < runtime) {

        if (resolved) {
            if (run_left == 0) {
                // Start a new run.
                ULONG64 r = GetNextRandom(&thread_rng);

                if (hot_count > 0 && (r % REVISIT_CHANCE) == 0) {
                    // Jump back to a recent base — these pages should still be
                    // young, and aging should keep them out of the high bins.
                    base_page = hot[GetNextRandom(&thread_rng) % hot_count];
                } else {
                    // Cold jump: somewhere new entirely.
                    base_page = GetNextRandom(&thread_rng) % total_pages;

                    hot[hot_next] = base_page;
                    hot_next = (hot_next + 1) % HOT_SPOTS;
                    if (hot_count < HOT_SPOTS) hot_count++;
                }

                run_left = MIN_RUN_PAGES +
                           (GetNextRandom(&thread_rng) % (MAX_RUN_PAGES - MIN_RUN_PAGES));

                // Don't run off the end of the VA space.
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
            *(PULONG_PTR)arbitrary_va = (ULONG_PTR) arbitrary_va;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
            page_faulted = TRUE;
        }

        if (page_faulted) {
            handle_page_fault(arbitrary_va);
            continue;   // retry the SAME va — do not advance
        }

        // Age hint, same CAS discipline as before.
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

        // Advance within the run.
        cur_page++;
        run_left--;
        resolved = TRUE;

        if (i > 0 && i % (runtime/100) == 0) {
            printf(".");
            fflush(stdout);
        }
        i++;
    }

    QueryPerformanceCounter(&end_time);
    elapsed_ms = (double)(end_time.QuadPart - start_time.QuadPart) * 1000.0 / frequency.QuadPart;

    printf("\nthread %d: %u accesses in %.2f ms\n", thread_number, i, elapsed_ms);
}

DWORD WINAPI AgeThreadWorker(LPVOID lpParam) {
    HANDLE waitEvents[2] = { StartAgingEvent, ShutdownEvent };
    static ULONG64 clock_hand = 0;

    while (TRUE) {
        DWORD wait = WaitForMultipleObjects(2, waitEvents, FALSE, 50);
        if (wait == WAIT_OBJECT_0 + 1) break;   // Shutdown

        const ULONG64 SWEEP = num_pte_regions;
        ULONG64 processed = 0;

        while (processed < SWEEP && processed < num_pte_regions) {
            PPTE_REGION region = &pte_regions[clock_hand];

            if (TryEnterCriticalSection(&region->lock)) {
                PLIST_ENTRY bin0_head  = &region->active_age_lists[0];
                PLIST_ENTRY bin0_stop  = bin0_head->Blink;   // last pre-existing entry
                BOOL bin0_was_empty    = IsListEmpty(bin0_head);

                // Descending is REQUIRED: we promote into bin (age+1), which
                // we have already walked. Ascending would re-walk promoted
                // pages and loop forever.
                for (int age = 7; age >= 1; age--) {
                    PLIST_ENTRY head = &region->active_age_lists[age];
                    if (IsListEmpty(head)) continue;

                    ULONG64 walked = 0;
                    PLIST_ENTRY cur = head->Flink;

                    while (cur != head) {

                        // GUARD 1: bounded walk. An age list can never hold more
                        // entries than there are physical frames. Exceeding that
                        // means the links form a cycle that never reaches head.
                        if (++walked > NUMBER_OF_PHYSICAL_PAGES) {
                            printf("AGE LIST CYCLE: region=%p bin=%d walked=%llu cur=%p\n",
                                   region, age, walked, cur);
                            DebugBreak();
                            break;
                        }

                        // GUARD 2: self-linked node. cur->Flink == cur means
                        // next == cur, so the walk never advances — this is the
                        // exact hang we hit. Never follow it.
                        if (cur->Flink == cur) {
                            printf("SELF-LINKED PFN: entry=%p Flink=%p Blink=%p\n",
                                   cur, cur->Flink, cur->Blink);
                            DebugBreak();
                            break;
                        }

                        // GUARD 3: half-unlinked node. The removal helpers null
                        // both links; a node with one NULL link is caught mid-op
                        // or corrupt. Following it dereferences NULL.
                        if (cur->Flink == NULL || cur->Blink == NULL) {
                            printf("AGE: half-unlinked entry=%p Flink=%p Blink=%p\n",
                                   cur, cur->Flink, cur->Blink);
                            DebugBreak();
                            break;
                        }

                        PLIST_ENTRY next = cur->Flink;
                        pfn_metadata* pfn = GetPfnFromListEntry(cur);

                        PPTE pte = pfn->pte;

                        // Anything in this region's age list must be active and owned by this region.
                        // If not, it's a state we don't own — leave it alone entirely.
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
                if (!bin0_was_empty) {
                    PLIST_ENTRY cur = bin0_head->Flink;
                    ULONG64 walked = 0;
                    for (;;) {
                        if (cur == bin0_head) break;
                        if (++walked > NUMBER_OF_PHYSICAL_PAGES) { DebugBreak(); break; }
                        if (cur->Flink == NULL || cur->Blink == NULL || cur->Flink == cur) { DebugBreak(); break; }

                        PLIST_ENTRY next    = cur->Flink;
                        BOOL        is_last = (cur == bin0_stop);   // check BEFORE we move it

                        pfn_metadata* pfn = GetPfnFromListEntry(cur);
                        PPTE pte = pfn->pte;

                        if (pfn->isOccupied == 1 && pte != NULL &&
                            get_pte_lock_from_pte_pointer(pte) == &region->lock &&
                            pte->hardware.valid == 1) {

                            if (pte->hardware.age == 1) {
                                pte->hardware.age = 0;              // touched; stays in bin 0
                            } else {
                                RemoveEntryList(&pfn->list);
                                InsertTailList(&region->active_age_lists[1], &pfn->list);
                            }
                            }

                        if (is_last) break;                          // reached the snapshot boundary
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

DWORD WINAPI TrimThreadWorker(LPVOID lpParam) {
    HANDLE waitEvents[2] = { LowPagesEvent, ShutdownEvent };


    while (TRUE) {
        DWORD waitResult = WaitForMultipleObjects(2, waitEvents, FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0 + 1) {
            break; // Shutdown
        }

        SetEvent(StartAgingEvent);
        WaitForSingleObject(FinishedAgingEvent, 100);

        while (pfn_free_list.count + pfn_standby_list.count < HIGH_WATERMARK) {
            if (WaitForSingleObject(ShutdownEvent, 0) == WAIT_OBJECT_0) return 0;

            // Don't outrun the disc thread — but don't stop working either.
            if (pfn_modified_list.count >= MAX_IN_FLIGHT) {
                SetEvent(PagesReadyForDiscEvent);
                Sleep(0);                 // yield; let disc drain
                continue;
            }

            int batch_count = 0;
            get_unmap_candidates(&batch_count, MAX_TRIM_PAGES);

            if (batch_count == 0) {
                break;   // genuinely nothing harvestable; retry on next wake
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

                    pfn_metadata* c = GetPfnFromListEntry(mod_entry);
                    lock_pfn(c);

                    if (c->isOccupied != 2) {          // soft fault rescued it in the pop gap
                        unlock_pfn(c);
                        continue;
                    }

                    c->isBeingWrittenToDisc = 1;

                    if (c->disc_index == INVALID_DISC_SLOT) {
                        c->disc_index = find_free_disc_slot();
                        if (c->disc_index == INVALID_DISC_SLOT) {
                            c->isBeingWrittenToDisc = 0;
                            unlock_pfn(c);
                            LockedInsertTail(&pfn_modified_list, &c->list);   // PUT IT BACK
                            break;
                        }
                    }

                    unlock_pfn(c);
                    batch[batch_count++] = c;
                }

                if (batch_count == 0) {
                    break;
                }

                // Phase 2: The I/O Gap
                write_to_disc(batch, batch_count);

                // Phase 3: The Aftermath
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

    printf("max_frame=%llu  slots=%llu  table=%llu MB\n",
       max_frame_number, max_frame_number + 1,
       ((max_frame_number + 1) * sizeof(pfn_metadata)) / MB(1));

    // Create auto-reset events (they automatically reset to non-signaled after a thread wakes up)
    LowPagesEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    PagesReadyForDiscEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    StandbyPageAvailableEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    // Create a manual-reset event for shutdown so all threads can see it at once
    ShutdownEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    StartAgingEvent    = CreateEvent(NULL, FALSE, FALSE, NULL);
    FinishedAgingEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // Workers at 0..NUM_WORKER_THREADS-1, background threads after.
    HANDLE threads[NUM_WORKER_THREADS + 3] = { NULL };   // 6 + 3 = 9 slots, indices 0..8

    // Background threads at the top slots:
    threads[NUM_WORKER_THREADS + 0] = CreateThread(NULL, 0, TrimThreadWorker, NULL, 0, NULL);
    threads[NUM_WORKER_THREADS + 1] = CreateThread(NULL, 0, DiscThreadWorker, NULL, 0, NULL);
    threads[NUM_WORKER_THREADS + 2] = CreateThread(NULL, 0, AgeThreadWorker,  NULL, 0, NULL);

    // Workers at 0..NUM_WORKER_THREADS-1. Note: main runs helper(0) inline, so the
    // created workers take VA-thread-numbers 1..NUM_WORKER_THREADS.
    for (int i = 0; i < NUM_WORKER_THREADS; i++) {
        threads[i] = CreateThread(NULL, 0,
                                  (LPTHREAD_START_ROUTINE) full_virtual_memory_test_helper_not_random,
                                  (PVOID)(i + 1), 0, NULL);
    }

    // Main thread also runs the workload (thread number 0).
    full_virtual_memory_test_helper_not_random(0);

    // Wait for ALL created workers to finish (indices 0..NUM_WORKER_THREADS-1).
    WaitForMultipleObjects(NUM_WORKER_THREADS, &threads[0], TRUE, INFINITE);

    // Now tell background threads to stop.
    SetEvent(ShutdownEvent);

    // Wait for the 3 background threads (trim, disc, age).
    WaitForMultipleObjects(3, &threads[NUM_WORKER_THREADS], TRUE, INFINITE);

    for (int i = 0; i < NUM_WORKER_THREADS + 3; i++) {
        if (threads[i]) CloseHandle(threads[i]);
    }
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
    for (ULONG64 f = 0; f <= max_frame_number; f++) {
        if (FRAME_IS_VALID(f)) {
            DeleteCriticalSection(&frame_to_pfn_table[f].lock);
        }
    }
    free(frame_valid_bitmap);
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