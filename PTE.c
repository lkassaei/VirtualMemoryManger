//
// Created by lilyk on 7/14/2026.
//
/* ============================================================================
 *  pte.c  --  PTE address arithmetic, region-lock derivation, state stamping.
 *
 *  The two arithmetic helpers below are complete. set_pte_valid /
 *  set_to_disc_state touch the PTE bitfields, so their bodies depend on YOUR
 *  exact PTE union -- paste those two bodies from your monolith. The wrappers
 *  and the surrounding contract are here so the call sites don't change.
 * ========================================================================== */

#include "Vmm.h"

/* va -> pte:  index into the PTE array by page offset from va_start. */
PPTE
get_pte_from_va(PVOID va) {
    ULONG64 offset = (ULONG64)((ULONG_PTR)va - (ULONG_PTR)va_start);
    ULONG64 index  = offset / PAGE_SIZE;
    return &page_table[index];
}

/* pte -> va:  inverse of the above. */
PVOID
get_va_from_pte(PPTE pte) {
    ULONG64 index = (ULONG64)(pte - page_table);
    return (PVOID)((ULONG_PTR)va_start + index * PAGE_SIZE);
}

/* pte -> owning region lock.  Regions are contiguous spans of PTE_REGION_SIZE
 * PTEs, so the region index is just the PTE index divided by the span. This is
 * THE function the whole "pfn->pte is written only under its region lock"
 * invariant depends on -- do not change the mapping without auditing every
 * writer of pfn->pte. */

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

/* Stamp a PTE to the VALID/hardware format for a freshly mapped frame.
 *   *(PULONG64)pte = 0;  then set valid + frame_number, clear age.
 * PASTE your exact field writes -- widths must match your union. */

VOID
set_pte_valid(PPTE pte, ULONG64 pfn) {
    pte->hardware.valid = TRUE;
    pte->hardware.frame_number = pfn;
}

VOID
set_pte_invalid(PPTE pte) {
    pte->hardware.valid = FALSE;
}

/* Stamp the OLD owner's PTE to DISC format when a standby frame is repurposed.
 * Reads p->disc_index -- caller must not have cleared it yet.
 * PASTE your exact field writes. */

VOID
set_to_disc_state(PPTE old_owner_pte, pfn_metadata* target_pfn) {
    // 1. MUST wipe the entire 64-bit struct to clear the transition bits!
    *(PULONG64)old_owner_pte = 0;

    // 2. Now it is safe to set the disc state
    old_owner_pte->disc.disc = 1;
    old_owner_pte->disc.disc_index = target_pfn->disc_index;
}