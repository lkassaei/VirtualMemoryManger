//
// Created by lilyk on 7/14/2026.
//
/* ============================================================================
 *  globals.c  --  THE single definition of every shared global.
 *
 *  vmm.h declares all of these `extern`. They are defined here exactly once.
 *  This is the whole trick to splitting a single-file C program: every other
 *  .c can *reference* these symbols (via the extern in vmm.h) but only this
 *  file *defines* them, so the linker sees one definition and is happy.
 *
 *  Do not add `extern` here, and do not define any of these in any other .c.
 * ========================================================================== */

#include "Vmm.h"

/* ---- page table / regions ---- */
PTE         *page_table       = NULL;
PTE_REGION  *pte_regions      = NULL;
ULONG64      num_pte_regions  = 0;

/* ---- virtual address space ---- */
PVOID   va_start                                = NULL;
ULONG64 virtual_address_size                    = 0;
ULONG64 virtual_address_size_in_unsigned_chunks = 0;
PVOID         staging_va_start = NULL;

/* ---- pfn table ---- */
pfn_metadata *frame_to_pfn_table = NULL;
ULONG64      *frame_valid_bitmap = NULL;
ULONG64       max_frame_number   = 0;

ULONG_PTR  physical_page_count   = 0;
PULONG_PTR physical_page_numbers = NULL;

/* ---- physical page lists ---- */
LOCKED_LIST  pfn_free_list;
LOCKED_LIST  pfn_modified_list;
LOCKED_LIST  pfn_standby_list;
LOCKED_LIST pfn_standby_shards[STANDBY_SHARDS];

volatile LONG64 g_hard_faults_disc = 0;
volatile LONG64 g_hard_faults_zero = 0;
volatile LONG64 g_soft_faults      = 0;
volatile LONG64 g_trim_unmaps      = 0;
volatile LONG64 g_time_lock_wait = 0;
volatile LONG64 g_frames_zeroed = 0;
volatile LONG64 g_dz_from_zerolist = 0;
volatile LONG64 g_dz_pristine = 0;
volatile LONG64 g_dz_dirty = 0;

/* ---- controller state (for dynamic trimmer) ---- */
volatile LONG64 g_fault_stalls = 0;
ULONG64 g_trim_target = 0;

/* ---- disc ---- */
PVOID           disc               = NULL;
PVOID           temp_disc_va_start = NULL;
PDISC_METADATA  disc_metadata      = NULL;
LOCKED_LIST     disc_free_list;
CRITICAL_SECTION disc_lock;
ULONG64         disc_page_count    = 0;

/* ---- events ---- */
HANDLE  StandbyPageAvailableEvent = NULL;
HANDLE  LowPagesEvent             = NULL;
HANDLE  PagesReadyForDiscEvent    = NULL;
HANDLE  StartAgingEvent           = NULL;
HANDLE  FinishedAgingEvent        = NULL;
HANDLE  ShutdownEvent             = NULL;

/* ---- zero thread worker ---- */
LOCKED_LIST  pfn_zeroed_list;
PVOID        zero_va_start   = NULL;
HANDLE       NeedZeroingEvent = NULL;
