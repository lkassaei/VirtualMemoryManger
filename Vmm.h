//
// Created by lilyk on 7/14/2026.
//
#ifndef VMM_H
#define VMM_H

/* ============================================================================
 *  vmm.h  --  single shared header for the virtual memory manager
 *
 *  Every .c file in the project does exactly:   #include "vmm.h"
 *
 *  This header contains ONLY:
 *    - the platform include
 *    - compile-time configuration (#define)
 *    - type definitions (structs / unions / typedefs)
 *    - extern declarations of shared globals   (defined once, in globals.c)
 *    - static __forceinline helpers            (must live in the header)
 *    - prototypes for cross-translation-unit functions
 *
 *  It contains NO function bodies (other than the inline helpers) and NO
 *  global *definitions*. Those live in globals.c so there is exactly one
 *  definition of each symbol -- that is what keeps the linker happy.
 * ========================================================================== */

#include <windows.h>   /* CRITICAL_SECTION, LIST_ENTRY, HANDLE, ULONG64, etc. */
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>


/* ============================================================================
 *  1. CONFIGURATION  (was scattered as #defines at the top of the old file)
 * ========================================================================== */


#define SUPPORT_MULTIPLE_VA_TO_SAME_PAGE 1

#pragma comment(lib, "advapi32.lib")

#if SUPPORT_MULTIPLE_VA_TO_SAME_PAGE
#pragma comment(lib, "onecore.lib")
#endif

// Sizing constants
#define PAGE_SIZE 4096

#define MB(x) (((ULONG64)(x)) * 1024 * 1024)
#define KB(x) ((ULONG64)(x) * 1024)
#define GB(x) (MB(x) * 1024)

#define MAX_AGE_BITS 8

#define NUMBER_OF_PHYSICAL_PAGES (1 * (GB(1) / PAGE_SIZE))
//#define NUMBER_OF_PHYSICAL_PAGES 4
#define NUM_PTEs (VIRTUAL_ADDRESS_SIZE / PAGE_SIZE)
#define NUM_DISC_PAGES (1 * NUMBER_OF_PHYSICAL_PAGES)
#define MAX_DISC_PTE_BITS 40
#define MAX_DISC_SIZE ((ULONG64) 1 << MAX_DISC_PTE_BITS)
#define INVALID_DISC_SLOT ((1ULL << MAX_DISC_PTE_BITS) - 1)

#define DISC_BITMAP_WORDS(n)  (((n) + 63) / 64)

#define DISC_BATCH 256                       // was 64 — amortize the map/unmap syscalls
#define MAX_TRIM_PAGES 256                   // match DISC_BATCH so one harvest = one disc batch
#define LOWEST_PAGES (NUMBER_OF_PHYSICAL_PAGES / 64)     // ~4,096: wake trim earlier
#define HIGH_WATERMARK (LOWEST_PAGES * 8)                // ~32,768: deeper buffer
#define MAX_IN_FLIGHT (4 * DISC_BATCH)                   // 1,024 — auto-scales with DISC_BATCH

#define MAX_PREFETCH 1

#define PTE_REGION_SIZE 512

#define PFN_LOCK_STRIPES 256
#define PFN_LOCK(p) (&pfn_lock_stripes[(p)->frame_number & (PFN_LOCK_STRIPES - 1)])

/* Per-worker-thread staging layout:
 *   - RING_SLOTS_PER_THREAD : the deferred disc-read ring (batch-unmapped)
 *   - one dedicated dirty-zero slot after the ring (immediate unmap)
 * Total per thread = RING_SLOTS_PER_THREAD + 1. */
#define RING_SLOTS_PER_THREAD     8
#define STAGING_SLOTS_PER_THREAD  (RING_SLOTS_PER_THREAD + 1)     /* ring + dirty-zero slot */
#define STAGING_SLOTS             ((NUM_WORKER_THREADS + 1) * STAGING_SLOTS_PER_THREAD)

