/**
 * TODO: look into override_symbol to check if there's any docs
 */
#include "memory_helper.h"
#include "../../common.h"
#include "../call_protected.h" //_flush_tlb_kernel_range()
#include <asm/cacheflush.h> //PAGE_ALIGN
#include <asm/page_types.h> //PAGE_SIZE
#include <asm/pgtable_types.h> //_PAGE_RW

#define PAGE_ALIGN_BOTTOM(addr) (PAGE_ALIGN(addr) - PAGE_SIZE) //aligns the memory address to bottom of the page boundary
#define NUM_PAGES_BETWEEN(low, high) (((PAGE_ALIGN_BOTTOM(high) - PAGE_ALIGN_BOTTOM(low)) / PAGE_SIZE) + 1)

void set_mem_addr_rw(const unsigned long vaddr, unsigned long len)
{
    unsigned long addr = PAGE_ALIGN_BOTTOM(vaddr);
    pr_loc_dbg("Disabling memory protection for page(s) at %p+%lu/%u (<<%p)", (void *) vaddr, len,
               (unsigned int) NUM_PAGES_BETWEEN(vaddr, vaddr + len), (void *) addr);

    //theoretically this should use set_pte_atomic() but we're touching pages that will not be modified by anything else
    unsigned int level;
    for(; addr < vaddr + len; addr += PAGE_SIZE) {
        pte_t *pte = lookup_address(addr, &level);
        pte->pte |= _PAGE_RW;
    }

    //Scoped to just the touched page(s) instead of the previous _flush_tlb_all() (every page, every CPU). Still goes
    //through the kernel's own flush_tlb_kernel_range() - correct under VMware paravirt (unlike a hand-rolled local
    //invlpg, which can't be assumed safe to call directly outside its normal PV-aware callers) - but at a small
    //fraction of the IPI cost of a full-system flush, since only the pages in [addr, vaddr+len) need invalidating.
    _flush_tlb_kernel_range(PAGE_ALIGN_BOTTOM(vaddr), vaddr + len);
}

void set_mem_addr_ro(const unsigned long vaddr, unsigned long len)
{
    unsigned long addr = PAGE_ALIGN_BOTTOM(vaddr);
    pr_loc_dbg("Enabling memory protection for page(s) at %p+%lu/%u (<<%p)", (void *) vaddr, len,
               (unsigned int) NUM_PAGES_BETWEEN(vaddr, vaddr + len), (void *) addr);

    //theoretically this should use set_pte_atomic() but we're touching pages that will not be modified by anything else
    unsigned int level;
    for(; addr < vaddr + len; addr += PAGE_SIZE) {
        pte_t *pte = lookup_address(addr, &level);
        pte->pte &= ~_PAGE_RW;
    }

    //See set_mem_addr_rw() above for why this is scoped instead of a full _flush_tlb_all().
    _flush_tlb_kernel_range(PAGE_ALIGN_BOTTOM(vaddr), vaddr + len);
}
