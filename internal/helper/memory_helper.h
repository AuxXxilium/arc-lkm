#ifndef REDPILL_MEMORY_HELPER_H
#define REDPILL_MEMORY_HELPER_H

/*
 * DSM 5.10.55+ kernels enforce CR0.WP independently of PTE flags. Setting PTE R/W alone is insufficient —
 * the CPU still page-faults supervisor writes to formerly-rodata pages. Call mem_disable_wp() before the
 * write and mem_restore_wp() after, bracketing both the set_mem_addr_rw() call and the actual store.
 * Must be used with local IRQs saved/restored to prevent preemption across the WP=0 window.
 */
unsigned long mem_disable_wp(void);
void mem_restore_wp(unsigned long cr0);

#define WITH_MEM_UNLOCKED(vaddr, size, code)           \
    do {                                               \
        set_mem_addr_rw((unsigned long)(vaddr), size); \
        ({code});                                      \
        set_mem_addr_ro((unsigned long)(vaddr), size); \
    } while(0)

/**
 * Disables write-protection for the memory where symbol resides
 *
 * There are a million different methods of circumventing the memory protection in Linux. The reason being the kernel
 * people make it harder and harder to modify syscall table (& others in the process), which in general is a great idea.
 * There are two core methods people use: 1) disabling CR0 WP bit, and 2) setting memory page(s) as R/W.
 * The 1) is a flag, present on x86 CPUs, which when cleared configures the MMU to *ignore* write protection set on
 * memory regions. However, this flag is per-core (=synchronization problems) and it works as all-or-none. We don't
 * want to leave such a big thing disabled (especially for long time).
 * The second mechanism disabled memory protection on per-page basis. Normally the kernel contains set_memory_rw() which
 * does what it says - sets the address (which should be lower-page aligned) to R/W. However, this function is evil for
 * some time (~2.6?). In its course it calls static_protections() which REMOVES the R/W flag from the request
 * (effectively making the call a noop) while still returning 0. Guess how long we debugged that... additionally, that
 * function is removed in newer kernels.
 * The easiest way is to just lookup the page table entry for a given address, modify the R/W attribute directly and
 * dump CPU caches. This will work as there's no middle-man to mess with our request.
 */
void set_mem_addr_rw(const unsigned long vaddr, unsigned long len);

/**
 * Reverses set_mem_rw()
 *
 * See set_mem_rw() for details
 */
void set_mem_addr_ro(const unsigned long vaddr, unsigned long len);

#endif //REDPILL_MEMORY_HELPER_H