#define NUM_WORKER_THREADS 7 // Actually becomes 8 with main thread

/* bounds so the controller can't run away */
#define TRIM_TARGET_MIN   (NUMBER_OF_PHYSICAL_PAGES / 64)   /* ~4K  floor */
#define TRIM_TARGET_MAX   (NUMBER_OF_PHYSICAL_PAGES / 8)    /* ~32K ceiling */
#define TRIM_TARGET_STEP  1024

/* small steps */

/* hysteresis: stalls-per-interval thresholds */
#define STALL_HIGH   100    /* above this: trimmer is behind, trim harder  */
#define STALL_LOW    5      /* below this: trimmer is ahead, back off      */

#define ZERO_BATCH        64      /* frames zeroed per background pass       */
#define ZEROED_LIST_LOW   2048    /* refill trigger: zero more below this    */
#define ZEROED_LIST_HIGH  4096    /* stop refilling above this               */

#define STANDBY_SHARDS 8                      /* power of two -> cheap masking */

/* ---- workload tuning knobs ----
 * WORKING_SET_DIVISOR: cold jumps are confined to
 *   total_pages / WORKING_SET_DIVISOR. 1 = the old full-span behavior
 *   (max pressure, everything goes to disc). Larger = tighter working set,
 *   more pages survive on standby, more soft faults. Try 2 or 4 first.
 * REVISIT_CHANCE: 1-in-N run starts is a revisit. Lower N = more revisits.
 * RECENT_BIAS: revisits pick from the newest RECENT_BIAS ring entries this
 *   often (1-in-N), otherwise anywhere in the ring. Recency matters because
 *   a page trimmed 2ms ago is on standby; one trimmed 2s ago is on disc. */
#define WORKING_SET_DIVISOR 1 // Make bigger for max performance
#define REVISIT_CHANCE      2      /* was 4 */
#define RECENT_BIAS         1    /* all revisits target the newest spots */

#define PAGER_PERIOD_MS         20    /* wake at least this often: aging must
* keep running even when not starving  */
#define PER_REGION_HARVEST_MAX  64    /* cap pages taken from any one region so
* one region cannot dominate a pass     */
#define REGIONS_PER_PASS_STEP    8

/* ============================================================================
 *  Diagnostics + safety switches.
 *
 *    VMM_DIAGNOSTICS  -- prints + counters. Real runtime cost. OFF for perf.
 *    VMM_SAFETY       -- correctness guards / DebugBreaks.
 * ========================================================================== */

#ifndef VMM_DIAGNOSTICS
#define VMM_DIAGNOSTICS 1        /* 1 = counters + prints on; 0 = silent/uncounted */
#endif

#ifndef VMM_SAFETY
#define VMM_SAFETY 1             /* 1 = guards active; 0 = only once proven correct */
#endif


/* ---- diagnostics: compile to nothing when VMM_DIAGNOSTICS == 0 ---- */
#if VMM_DIAGNOSTICS
  #define DIAG_PRINT(...)   printf(__VA_ARGS__)
  #define DIAG_COUNT(x)     InterlockedIncrement64(&(x))
  #define DIAG_ADD(x, n)    InterlockedAdd64(&(x), (LONG64)(n))
#else
  #define DIAG_PRINT(...)   ((void)0)
  #define DIAG_COUNT(x)     ((void)0)
  #define DIAG_ADD(x, n)    ((void)0)
#endif

#if VMM_SAFETY
  #define VMM_ASSERT(cond, ...)                                   \
      if (!(cond)) { printf(__VA_ARGS__); DebugBreak(); }
#else
  #define VMM_ASSERT(cond, ...)  ((void)0)
#endif
/* ============================================================================
 *  2. TYPES
 * ========================================================================== */

// Struct for our PTEs
typedef struct {
    ULONG64 valid: 1;
    ULONG64 frame_number: 40;
    ULONG64 age: 1; // Since age is now stored in lists within pte regions, I only need to set to 1 on access
    ULONG64 reserved: 22;
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
        ULONG64 entire_contents; // How we can read the entire thing in one shot
    };
} PTE, *PPTE;


