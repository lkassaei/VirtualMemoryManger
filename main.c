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



#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

//
// This define enables code that lets us create multiple virtual address
// mappings to a single physical page.  We only/need want this if/when we
// start using reference counts to avoid holding locks while performing
// pagefile I/Os - because otherwise disallowing this makes it easier to
// detect and fix unintended failures to unmap virtual addresses properly.
//

#define SUPPORT_MULTIPLE_VA_TO_SAME_PAGE 0

#pragma comment(lib, "advapi32.lib")

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
#pragma comment(lib, "onecore.lib")
#endif

#define PAGE_SIZE                   4096

#define MB(x)                       ((x) * 1024 * 1024)
#define KB(x)                       ((x) * 1024)


//
// This is intentionally a power of two so we can use masking to stay
// within bounds.
//

// #define VIRTUAL_ADDRESS_SIZE        MB(16)
//#define VIRTUAL_ADDRESS_SIZE        KB(16)

//#define VIRTUAL_ADDRESS_SIZE_IN_UNSIGNED_CHUNKS        (VIRTUAL_ADDRESS_SIZE / sizeof (ULONG_PTR))

//
// Deliberately use a physical page pool that is approximately 1% of the
// virtual address space !
//

// #define NUMBER_OF_PHYSICAL_PAGES   ((VIRTUAL_ADDRESS_SIZE / PAGE_SIZE) / 64)
 #define NUMBER_OF_PHYSICAL_PAGES   (MB(1024) / PAGE_SIZE)

#define NUM_PTEs (VIRTUAL_ADDRESS_SIZE / PAGE_SIZE)

#define NUM_DISC_PAGES  (3 * NUMBER_OF_PHYSICAL_PAGES)

#define MAX_DISC_PTE_BITS 40

#define MAX_DISC_SIZE ((ULONG64) 1 << MAX_DISC_PTE_BITS)

#define MAX_TRIM_PAGES 64

#define DEBUG 1
#if defined(DEBUG)
#define ASSERT(condition) \
    if ((condition)== FALSE) { \
        DebugBreak(); \
    }
#else
#define ASSERT(condition)
#endif

#if 0
typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink;
    struct _LIST_ENTRY *Blink;
} LIST_ENTRY, *PLIST_ENTRY;
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

    //
    // Insert a new entry at the tail.
    //

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

    //
    // Remove the entry currently at the head of the list.
    //

    Entry = ListHead->Flink;
    Flink = Entry->Flink;
    ListHead->Flink = Flink;
    Flink->Blink = ListHead;

    return Entry;
}