/* -------------------------------------------------------------------------
 *  LOCKED_LIST  --  a doubly-linked list with its own lock and a count.
 * ------------------------------------------------------------------------- */
typedef struct _LOCKED_LIST {
    LIST_ENTRY        head;
    ULONG64           count;
    CRITICAL_SECTION  lock;
} LOCKED_LIST, *PLOCKED_LIST;


/* -------------------------------------------------------------------------
 *  pfn_metadata  --  one per physical frame.
 *
 *  Field ownership (the invariant the whole design rests on):
 *    frame_number            immutable after setup
 *    pte                     region lock of the PTE it points at
 *    disc_index / isOccupied / isBeingWrittenToDisc / lock_bit
 *                            the page's OWN lock  (they share one qword)
 *    list                    the owning list's lock, or region lock for age lists
 *
 *  `lock` is the temporary per-page CRITICAL_SECTION. `lock_bit` is reserved
 *  as its eventual 1-bit-spinlock replacement -- keeping it in the shared
 *  qword now means the layout won't shift when you make the swap.
 * ------------------------------------------------------------------------- */
// Struct for our PFNs
typedef struct {
    LIST_ENTRY list;
    ULONG64 frame_number;
    PPTE pte;
    ULONG64 disc_index: MAX_DISC_PTE_BITS;
    ULONG64 isOccupied: 2; // 00 = free, 01 = active, 10 = modified, 11 = standby
    ULONG64 is_zero: 1;
    ULONG64 isBeingWrittenToDisc: 1; // 0 = no 1 = yes
    ULONG64 lock_bit: 1;              // reserved: future home of the 1-bit lock
    CRITICAL_SECTION lock;            // delete once lock_bit works
} pfn_metadata;


/* -------------------------------------------------------------------------
 *  pte_region  --  a contiguous span of PTE_REGION_SIZE PTEs sharing one lock,
 *  plus the eight age lists for pages currently active in that region.
 * ------------------------------------------------------------------------- */
typedef struct _PTE_REGION {
    CRITICAL_SECTION lock;
    LIST_ENTRY active_age_lists[MAX_AGE_BITS];
    ULONG64 active_page_count; // Tracks how many active pages are in this region
} PTE_REGION, *PPTE_REGION;

/* -------------------------------------------------------------------------
 *  THREAD_RNG_STATE  --  per-thread PRNG state used by the workload.
 * ------------------------------------------------------------------------- */
typedef struct _THREAD_RNG_STATE {
    ULONG64  state;
    ULONG64  counter;
} THREAD_RNG_STATE;


/* ============================================================================
 *  3. SHARED GLOBALS   (declared extern here, DEFINED once in globals.c)
 * ========================================================================== */

/* ---- page table / regions ---- */
extern PTE         *page_table;          /* base of the PTE array            */
extern PTE_REGION  *pte_regions;         /* array of num_pte_regions regions */
extern ULONG64      num_pte_regions;

/* ---- virtual address space ---- */
extern PVOID        va_start;
extern ULONG64      virtual_address_size;
extern ULONG64      virtual_address_size_in_unsigned_chunks;
extern PVOID         staging_va_start;          /* STAGING_SLOTS-page window */

/* ---- pfn table ---- */
extern pfn_metadata *frame_to_pfn_table; /* indexed by frame number (sparse) */
extern ULONG64      *frame_valid_bitmap; /* 1 bit per possible frame         */
extern ULONG64       max_frame_number;

extern ULONG_PTR  physical_page_count;
extern PULONG_PTR physical_page_numbers;

/* ---- physical page lists ---- */
extern LOCKED_LIST   pfn_free_list;
extern LOCKED_LIST   pfn_modified_list;
//extern LOCKED_LIST   pfn_standby_list;
extern LOCKED_LIST   pfn_standby_shards[STANDBY_SHARDS];


/* ---- disc ---- */
extern PVOID          disc;               /* the buffer (standardized name) */
extern PVOID          temp_disc_va_start; /* DISC_BATCH-page scratch window  */
extern CRITICAL_SECTION disc_lock;
extern ULONG64        disc_page_count;    /* actual size from create_page_file */
extern volatile LONG64* disc_bitmap;        /* 1 bit per slot: 1 = occupied */
extern ULONG64          disc_bitmap_words;
extern volatile LONG64  g_disc_free_count;  /* approximate; diagnostics + low-disc checks */

/* ---- events ---- */
extern HANDLE  StandbyPageAvailableEvent;
extern HANDLE  LowPagesEvent;
extern HANDLE  PagesReadyForDiscEvent;
extern HANDLE  StartAgingEvent;
extern HANDLE  FinishedAgingEvent;
extern HANDLE  ShutdownEvent;

extern LOCKED_LIST  pfn_zeroed_list;      /* frames confirmed zero, ready to hand out */
extern PVOID        zero_va_start;        /* the zeroing thread's OWN mapping window   */
extern HANDLE       NeedZeroingEvent;     /* wake the zeroing thread                   */

extern __declspec(thread) int t_thread_number;
extern __declspec(thread) int t_ring_pos;

extern volatile LONG64 g_hard_faults_disc;
extern volatile LONG64 g_hard_faults_zero;
extern volatile LONG64 g_soft_faults;
extern volatile LONG64 g_trim_unmaps;
extern volatile LONG64 g_time_lock_wait;
extern volatile LONG64 g_frames_zeroed;
extern volatile LONG64 g_dz_from_zerolist;
extern volatile LONG64 g_dz_pristine;
extern volatile LONG64 g_dz_dirty;


/* ---- controller state (for dynamic trimmer) ---- */
extern volatile ULONG64 g_trim_target;
extern volatile LONG64  g_fault_stalls;


/* ============================================================================
 *  4. INLINE HELPERS
 *  These MUST live in the header: they are __forceinline and are used from
 *  more than one .c file, so each translation unit needs to see the body.
 * ========================================================================== */

static __forceinline pfn_metadata *
GetPfnFromListEntry(PLIST_ENTRY entry) {
    return CONTAINING_RECORD(entry, pfn_metadata, list);
}

static __forceinline VOID lock_pfn(pfn_metadata *p)   { EnterCriticalSection(&p->lock); }
static __forceinline VOID unlock_pfn(pfn_metadata *p) { LeaveCriticalSection(&p->lock); }
static __forceinline int standby_shard_of(ULONG64 frame_number) {
    return (int)(frame_number & (STANDBY_SHARDS - 1));
}

static __forceinline ULONG64 standby_total_count(void) {
    ULONG64 total = 0;
    for (int i = 0; i < STANDBY_SHARDS; i++) {
        total += pfn_standby_shards[i].count;
    }
    return total;
}
/* When you migrate to the 1-bit lock, ONLY these two bodies change:
 *
 *   #define PFN_LOCK_BIT 43   // == bit position of lock_bit in the qword
 *   static __forceinline VOID lock_pfn(pfn_metadata *p) {
 *       volatile LONG64 *w = (volatile LONG64 *)&p->disc_index;
 *       while (InterlockedBitTestAndSet64(w, PFN_LOCK_BIT))
 *           while (*w & (1LL << PFN_LOCK_BIT)) YieldProcessor();
 *   }
 *   static __forceinline VOID unlock_pfn(pfn_metadata *p) {
 *       InterlockedBitTestAndReset64((volatile LONG64 *)&p->disc_index, PFN_LOCK_BIT);
 *   }
 *
 * Rules that must hold before you flip it: no path takes the same page lock
 * twice (a bit spinlock is NOT recursive), and no path holds a page lock
 * across a syscall (e.g. MapUserPhysicalPages). */