// Use on rescue
BOOLEAN
RemoveEntryList (
    PLIST_ENTRY Entry
    )
{
    PLIST_ENTRY Blink;
    PLIST_ENTRY Flink;

    //
    // Remove the caller's known entry.
    //

    Flink = Entry->Flink;
    Blink = Entry->Blink;
    Blink->Flink = Flink;
    Flink->Blink = Blink;

    //
    // Return whether list is now empty.
    //

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

// SAME 8 BYTES DIFFERENT VIEWS
typedef struct {
    union {
        VALID_PTE hardware;
        TRANSITION_PTE transition;
        DISC_PTE disc;
        //ZERO_PTE zero;
    };
} PTE, *PPTE;

PPTE page_table;

// Struct for our PFNs
typedef struct {
    LIST_ENTRY list;
    ULONG64 frame_number;
    PPTE pte;
    ULONG64 disc_index: MAX_DISC_PTE_BITS;
    ULONG64 isOccupied: 2; // 00 = free, 01 = active, 10 = modified, 11 = standby
} pfn_metadata;

LIST_ENTRY pfn_active_list;
LIST_ENTRY pfn_modified_list;
LIST_ENTRY pfn_standby_list;

// LK make constants for pfn states and fix

// This array represents our physical memory slots
pfn_metadata physical_pool[NUMBER_OF_PHYSICAL_PAGES] = { 0 };

// Inline helper to back-step from a List Entry pointer to the metadata struct
pfn_metadata*
GetPfnFromListEntry(PLIST_ENTRY entry) {
    if (entry == NULL) return NULL;
    return CONTAINING_RECORD(entry, pfn_metadata, list);
}

pfn_metadata*
find_pfn_from_frame_number(ULONG64 frame_number, int physical_page_count) {
    int count = 0;
    for (int i = 0; i < physical_page_count; i++) {
        if (physical_pool[i].frame_number
        == frame_number) {
            return &physical_pool[i];
        }
        count++;
    }
    ASSERT(count != NUMBER_OF_PHYSICAL_PAGES);
    return NULL;
}

VOID
setup_pfn_metadata(ULONG_PTR physical_page_count, PULONG_PTR physical_page_numbers) {
    // Clear pool metadata states explicitly
    for (int j = 0; j < physical_page_count; j++) {
        physical_pool[j].frame_number = physical_page_numbers[j];
        physical_pool[j].isOccupied = 0; // 0 = Clean/Free
        physical_pool[j].pte = NULL;
        InitializeListHead(&physical_pool[j].list);
    }
}

PVOID
zero_malloc(size_t num_bytes) {
    PVOID p = malloc(num_bytes);
    if (p == NULL) {
        DebugBreak();
    }
    memset(p, 0, num_bytes);
    return p;
}

PULONG_PTR va_start;

PPTE
get_pte_from_va(PULONG_PTR arbitrary_va) {
    ULONG64 byte_distance = (ULONG64)arbitrary_va - (ULONG64)va_start;
    ULONG64 index = byte_distance / PAGE_SIZE;
    return &page_table[index];
}

PULONG_PTR
get_va_from_pte(PPTE pte) {
    ULONG64 pte_index = pte - page_table;
    // Calculate the raw byte offset first, then apply it to the base address
    ULONG_PTR byte_offset = pte_index * PAGE_SIZE;
    return (PULONG_PTR)((ULONG_PTR)va_start + byte_offset);
}

VOID
set_pte_valid(PPTE pte, ULONG64 pfn) {
    pte->hardware.frame_number = pfn;
    pte->hardware.valid = TRUE;
}

VOID
set_pte_invalid(PPTE pte) {
    pte->hardware.valid = FALSE;
}

VOID
increment_pte_age(PVALID_PTE pte) {
    if (pte->age < 7) {
        pte->age++;
    }
}

pfn_metadata*
get_free_page() {
    pfn_metadata* target_pfn = NULL;
    for (int j = 0; j < NUMBER_OF_PHYSICAL_PAGES; j++) {
        if (physical_pool[j].isOccupied == 0) {
            target_pfn = &physical_pool[j];
            break;
        }
    }
    return target_pfn;
}

VOID
set_to_disc_state(PPTE old_owner_pte, pfn_metadata* target_pfn) {
    old_owner_pte->hardware.valid = 0;
    old_owner_pte->disc.transition = 0; // No longer transitioning
    old_owner_pte->disc.disc = 1;       // Strictly on disk now
    old_owner_pte->disc.disc_index = target_pfn->disc_index;
}


PBOOLEAN disc_metadata;
// Create fake disk
PVOID
create_page_file(PULONG64 number_of_pages) {
    PVOID p;
    ULONG64 num_bytes;

    if (*number_of_pages > MAX_DISC_SIZE) {
        *number_of_pages = MAX_DISC_SIZE;
    }
    num_bytes = *number_of_pages * PAGE_SIZE;

    p = malloc(num_bytes);
    while (p == NULL) {
        num_bytes /= 2;
        p = malloc(num_bytes);
    }
    *number_of_pages = num_bytes / PAGE_SIZE;
    disc_metadata = malloc(*number_of_pages);
    memset(disc_metadata, 0, *number_of_pages);
    if (disc_metadata == NULL) {
        printf("could not allocate disc_metadata and fake disc\n");
    }


    return p;

}

int find_free_disc_slot(int disc_page_count) {
    for (int i = 0; i < NUM_DISC_PAGES; i++) {
        if (disc_metadata[i] == FALSE) { // FALSE means slot is free
            disc_metadata[i] = TRUE;     // Claim it
            return i;
        }
    }
    return -1; // Disk full!
}

VOID
get_unmap_candidates(int* batch_count, int batch_size,
    ULONG_PTR physical_page_count, PULONG_PTR* unmap_batch,
    ULONG64 disc_page_count, PVOID disc) {
    while (*batch_count < batch_size) {
        int oldest_slot = -1;
        ULONG64 highest_age = 0;

        // Scan specifically for live, ACTIVE (1) frames to find the oldest
        for (int j = 0; j < physical_page_count; j++) {
            if (physical_pool[j].isOccupied == 1 && physical_pool[j].pte != NULL) {
                ULONG64 current_age = physical_pool[j].pte->hardware.age;

                if (current_age >= highest_age) {
                    PULONG_PTR candidate_va = get_va_from_pte(physical_pool[j].pte);
                    BOOL already_batched = FALSE;
                    for (int k = 0; k < *batch_count; k++) {
                        if (unmap_batch[k] == candidate_va) {
                            already_batched = TRUE;
                            break;
                        }
                    }

                    if (!already_batched) {
                        highest_age = current_age;
                        oldest_slot = j;
                    }
                }
            }
        }

        // Transition the chosen old page into the Modified state because we will unmap
        if (oldest_slot != -1) {
            pfn_metadata* evict_pfn = &physical_pool[oldest_slot];
            PPTE evict_pte = evict_pfn->pte;
            PULONG_PTR evict_va = get_va_from_pte(evict_pte);

            unmap_batch[*batch_count] = evict_va;
            (*batch_count)++;

            // Remove from active list tracking
            RemoveEntryList(&evict_pfn->list);

            // Move to MODIFIED state and list (Marked as trimmed, but data not yet written)
            evict_pfn->isOccupied = 2;
            InsertTailList(&pfn_modified_list, &evict_pfn->list);

            // Find disc slot to transfer that data to later
            int disc_slot = find_free_disc_slot(disc_page_count);
            if (disc_slot != -1) {
                // Get disc address
                void* disc_address = (char*)disc + (disc_slot * PAGE_SIZE);

                // Copy data while physical mapping is fully valid and readable
                memcpy(disc_address, (void*)evict_va, PAGE_SIZE);

                // Mark software PTE as an invalid transition entry pointing to the disk index
                evict_pte->hardware.valid = 0;
                evict_pte->disc.transition = 0;
                evict_pte->disc.disc = 1;
                evict_pte->disc.disc_index = disc_slot;

                evict_pfn->disc_index = disc_slot;
            } else {
                printf("CRITICAL: Swap space exhaustion during trim phase!\n");
                return;
            }
        } else {
            break;
        }
    }
}


BOOL
GetPrivilege  (
    VOID
    )
{
    struct {
        DWORD Count;
        LUID_AND_ATTRIBUTES Privilege [1];
    } Info;

    //
    // This is Windows-specific code to acquire a privilege.
    // Understanding each line of it is not so important for
    // our efforts.
    //

    HANDLE hProcess;
    HANDLE Token;
    BOOL Result;

    //
    // Open the token.
    //

    hProcess = GetCurrentProcess ();

    Result = OpenProcessToken (hProcess,
                               TOKEN_ADJUST_PRIVILEGES,
                               &Token);

    if (Result == FALSE) {
        printf ("Cannot open process token.\n");
        return FALSE;
    }

    //
    // Enable the privilege.
    //

    Info.Count = 1;
    Info.Privilege[0].Attributes = SE_PRIVILEGE_ENABLED;

    //
    // Get the LUID.
    //

    Result = LookupPrivilegeValue (NULL,
                                   SE_LOCK_MEMORY_NAME,
                                   &(Info.Privilege[0].Luid));

    if (Result == FALSE) {
        printf ("Cannot get privilege\n");
        return FALSE;
    }

    //
    // Adjust the privilege.
    //

    Result = AdjustTokenPrivileges (Token,
                                    FALSE,
                                    (PTOKEN_PRIVILEGES) &Info,
                                    0,
                                    NULL,
                                    NULL);

    //
    // Check the result.
    //

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

    //
    // Create an AWE section.  Later we deposit pages into it and/or
    // return them.
    //

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

VOID
full_virtual_memory_test (
    VOID
    ) {
    unsigned i;
    PULONG_PTR arbitrary_va;
    unsigned random_number;
    BOOL allocated;
    BOOL page_faulted;
    BOOL privilege;
    BOOL obtained_pages;
    ULONG_PTR physical_page_count;
    PULONG_PTR physical_page_numbers;
    HANDLE physical_page_handle;
    ULONG_PTR virtual_address_size;
    ULONG_PTR virtual_address_size_in_unsigned_chunks;

    InitializeListHead(&pfn_active_list);
    InitializeListHead(&pfn_modified_list);
    InitializeListHead(&pfn_standby_list);

    //
    // Allocate the physical pages that we will be managing.
    //
    // First acquire privilege to do this since physical page control
    // is typically something the operating system reserves the sole
    // right to do.
    //

    privilege = GetPrivilege ();

    if (privilege == FALSE) {
        printf ("full_virtual_memory_test : could not get privilege\n");
        return;
    }

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE

    physical_page_handle = CreateSharedMemorySection ();

    if (physical_page_handle == NULL) {
        printf ("CreateFileMapping2 failed, error %#x\n", GetLastError ());
        return;
    }

#else

    physical_page_handle = GetCurrentProcess ();

#endif

    physical_page_count = NUMBER_OF_PHYSICAL_PAGES;

    // Create array for PFNs
    physical_page_numbers = malloc (physical_page_count * sizeof (ULONG_PTR));

    if (physical_page_numbers == NULL) {
        printf ("full_virtual_memory_test : could not allocate array to hold physical page numbers\n");
        return;
    }



    // Put PFNs in our array, so we can keep track of which pages we can use
    // & specifies the address (for any var not just pointers)
    allocated = AllocateUserPhysicalPages (physical_page_handle,
                                           &physical_page_count,
                                           physical_page_numbers);

    if (allocated == FALSE) {
        printf ("full_virtual_memory_test : could not allocate physical pages\n");
        return;
    }

    if (physical_page_count != NUMBER_OF_PHYSICAL_PAGES) {

        printf ("full_virtual_memory_test : allocated only %llu pages out of %u pages requested\n",
                physical_page_count,
                NUMBER_OF_PHYSICAL_PAGES);

        if (physical_page_count == 0) {
            printf("Received no pages");
            return;
        }
    }

    // Make sure all pfn metadata has the right frame number, is set as free, etc.
    setup_pfn_metadata(physical_page_count, physical_page_numbers);

    //
    // Reserve a user address space region using the Windows kernel
    // AWE (address windowing extensions) APIs.
    //
    // This will let us connect physical pages of our choosing to
    // any given virtual address within our allocated region.
    //
    // We deliberately make this much larger than physical memory
    // to illustrate how we can manage the illusion.
    //

    ULONG64 disc_page_count = NUM_DISC_PAGES;
    PVOID disc = create_page_file(&disc_page_count);
    virtual_address_size = (physical_page_count + disc_page_count - 1) * PAGE_SIZE;

    //
    // Round down to a PAGE_SIZE boundary.
    //

    virtual_address_size &= ~PAGE_SIZE;

    virtual_address_size_in_unsigned_chunks =
                        virtual_address_size / sizeof (ULONG_PTR);

    // The problem was that using VIRTUAL_ADDRESS_SIZE did not account for this variable that increased the size
    ULONG64 actual_num_ptes  = virtual_address_size / PAGE_SIZE;

    // Pointer to our page table
    page_table = zero_malloc(actual_num_ptes * sizeof(PTE));

    PULONG_PTR p;

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE

    MEM_EXTENDED_PARAMETER parameter = { 0 };

    //
    // Allocate a MEM_PHYSICAL region that is "connected" to the AWE section
    // created above.
    //

    parameter.Type = MemExtendedParameterUserPhysicalHandle;
    parameter.Handle = physical_page_handle;

    p = VirtualAlloc2 (NULL,
                       NULL,
                       virtual_address_size,
                       MEM_RESERVE | MEM_PHYSICAL,
                       PAGE_READWRITE,
                       &parameter,
                       1);

#else

    va_start = VirtualAlloc (NULL,
                      virtual_address_size,
                      MEM_RESERVE | MEM_PHYSICAL, // This lets us actually reserve and get access to PHYSICAL pages
                      PAGE_READWRITE);

#endif

    if (va_start == NULL) {

        printf ("full_virtual_memory_test : could not reserve memory %x\n",
                GetLastError ());

        return;
    }

    //
    // Now perform random accesses.
    //

    for (i = 0; i < (MB(1) / 4); i += 1) {
        random_number = rand () * rand () * rand ();
        random_number %= virtual_address_size_in_unsigned_chunks;

        random_number &= ~0x7;
        arbitrary_va = va_start + random_number;

        page_faulted = FALSE;

        __try {
            // Keep ONLY the execution touch line inside the monitored try block
            *arbitrary_va = (ULONG_PTR) arbitrary_va;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            page_faulted = TRUE;
        }

        if (page_faulted) {
            // Get the PTE matching this faulted address
            PPTE pte = get_pte_from_va(arbitrary_va);

            // Either it's on disk (disc = 1) of it's a brand new page (all zeros)
            if ((pte->hardware.valid == 0 && pte->transition.transition == 0 && pte->disc.disc == 1) ||
                (pte->hardware.valid == 0 && pte->transition.transition == 0 && pte->disc.disc == 0))
            {

                // If we are on disc, set old_disc slot to disc_index, else set to -1
                BOOL is_on_disk = pte->disc.disc;
                int old_disc_slot = is_on_disk ? pte->disc.disc_index : -1;

                pfn_metadata* target_pfn = NULL;

                // 1. Check for free pages we can use
                target_pfn = get_free_page();

                // 2. If no free pages, try to rescue/steal a frame sitting on the Standby List
                if (target_pfn == NULL && !IsListEmpty(&pfn_standby_list)) {
                    // Remove from standby list and get pfn
                    PLIST_ENTRY standby_entry = RemoveHeadList(&pfn_standby_list);
                    target_pfn = GetPfnFromListEntry(standby_entry);

                    // Sever the link to its old owner completely since we are recycling this frame now
                    PPTE old_owner_pte = target_pfn->pte;
                    if (old_owner_pte != NULL) {
                        set_to_disc_state(old_owner_pte, target_pfn);
                    }
                }

                // 3. If standby list is full, use batch trimming by age to get new pages from Active list
                else if (target_pfn == NULL) {
                    // Get batch size
                    // LK make sure this doesn't break when we get huge
                    int batch_size = MAX_TRIM_PAGES;
                    if (batch_size == 0) batch_size = 1;

                    // Keep track of the VAs for this batch
                    PULONG_PTR unmap_batch[MAX_TRIM_PAGES] = { NULL };
                    int batch_count = 0;

                    // Get our candidates
                    get_unmap_candidates(&batch_count, batch_size, physical_page_count,
                        unmap_batch, disc_page_count, disc);

                    // Unmap our candidates to free their pages
                    if (batch_count > 0) {
                        if (MapUserPhysicalPagesScatter((PVOID *)unmap_batch, batch_count, NULL) == FALSE) {
                            printf("Failed to unmap oldest batch via scatter. Error: %lu\n", GetLastError());
                            return;
                        }

                        // Now that data is safely on disk and unmapped, move pages from Modified (2) to Standby (3)
                        while (!IsListEmpty(&pfn_modified_list)) {
                            // Get modified pfn from the list
                            PLIST_ENTRY mod_entry = RemoveHeadList(&pfn_modified_list);
                            pfn_metadata* transition_pfn = GetPfnFromListEntry(mod_entry);

                            // Move to standby
                            transition_pfn->isOccupied = 3;
                            InsertTailList(&pfn_standby_list, &transition_pfn->list);
                        }
                    }

                    // Now that we have pages, we need to deal with the original fault
                    if (!IsListEmpty(&pfn_standby_list)) {
                        // Get page off of standby and transition it to active
                        PLIST_ENTRY standby_entry = RemoveHeadList(&pfn_standby_list);
                        target_pfn = GetPfnFromListEntry(standby_entry);

                        if (target_pfn->pte != NULL) {
                            target_pfn->pte->disc.transition = 0;
                            target_pfn->pte->disc.disc = 1;
                        }
                    }
                }

                if (target_pfn == NULL) {
                    printf("CRITICAL: No physical frame could be recovered!\n");
                    DebugBreak();
                    return;
                }

                // Now we need to actually map our new page
                ULONG64 hardware_frame = target_pfn->frame_number;
                PULONG_PTR aligned_new_va = get_va_from_pte(pte);

                // Map the physical frame into our hardware address window
                if (MapUserPhysicalPages((PVOID)aligned_new_va, 1, &hardware_frame) == FALSE) {
                    printf("Failed to map target window. Error: %lu\n", GetLastError());
                    return;
                }

                // 4. If we had data on disc, we can put it on our new page
                if (is_on_disk) {
                    void* disc_address = (char*)disc + (old_disc_slot * PAGE_SIZE);
                    memcpy((void*)aligned_new_va, disc_address, PAGE_SIZE);
                    disc_metadata[old_disc_slot] = FALSE; // Reclaim disk slot
                }
                // If we didn't have data then this is the first time we have accessed this page
                // We must clear it out to prevent security issues
                else {
                    memset((void*)aligned_new_va, 0, PAGE_SIZE);
                }

                // The page is now active so have both the pte and the pfn reflect this
                set_pte_valid(pte, hardware_frame);
                pte->hardware.age = 1; // Reset age on access

                target_pfn->pte = pte;
                target_pfn->isOccupied = 1; // Frame is officially Active (1)
                InsertTailList(&pfn_active_list, &target_pfn->list);
            }
        }
        // If we do not fault
        else {
            // Get our pte and frame number
            PPTE pte = get_pte_from_va(arbitrary_va);
            ULONG64 hardware_frame = pte->hardware.frame_number;

            //  Find our pfn metadata
            pfn_metadata* active_pfn = find_pfn_from_frame_number(hardware_frame, physical_page_count);

            if (active_pfn != NULL) {
                // Keep the age reset since the page is actively being touched by the user app
                if (active_pfn->pte != NULL) {
                    // Reset age since we got re-accessed
                    active_pfn->pte->hardware.age = 1;
                }
            }
        }
    }

    printf ("full_virtual_memory_test : finished accessing %u random virtual addresses\n", i);

    //
    // Now that we're done with our memory we can be a good
    // citizen and free it.
    //

    VirtualFree (va_start, 0, MEM_RELEASE);

    return;
}
VOID
main (
    int argc,
    char** argv
    )
{
    //
    // Test a simple malloc implementation - we call the operating
    // system to pay the up front cost to reserve and commit everything.
    //
    // Page faults will occur but the operating system will silently
    // handle them under the covers invisibly to us.
    //

    //malloc_test ();

    //
    // Test a slightly more complicated implementation - where we reserve
    // a big virtual address range up front, and only commit virtual
    // addresses as they get accessed.  This saves us from paying
    // commit costs for any portions we don't actually access.  But
    // the downside is what if we cannot commit it at the time of the
    // fault !
    //

    //commit_at_fault_time_test ();

    //
    // Test our very complicated usermode virtual implementation.
    //
    // We will control the virtual and physical address space management
    // ourselves with the only two exceptions being that we will :
    //
    // 1. Ask the operating system for the physical pages we'll use to
    //    form our pool.
    //
    // 2. Ask the operating system to connect one of our virtual addresses
    //    to one of our physical pages (from our pool).
    //
    // We would do both of those operations ourselves but the operating
    // system (for security reasons) does not allow us to.
    //
    // But we will do all the heavy lifting of maintaining translation
    // tables, PFN data structures, management of physical pages,
    // virtual memory operations like handling page faults, materializing
    // mappings, freeing them, trimming them, writing them out to backing
    // store, bringing them back from backing store, protecting them, etc.
    //
    // This is where we can be as creative as we like, the sky's the limit !
    //

    full_virtual_memory_test ();

    return;
}