#define FRAME_IS_VALID(f)                                     \
    (((f) <= max_frame_number) &&                             \
     ((frame_valid_bitmap[(f) >> 6] >> ((f) & 63)) & 1ULL))


/* ============================================================================
 *  5. CROSS-MODULE PROTOTYPES
 *  Only functions called from a DIFFERENT .c than the one that defines them
 *  are listed. Purely-internal helpers (pull_from_standby_safely,
 *  handle_soft_fault, handle_hard_fault, harvest_one_bin, get_unmap_candidates)
 *  stay `static` inside their own .c and are intentionally NOT declared here.
 * ========================================================================== */

/* --- lists.c --- */
VOID        InitializeLockedList(PLOCKED_LIST list);
VOID        LockedInsertTail(PLOCKED_LIST list, PLIST_ENTRY entry);
VOID        LockedInsertTailBatch(PLOCKED_LIST list, PLIST_ENTRY* entries, int n);
PLIST_ENTRY LockedRemoveHead(PLOCKED_LIST list);
BOOL        LockedTryRemoveEntry(PLOCKED_LIST list, PLIST_ENTRY entry);

/* --- list primitives (windows.h isn't providing these in this config) --- */
VOID        InitializeListHead(PLIST_ENTRY ListHead);
BOOLEAN     IsListEmpty(PLIST_ENTRY ListHead);
VOID        InsertTailList(PLIST_ENTRY ListHead, PLIST_ENTRY Entry);
PLIST_ENTRY RemoveHeadList(PLIST_ENTRY ListHead);
BOOLEAN     RemoveEntryList(PLIST_ENTRY Entry);

/* --- pte.c --- */
PPTE              get_pte_from_va(PVOID va);
PVOID             get_va_from_pte(PPTE pte);
CRITICAL_SECTION* get_pte_lock_for_va(PVOID va);
CRITICAL_SECTION* get_pte_lock_from_pte_pointer(PPTE pte);
int  claim_staging_slot(VOID);
VOID release_staging_slot(int slot);
VOID              set_pte_valid(PPTE pte, ULONG64 frame_number);
VOID              set_to_disc_state(PPTE pte, pfn_metadata *p);

/* --- pfn.c --- */
VOID          setup_pfn_metadata(ULONG_PTR physical_page_count,
                                 PULONG_PTR physical_page_numbers);
pfn_metadata *find_pfn_from_frame_number(ULONG64 frame_number);
VOID          ensure_metadata_slot_is_committed(pfn_metadata *table_base,
                                                ULONG64 frame_number);

/* --- disc.c --- */
PVOID   create_page_file(PULONG64 number_of_pages);
ULONG64 find_free_disc_slot(VOID);
VOID free_disc_slot(ULONG64 slot);
VOID    write_to_disc(pfn_metadata** candidates, ULONG64 batch_count);


/* --- fault.c --- */
ULONG64 GetNextRandom(THREAD_RNG_STATE* rng);
VOID  handle_page_fault(PVOID arbitrary_va);
VOID staging_flush_current_thread(void);

/* --- threads.c --- */
DWORD WINAPI AgeThreadWorker(LPVOID param);
DWORD WINAPI TrimThreadWorker(LPVOID param);
DWORD WINAPI DiscThreadWorker(LPVOID param);
DWORD WINAPI ZeroThreadWorker(LPVOID param);

/* --- rng.c --- */
ULONG64 GetNextRandom(THREAD_RNG_STATE *rng);

/* --- workload.c --- */
VOID full_virtual_memory_test(VOID);
VOID full_virtual_memory_test_helper(int thread_number);
VOID full_virtual_memory_test_helper_not_random(int thread_number);
VOID handle_page_fault_run(PVOID base_va, ULONG64 run_ahead);

/* --- main.c / setup --- */
BOOL set_up_program(VOID);
PVOID zero_malloc(size_t num_bytes);

#endif /* VMM_H */
