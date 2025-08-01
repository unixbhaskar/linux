/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PGTABLE_H
#define _LINUX_PGTABLE_H

#include <linux/pfn.h>
#include <asm/pgtable.h>

#define PMD_ORDER	(PMD_SHIFT - PAGE_SHIFT)
#define PUD_ORDER	(PUD_SHIFT - PAGE_SHIFT)

#ifndef __ASSEMBLY__
#ifdef CONFIG_MMU

#include <linux/mm_types.h>
#include <linux/bug.h>
#include <linux/errno.h>
#include <asm-generic/pgtable_uffd.h>
#include <linux/page_table_check.h>

#if 5 - defined(__PAGETABLE_P4D_FOLDED) - defined(__PAGETABLE_PUD_FOLDED) - \
	defined(__PAGETABLE_PMD_FOLDED) != CONFIG_PGTABLE_LEVELS
#error CONFIG_PGTABLE_LEVELS is not consistent with __PAGETABLE_{P4D,PUD,PMD}_FOLDED
#endif

/*
 * On almost all architectures and configurations, 0 can be used as the
 * upper ceiling to free_pgtables(): on many architectures it has the same
 * effect as using TASK_SIZE.  However, there is one configuration which
 * must impose a more careful limit, to avoid freeing kernel pgtables.
 */
#ifndef USER_PGTABLES_CEILING
#define USER_PGTABLES_CEILING	0UL
#endif

/*
 * This defines the first usable user address. Platforms
 * can override its value with custom FIRST_USER_ADDRESS
 * defined in their respective <asm/pgtable.h>.
 */
#ifndef FIRST_USER_ADDRESS
#define FIRST_USER_ADDRESS	0UL
#endif

/*
 * This defines the generic helper for accessing PMD page
 * table page. Although platforms can still override this
 * via their respective <asm/pgtable.h>.
 */
#ifndef pmd_pgtable
#define pmd_pgtable(pmd) pmd_page(pmd)
#endif

#define pmd_folio(pmd) page_folio(pmd_page(pmd))

/*
 * A page table page can be thought of an array like this: pXd_t[PTRS_PER_PxD]
 *
 * The pXx_index() functions return the index of the entry in the page
 * table page which would control the given virtual address
 *
 * As these functions may be used by the same code for different levels of
 * the page table folding, they are always available, regardless of
 * CONFIG_PGTABLE_LEVELS value. For the folded levels they simply return 0
 * because in such cases PTRS_PER_PxD equals 1.
 */

static inline unsigned long pte_index(unsigned long address)
{
	return (address >> PAGE_SHIFT) & (PTRS_PER_PTE - 1);
}

#ifndef pmd_index
static inline unsigned long pmd_index(unsigned long address)
{
	return (address >> PMD_SHIFT) & (PTRS_PER_PMD - 1);
}
#define pmd_index pmd_index
#endif

#ifndef pud_index
static inline unsigned long pud_index(unsigned long address)
{
	return (address >> PUD_SHIFT) & (PTRS_PER_PUD - 1);
}
#define pud_index pud_index
#endif

#ifndef pgd_index
/* Must be a compile-time constant, so implement it as a macro */
#define pgd_index(a)  (((a) >> PGDIR_SHIFT) & (PTRS_PER_PGD - 1))
#endif

#ifndef kernel_pte_init
static inline void kernel_pte_init(void *addr)
{
}
#define kernel_pte_init kernel_pte_init
#endif

#ifndef pmd_init
static inline void pmd_init(void *addr)
{
}
#define pmd_init pmd_init
#endif

#ifndef pud_init
static inline void pud_init(void *addr)
{
}
#define pud_init pud_init
#endif

#ifndef pte_offset_kernel
static inline pte_t *pte_offset_kernel(pmd_t *pmd, unsigned long address)
{
	return (pte_t *)pmd_page_vaddr(*pmd) + pte_index(address);
}
#define pte_offset_kernel pte_offset_kernel
#endif

#ifdef CONFIG_HIGHPTE
#define __pte_map(pmd, address) \
	((pte_t *)kmap_local_page(pmd_page(*(pmd))) + pte_index((address)))
#define pte_unmap(pte)	do {	\
	kunmap_local((pte));	\
	rcu_read_unlock();	\
} while (0)
#else
static inline pte_t *__pte_map(pmd_t *pmd, unsigned long address)
{
	return pte_offset_kernel(pmd, address);
}
static inline void pte_unmap(pte_t *pte)
{
	rcu_read_unlock();
}
#endif

void pte_free_defer(struct mm_struct *mm, pgtable_t pgtable);

/* Find an entry in the second-level page table.. */
#ifndef pmd_offset
static inline pmd_t *pmd_offset(pud_t *pud, unsigned long address)
{
	return pud_pgtable(*pud) + pmd_index(address);
}
#define pmd_offset pmd_offset
#endif

#ifndef pud_offset
static inline pud_t *pud_offset(p4d_t *p4d, unsigned long address)
{
	return p4d_pgtable(*p4d) + pud_index(address);
}
#define pud_offset pud_offset
#endif

static inline pgd_t *pgd_offset_pgd(pgd_t *pgd, unsigned long address)
{
	return (pgd + pgd_index(address));
};

/*
 * a shortcut to get a pgd_t in a given mm
 */
#ifndef pgd_offset
#define pgd_offset(mm, address)		pgd_offset_pgd((mm)->pgd, (address))
#endif

/*
 * a shortcut which implies the use of the kernel's pgd, instead
 * of a process's
 */
#define pgd_offset_k(address)		pgd_offset(&init_mm, (address))

/*
 * In many cases it is known that a virtual address is mapped at PMD or PTE
 * level, so instead of traversing all the page table levels, we can get a
 * pointer to the PMD entry in user or kernel page table or translate a virtual
 * address to the pointer in the PTE in the kernel page tables with simple
 * helpers.
 */
static inline pmd_t *pmd_off(struct mm_struct *mm, unsigned long va)
{
	return pmd_offset(pud_offset(p4d_offset(pgd_offset(mm, va), va), va), va);
}

static inline pmd_t *pmd_off_k(unsigned long va)
{
	return pmd_offset(pud_offset(p4d_offset(pgd_offset_k(va), va), va), va);
}

static inline pte_t *virt_to_kpte(unsigned long vaddr)
{
	pmd_t *pmd = pmd_off_k(vaddr);

	return pmd_none(*pmd) ? NULL : pte_offset_kernel(pmd, vaddr);
}

#ifndef pmd_young
static inline int pmd_young(pmd_t pmd)
{
	return 0;
}
#endif

#ifndef pmd_dirty
static inline int pmd_dirty(pmd_t pmd)
{
	return 0;
}
#endif

/*
 * A facility to provide lazy MMU batching.  This allows PTE updates and
 * page invalidations to be delayed until a call to leave lazy MMU mode
 * is issued.  Some architectures may benefit from doing this, and it is
 * beneficial for both shadow and direct mode hypervisors, which may batch
 * the PTE updates which happen during this window.  Note that using this
 * interface requires that read hazards be removed from the code.  A read
 * hazard could result in the direct mode hypervisor case, since the actual
 * write to the page tables may not yet have taken place, so reads though
 * a raw PTE pointer after it has been modified are not guaranteed to be
 * up to date.
 *
 * In the general case, no lock is guaranteed to be held between entry and exit
 * of the lazy mode. So the implementation must assume preemption may be enabled
 * and cpu migration is possible; it must take steps to be robust against this.
 * (In practice, for user PTE updates, the appropriate page table lock(s) are
 * held, but for kernel PTE updates, no lock is held). Nesting is not permitted
 * and the mode cannot be used in interrupt context.
 */
#ifndef __HAVE_ARCH_ENTER_LAZY_MMU_MODE
#define arch_enter_lazy_mmu_mode()	do {} while (0)
#define arch_leave_lazy_mmu_mode()	do {} while (0)
#define arch_flush_lazy_mmu_mode()	do {} while (0)
#endif

#ifndef pte_batch_hint
/**
 * pte_batch_hint - Number of pages that can be added to batch without scanning.
 * @ptep: Page table pointer for the entry.
 * @pte: Page table entry.
 *
 * Some architectures know that a set of contiguous ptes all map the same
 * contiguous memory with the same permissions. In this case, it can provide a
 * hint to aid pte batching without the core code needing to scan every pte.
 *
 * An architecture implementation may ignore the PTE accessed state. Further,
 * the dirty state must apply atomically to all the PTEs described by the hint.
 *
 * May be overridden by the architecture, else pte_batch_hint is always 1.
 */
static inline unsigned int pte_batch_hint(pte_t *ptep, pte_t pte)
{
	return 1;
}
#endif

#ifndef pte_advance_pfn
static inline pte_t pte_advance_pfn(pte_t pte, unsigned long nr)
{
	return __pte(pte_val(pte) + (nr << PFN_PTE_SHIFT));
}
#endif

#define pte_next_pfn(pte) pte_advance_pfn(pte, 1)

#ifndef set_ptes
/**
 * set_ptes - Map consecutive pages to a contiguous range of addresses.
 * @mm: Address space to map the pages into.
 * @addr: Address to map the first page at.
 * @ptep: Page table pointer for the first entry.
 * @pte: Page table entry for the first page.
 * @nr: Number of pages to map.
 *
 * When nr==1, initial state of pte may be present or not present, and new state
 * may be present or not present. When nr>1, initial state of all ptes must be
 * not present, and new state must be present.
 *
 * May be overridden by the architecture, or the architecture can define
 * set_pte() and PFN_PTE_SHIFT.
 *
 * Context: The caller holds the page table lock.  The pages all belong
 * to the same folio.  The PTEs are all in the same PMD.
 */
static inline void set_ptes(struct mm_struct *mm, unsigned long addr,
		pte_t *ptep, pte_t pte, unsigned int nr)
{
	page_table_check_ptes_set(mm, ptep, pte, nr);

	for (;;) {
		set_pte(ptep, pte);
		if (--nr == 0)
			break;
		ptep++;
		pte = pte_next_pfn(pte);
	}
}
#endif
#define set_pte_at(mm, addr, ptep, pte) set_ptes(mm, addr, ptep, pte, 1)

#ifndef __HAVE_ARCH_PTEP_SET_ACCESS_FLAGS
extern int ptep_set_access_flags(struct vm_area_struct *vma,
				 unsigned long address, pte_t *ptep,
				 pte_t entry, int dirty);
#endif

#ifndef __HAVE_ARCH_PMDP_SET_ACCESS_FLAGS
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
extern int pmdp_set_access_flags(struct vm_area_struct *vma,
				 unsigned long address, pmd_t *pmdp,
				 pmd_t entry, int dirty);
extern int pudp_set_access_flags(struct vm_area_struct *vma,
				 unsigned long address, pud_t *pudp,
				 pud_t entry, int dirty);
#else
static inline int pmdp_set_access_flags(struct vm_area_struct *vma,
					unsigned long address, pmd_t *pmdp,
					pmd_t entry, int dirty)
{
	BUILD_BUG();
	return 0;
}
static inline int pudp_set_access_flags(struct vm_area_struct *vma,
					unsigned long address, pud_t *pudp,
					pud_t entry, int dirty)
{
	BUILD_BUG();
	return 0;
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
#endif

#ifndef ptep_get
static inline pte_t ptep_get(pte_t *ptep)
{
	return READ_ONCE(*ptep);
}
#endif

#ifndef pmdp_get
static inline pmd_t pmdp_get(pmd_t *pmdp)
{
	return READ_ONCE(*pmdp);
}
#endif

#ifndef pudp_get
static inline pud_t pudp_get(pud_t *pudp)
{
	return READ_ONCE(*pudp);
}
#endif

#ifndef p4dp_get
static inline p4d_t p4dp_get(p4d_t *p4dp)
{
	return READ_ONCE(*p4dp);
}
#endif

#ifndef pgdp_get
static inline pgd_t pgdp_get(pgd_t *pgdp)
{
	return READ_ONCE(*pgdp);
}
#endif

#ifndef __HAVE_ARCH_PTEP_TEST_AND_CLEAR_YOUNG
static inline int ptep_test_and_clear_young(struct vm_area_struct *vma,
					    unsigned long address,
					    pte_t *ptep)
{
	pte_t pte = ptep_get(ptep);
	int r = 1;
	if (!pte_young(pte))
		r = 0;
	else
		set_pte_at(vma->vm_mm, address, ptep, pte_mkold(pte));
	return r;
}
#endif

#ifndef __HAVE_ARCH_PMDP_TEST_AND_CLEAR_YOUNG
#if defined(CONFIG_TRANSPARENT_HUGEPAGE) || defined(CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG)
static inline int pmdp_test_and_clear_young(struct vm_area_struct *vma,
					    unsigned long address,
					    pmd_t *pmdp)
{
	pmd_t pmd = *pmdp;
	int r = 1;
	if (!pmd_young(pmd))
		r = 0;
	else
		set_pmd_at(vma->vm_mm, address, pmdp, pmd_mkold(pmd));
	return r;
}
#else
static inline int pmdp_test_and_clear_young(struct vm_area_struct *vma,
					    unsigned long address,
					    pmd_t *pmdp)
{
	BUILD_BUG();
	return 0;
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE || CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG */
#endif

#ifndef __HAVE_ARCH_PTEP_CLEAR_YOUNG_FLUSH
int ptep_clear_flush_young(struct vm_area_struct *vma,
			   unsigned long address, pte_t *ptep);
#endif

#ifndef __HAVE_ARCH_PMDP_CLEAR_YOUNG_FLUSH
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
extern int pmdp_clear_flush_young(struct vm_area_struct *vma,
				  unsigned long address, pmd_t *pmdp);
#else
/*
 * Despite relevant to THP only, this API is called from generic rmap code
 * under PageTransHuge(), hence needs a dummy implementation for !THP
 */
static inline int pmdp_clear_flush_young(struct vm_area_struct *vma,
					 unsigned long address, pmd_t *pmdp)
{
	BUILD_BUG();
	return 0;
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
#endif

#ifndef arch_has_hw_nonleaf_pmd_young
/*
 * Return whether the accessed bit in non-leaf PMD entries is supported on the
 * local CPU.
 */
static inline bool arch_has_hw_nonleaf_pmd_young(void)
{
	return IS_ENABLED(CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG);
}
#endif

#ifndef arch_has_hw_pte_young
/*
 * Return whether the accessed bit is supported on the local CPU.
 *
 * This stub assumes accessing through an old PTE triggers a page fault.
 * Architectures that automatically set the access bit should overwrite it.
 */
static inline bool arch_has_hw_pte_young(void)
{
	return IS_ENABLED(CONFIG_ARCH_HAS_HW_PTE_YOUNG);
}
#endif

#ifndef exec_folio_order
/*
 * Returns preferred minimum folio order for executable file-backed memory. Must
 * be in range [0, PMD_ORDER). Default to order-0.
 */
static inline unsigned int exec_folio_order(void)
{
	return 0;
}
#endif

#ifndef arch_check_zapped_pte
static inline void arch_check_zapped_pte(struct vm_area_struct *vma,
					 pte_t pte)
{
}
#endif

#ifndef arch_check_zapped_pmd
static inline void arch_check_zapped_pmd(struct vm_area_struct *vma,
					 pmd_t pmd)
{
}
#endif

#ifndef arch_check_zapped_pud
static inline void arch_check_zapped_pud(struct vm_area_struct *vma, pud_t pud)
{
}
#endif

#ifndef __HAVE_ARCH_PTEP_GET_AND_CLEAR
static inline pte_t ptep_get_and_clear(struct mm_struct *mm,
				       unsigned long address,
				       pte_t *ptep)
{
	pte_t pte = ptep_get(ptep);
	pte_clear(mm, address, ptep);
	page_table_check_pte_clear(mm, pte);
	return pte;
}
#endif

#ifndef clear_young_dirty_ptes
/**
 * clear_young_dirty_ptes - Mark PTEs that map consecutive pages of the
 *		same folio as old/clean.
 * @mm: Address space the pages are mapped into.
 * @addr: Address the first page is mapped at.
 * @ptep: Page table pointer for the first entry.
 * @nr: Number of entries to mark old/clean.
 * @flags: Flags to modify the PTE batch semantics.
 *
 * May be overridden by the architecture; otherwise, implemented by
 * get_and_clear/modify/set for each pte in the range.
 *
 * Note that PTE bits in the PTE range besides the PFN can differ. For example,
 * some PTEs might be write-protected.
 *
 * Context: The caller holds the page table lock.  The PTEs map consecutive
 * pages that belong to the same folio.  The PTEs are all in the same PMD.
 */
static inline void clear_young_dirty_ptes(struct vm_area_struct *vma,
					  unsigned long addr, pte_t *ptep,
					  unsigned int nr, cydp_t flags)
{
	pte_t pte;

	for (;;) {
		if (flags == CYDP_CLEAR_YOUNG)
			ptep_test_and_clear_young(vma, addr, ptep);
		else {
			pte = ptep_get_and_clear(vma->vm_mm, addr, ptep);
			if (flags & CYDP_CLEAR_YOUNG)
				pte = pte_mkold(pte);
			if (flags & CYDP_CLEAR_DIRTY)
				pte = pte_mkclean(pte);
			set_pte_at(vma->vm_mm, addr, ptep, pte);
		}
		if (--nr == 0)
			break;
		ptep++;
		addr += PAGE_SIZE;
	}
}
#endif

static inline void ptep_clear(struct mm_struct *mm, unsigned long addr,
			      pte_t *ptep)
{
	pte_t pte = ptep_get(ptep);

	pte_clear(mm, addr, ptep);
	/*
	 * No need for ptep_get_and_clear(): page table check doesn't care about
	 * any bits that could have been set by HW concurrently.
	 */
	page_table_check_pte_clear(mm, pte);
}

#ifdef CONFIG_GUP_GET_PXX_LOW_HIGH
/*
 * For walking the pagetables without holding any locks.  Some architectures
 * (eg x86-32 PAE) cannot load the entries atomically without using expensive
 * instructions.  We are guaranteed that a PTE will only either go from not
 * present to present, or present to not present -- it will not switch to a
 * completely different present page without a TLB flush inbetween; which we
 * are blocking by holding interrupts off.
 *
 * Setting ptes from not present to present goes:
 *
 *   ptep->pte_high = h;
 *   smp_wmb();
 *   ptep->pte_low = l;
 *
 * And present to not present goes:
 *
 *   ptep->pte_low = 0;
 *   smp_wmb();
 *   ptep->pte_high = 0;
 *
 * We must ensure here that the load of pte_low sees 'l' IFF pte_high sees 'h'.
 * We load pte_high *after* loading pte_low, which ensures we don't see an older
 * value of pte_high.  *Then* we recheck pte_low, which ensures that we haven't
 * picked up a changed pte high. We might have gotten rubbish values from
 * pte_low and pte_high, but we are guaranteed that pte_low will not have the
 * present bit set *unless* it is 'l'. Because get_user_pages_fast() only
 * operates on present ptes we're safe.
 */
static inline pte_t ptep_get_lockless(pte_t *ptep)
{
	pte_t pte;

	do {
		pte.pte_low = ptep->pte_low;
		smp_rmb();
		pte.pte_high = ptep->pte_high;
		smp_rmb();
	} while (unlikely(pte.pte_low != ptep->pte_low));

	return pte;
}
#define ptep_get_lockless ptep_get_lockless

#if CONFIG_PGTABLE_LEVELS > 2
static inline pmd_t pmdp_get_lockless(pmd_t *pmdp)
{
	pmd_t pmd;

	do {
		pmd.pmd_low = pmdp->pmd_low;
		smp_rmb();
		pmd.pmd_high = pmdp->pmd_high;
		smp_rmb();
	} while (unlikely(pmd.pmd_low != pmdp->pmd_low));

	return pmd;
}
#define pmdp_get_lockless pmdp_get_lockless
#define pmdp_get_lockless_sync() tlb_remove_table_sync_one()
#endif /* CONFIG_PGTABLE_LEVELS > 2 */
#endif /* CONFIG_GUP_GET_PXX_LOW_HIGH */

/*
 * We require that the PTE can be read atomically.
 */
#ifndef ptep_get_lockless
static inline pte_t ptep_get_lockless(pte_t *ptep)
{
	return ptep_get(ptep);
}
#endif

#ifndef pmdp_get_lockless
static inline pmd_t pmdp_get_lockless(pmd_t *pmdp)
{
	return pmdp_get(pmdp);
}
static inline void pmdp_get_lockless_sync(void)
{
}
#endif

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#ifndef __HAVE_ARCH_PMDP_HUGE_GET_AND_CLEAR
static inline pmd_t pmdp_huge_get_and_clear(struct mm_struct *mm,
					    unsigned long address,
					    pmd_t *pmdp)
{
	pmd_t pmd = *pmdp;

	pmd_clear(pmdp);
	page_table_check_pmd_clear(mm, pmd);

	return pmd;
}
#endif /* __HAVE_ARCH_PMDP_HUGE_GET_AND_CLEAR */
#ifndef __HAVE_ARCH_PUDP_HUGE_GET_AND_CLEAR
static inline pud_t pudp_huge_get_and_clear(struct mm_struct *mm,
					    unsigned long address,
					    pud_t *pudp)
{
	pud_t pud = *pudp;

	pud_clear(pudp);
	page_table_check_pud_clear(mm, pud);

	return pud;
}
#endif /* __HAVE_ARCH_PUDP_HUGE_GET_AND_CLEAR */
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#ifndef __HAVE_ARCH_PMDP_HUGE_GET_AND_CLEAR_FULL
static inline pmd_t pmdp_huge_get_and_clear_full(struct vm_area_struct *vma,
					    unsigned long address, pmd_t *pmdp,
					    int full)
{
	return pmdp_huge_get_and_clear(vma->vm_mm, address, pmdp);
}
#endif

#ifndef __HAVE_ARCH_PUDP_HUGE_GET_AND_CLEAR_FULL
static inline pud_t pudp_huge_get_and_clear_full(struct vm_area_struct *vma,
					    unsigned long address, pud_t *pudp,
					    int full)
{
	return pudp_huge_get_and_clear(vma->vm_mm, address, pudp);
}
#endif
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#ifndef __HAVE_ARCH_PTEP_GET_AND_CLEAR_FULL
static inline pte_t ptep_get_and_clear_full(struct mm_struct *mm,
					    unsigned long address, pte_t *ptep,
					    int full)
{
	return ptep_get_and_clear(mm, address, ptep);
}
#endif

#ifndef get_and_clear_full_ptes
/**
 * get_and_clear_full_ptes - Clear present PTEs that map consecutive pages of
 *			     the same folio, collecting dirty/accessed bits.
 * @mm: Address space the pages are mapped into.
 * @addr: Address the first page is mapped at.
 * @ptep: Page table pointer for the first entry.
 * @nr: Number of entries to clear.
 * @full: Whether we are clearing a full mm.
 *
 * May be overridden by the architecture; otherwise, implemented as a simple
 * loop over ptep_get_and_clear_full(), merging dirty/accessed bits into the
 * returned PTE.
 *
 * Note that PTE bits in the PTE range besides the PFN can differ. For example,
 * some PTEs might be write-protected.
 *
 * Context: The caller holds the page table lock.  The PTEs map consecutive
 * pages that belong to the same folio.  The PTEs are all in the same PMD.
 */
static inline pte_t get_and_clear_full_ptes(struct mm_struct *mm,
		unsigned long addr, pte_t *ptep, unsigned int nr, int full)
{
	pte_t pte, tmp_pte;

	pte = ptep_get_and_clear_full(mm, addr, ptep, full);
	while (--nr) {
		ptep++;
		addr += PAGE_SIZE;
		tmp_pte = ptep_get_and_clear_full(mm, addr, ptep, full);
		if (pte_dirty(tmp_pte))
			pte = pte_mkdirty(pte);
		if (pte_young(tmp_pte))
			pte = pte_mkyoung(pte);
	}
	return pte;
}
#endif

#ifndef clear_full_ptes
/**
 * clear_full_ptes - Clear present PTEs that map consecutive pages of the same
 *		     folio.
 * @mm: Address space the pages are mapped into.
 * @addr: Address the first page is mapped at.
 * @ptep: Page table pointer for the first entry.
 * @nr: Number of entries to clear.
 * @full: Whether we are clearing a full mm.
 *
 * May be overridden by the architecture; otherwise, implemented as a simple
 * loop over ptep_get_and_clear_full().
 *
 * Note that PTE bits in the PTE range besides the PFN can differ. For example,
 * some PTEs might be write-protected.
 *
 * Context: The caller holds the page table lock.  The PTEs map consecutive
 * pages that belong to the same folio.  The PTEs are all in the same PMD.
 */
static inline void clear_full_ptes(struct mm_struct *mm, unsigned long addr,
		pte_t *ptep, unsigned int nr, int full)
{
	for (;;) {
		ptep_get_and_clear_full(mm, addr, ptep, full);
		if (--nr == 0)
			break;
		ptep++;
		addr += PAGE_SIZE;
	}
}
#endif

/*
 * If two threads concurrently fault at the same page, the thread that
 * won the race updates the PTE and its local TLB/Cache. The other thread
 * gives up, simply does nothing, and continues; on architectures where
 * software can update TLB,  local TLB can be updated here to avoid next page
 * fault. This function updates TLB only, do nothing with cache or others.
 * It is the difference with function update_mmu_cache.
 */
#ifndef update_mmu_tlb_range
static inline void update_mmu_tlb_range(struct vm_area_struct *vma,
				unsigned long address, pte_t *ptep, unsigned int nr)
{
}
#endif

static inline void update_mmu_tlb(struct vm_area_struct *vma,
				unsigned long address, pte_t *ptep)
{
	update_mmu_tlb_range(vma, address, ptep, 1);
}

/*
 * Some architectures may be able to avoid expensive synchronization
 * primitives when modifications are made to PTE's which are already
 * not present, or in the process of an address space destruction.
 */
#ifndef __HAVE_ARCH_PTE_CLEAR_NOT_PRESENT_FULL
static inline void pte_clear_not_present_full(struct mm_struct *mm,
					      unsigned long address,
					      pte_t *ptep,
					      int full)
{
	pte_clear(mm, address, ptep);
}
#endif

#ifndef clear_not_present_full_ptes
/**
 * clear_not_present_full_ptes - Clear multiple not present PTEs which are
 *				 consecutive in the pgtable.
 * @mm: Address space the ptes represent.
 * @addr: Address of the first pte.
 * @ptep: Page table pointer for the first entry.
 * @nr: Number of entries to clear.
 * @full: Whether we are clearing a full mm.
 *
 * May be overridden by the architecture; otherwise, implemented as a simple
 * loop over pte_clear_not_present_full().
 *
 * Context: The caller holds the page table lock.  The PTEs are all not present.
 * The PTEs are all in the same PMD.
 */
static inline void clear_not_present_full_ptes(struct mm_struct *mm,
		unsigned long addr, pte_t *ptep, unsigned int nr, int full)
{
	for (;;) {
		pte_clear_not_present_full(mm, addr, ptep, full);
		if (--nr == 0)
			break;
		ptep++;
		addr += PAGE_SIZE;
	}
}
#endif

#ifndef __HAVE_ARCH_PTEP_CLEAR_FLUSH
extern pte_t ptep_clear_flush(struct vm_area_struct *vma,
			      unsigned long address,
			      pte_t *ptep);
#endif

#ifndef __HAVE_ARCH_PMDP_HUGE_CLEAR_FLUSH
extern pmd_t pmdp_huge_clear_flush(struct vm_area_struct *vma,
			      unsigned long address,
			      pmd_t *pmdp);
extern pud_t pudp_huge_clear_flush(struct vm_area_struct *vma,
			      unsigned long address,
			      pud_t *pudp);
#endif

#ifndef pte_mkwrite
static inline pte_t pte_mkwrite(pte_t pte, struct vm_area_struct *vma)
{
	return pte_mkwrite_novma(pte);
}
#endif

#if defined(CONFIG_ARCH_WANT_PMD_MKWRITE) && !defined(pmd_mkwrite)
static inline pmd_t pmd_mkwrite(pmd_t pmd, struct vm_area_struct *vma)
{
	return pmd_mkwrite_novma(pmd);
}
#endif

#ifndef __HAVE_ARCH_PTEP_SET_WRPROTECT
struct mm_struct;
static inline void ptep_set_wrprotect(struct mm_struct *mm, unsigned long address, pte_t *ptep)
{
	pte_t old_pte = ptep_get(ptep);
	set_pte_at(mm, address, ptep, pte_wrprotect(old_pte));
}
#endif

#ifndef wrprotect_ptes
/**
 * wrprotect_ptes - Write-protect PTEs that map consecutive pages of the same
 *		    folio.
 * @mm: Address space the pages are mapped into.
 * @addr: Address the first page is mapped at.
 * @ptep: Page table pointer for the first entry.
 * @nr: Number of entries to write-protect.
 *
 * May be overridden by the architecture; otherwise, implemented as a simple
 * loop over ptep_set_wrprotect().
 *
 * Note that PTE bits in the PTE range besides the PFN can differ. For example,
 * some PTEs might be write-protected.
 *
 * Context: The caller holds the page table lock.  The PTEs map consecutive
 * pages that belong to the same folio.  The PTEs are all in the same PMD.
 */
static inline void wrprotect_ptes(struct mm_struct *mm, unsigned long addr,
		pte_t *ptep, unsigned int nr)
{
	for (;;) {
		ptep_set_wrprotect(mm, addr, ptep);
		if (--nr == 0)
			break;
		ptep++;
		addr += PAGE_SIZE;
	}
}
#endif

/*
 * On some architectures hardware does not set page access bit when accessing
 * memory page, it is responsibility of software setting this bit. It brings
 * out extra page fault penalty to track page access bit. For optimization page
 * access bit can be set during all page fault flow on these arches.
 * To be differentiate with macro pte_mkyoung, this macro is used on platforms
 * where software maintains page access bit.
 */
#ifndef pte_sw_mkyoung
static inline pte_t pte_sw_mkyoung(pte_t pte)
{
	return pte;
}
#define pte_sw_mkyoung	pte_sw_mkyoung
#endif

#ifndef __HAVE_ARCH_PMDP_SET_WRPROTECT
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static inline void pmdp_set_wrprotect(struct mm_struct *mm,
				      unsigned long address, pmd_t *pmdp)
{
	pmd_t old_pmd = *pmdp;
	set_pmd_at(mm, address, pmdp, pmd_wrprotect(old_pmd));
}
#else
static inline void pmdp_set_wrprotect(struct mm_struct *mm,
				      unsigned long address, pmd_t *pmdp)
{
	BUILD_BUG();
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
#endif
#ifndef __HAVE_ARCH_PUDP_SET_WRPROTECT
#ifdef CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static inline void pudp_set_wrprotect(struct mm_struct *mm,
				      unsigned long address, pud_t *pudp)
{
	pud_t old_pud = *pudp;

	set_pud_at(mm, address, pudp, pud_wrprotect(old_pud));
}
#else
static inline void pudp_set_wrprotect(struct mm_struct *mm,
				      unsigned long address, pud_t *pudp)
{
	BUILD_BUG();
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
#endif /* CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD */
#endif

#ifndef pmdp_collapse_flush
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
extern pmd_t pmdp_collapse_flush(struct vm_area_struct *vma,
				 unsigned long address, pmd_t *pmdp);
#else
static inline pmd_t pmdp_collapse_flush(struct vm_area_struct *vma,
					unsigned long address,
					pmd_t *pmdp)
{
	BUILD_BUG();
	return *pmdp;
}
#define pmdp_collapse_flush pmdp_collapse_flush
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
#endif

#ifndef __HAVE_ARCH_PGTABLE_DEPOSIT
extern void pgtable_trans_huge_deposit(struct mm_struct *mm, pmd_t *pmdp,
				       pgtable_t pgtable);
#endif

#ifndef __HAVE_ARCH_PGTABLE_WITHDRAW
extern pgtable_t pgtable_trans_huge_withdraw(struct mm_struct *mm, pmd_t *pmdp);
#endif

#ifndef arch_needs_pgtable_deposit
#define arch_needs_pgtable_deposit() (false)
#endif

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/*
 * This is an implementation of pmdp_establish() that is only suitable for an
 * architecture that doesn't have hardware dirty/accessed bits. In this case we
 * can't race with CPU which sets these bits and non-atomic approach is fine.
 */
static inline pmd_t generic_pmdp_establish(struct vm_area_struct *vma,
		unsigned long address, pmd_t *pmdp, pmd_t pmd)
{
	pmd_t old_pmd = *pmdp;
	set_pmd_at(vma->vm_mm, address, pmdp, pmd);
	return old_pmd;
}
#endif

#ifndef __HAVE_ARCH_PMDP_INVALIDATE
extern pmd_t pmdp_invalidate(struct vm_area_struct *vma, unsigned long address,
			    pmd_t *pmdp);
#endif

#ifndef __HAVE_ARCH_PMDP_INVALIDATE_AD

/*
 * pmdp_invalidate_ad() invalidates the PMD while changing a transparent
 * hugepage mapping in the page tables. This function is similar to
 * pmdp_invalidate(), but should only be used if the access and dirty bits would
 * not be cleared by the software in the new PMD value. The function ensures
 * that hardware changes of the access and dirty bits updates would not be lost.
 *
 * Doing so can allow in certain architectures to avoid a TLB flush in most
 * cases. Yet, another TLB flush might be necessary later if the PMD update
 * itself requires such flush (e.g., if protection was set to be stricter). Yet,
 * even when a TLB flush is needed because of the update, the caller may be able
 * to batch these TLB flushing operations, so fewer TLB flush operations are
 * needed.
 */
extern pmd_t pmdp_invalidate_ad(struct vm_area_struct *vma,
				unsigned long address, pmd_t *pmdp);
#endif

#ifndef __HAVE_ARCH_PTE_SAME
static inline int pte_same(pte_t pte_a, pte_t pte_b)
{
	return pte_val(pte_a) == pte_val(pte_b);
}
#endif

#ifndef __HAVE_ARCH_PTE_UNUSED
/*
 * Some architectures provide facilities to virtualization guests
 * so that they can flag allocated pages as unused. This allows the
 * host to transparently reclaim unused pages. This function returns
 * whether the pte's page is unused.
 */
static inline int pte_unused(pte_t pte)
{
	return 0;
}
#endif

#ifndef pte_access_permitted
#define pte_access_permitted(pte, write) \
	(pte_present(pte) && (!(write) || pte_write(pte)))
#endif

#ifndef pmd_access_permitted
#define pmd_access_permitted(pmd, write) \
	(pmd_present(pmd) && (!(write) || pmd_write(pmd)))
#endif

#ifndef pud_access_permitted
#define pud_access_permitted(pud, write) \
	(pud_present(pud) && (!(write) || pud_write(pud)))
#endif

#ifndef p4d_access_permitted
#define p4d_access_permitted(p4d, write) \
	(p4d_present(p4d) && (!(write) || p4d_write(p4d)))
#endif

#ifndef pgd_access_permitted
#define pgd_access_permitted(pgd, write) \
	(pgd_present(pgd) && (!(write) || pgd_write(pgd)))
#endif

#ifndef __HAVE_ARCH_PMD_SAME
static inline int pmd_same(pmd_t pmd_a, pmd_t pmd_b)
{
	return pmd_val(pmd_a) == pmd_val(pmd_b);
}
#endif

#ifndef pud_same
static inline int pud_same(pud_t pud_a, pud_t pud_b)
{
	return pud_val(pud_a) == pud_val(pud_b);
}
#define pud_same pud_same
#endif

#ifndef __HAVE_ARCH_P4D_SAME
static inline int p4d_same(p4d_t p4d_a, p4d_t p4d_b)
{
	return p4d_val(p4d_a) == p4d_val(p4d_b);
}
#endif

#ifndef __HAVE_ARCH_PGD_SAME
static inline int pgd_same(pgd_t pgd_a, pgd_t pgd_b)
{
	return pgd_val(pgd_a) == pgd_val(pgd_b);
}
#endif

#ifndef __HAVE_ARCH_DO_SWAP_PAGE
static inline void arch_do_swap_page_nr(struct mm_struct *mm,
				     struct vm_area_struct *vma,
				     unsigned long addr,
				     pte_t pte, pte_t oldpte,
				     int nr)
{

}
#else
/*
 * Some architectures support metadata associated with a page. When a
 * page is being swapped out, this metadata must be saved so it can be
 * restored when the page is swapped back in. SPARC M7 and newer
 * processors support an ADI (Application Data Integrity) tag for the
 * page as metadata for the page. arch_do_swap_page() can restore this
 * metadata when a page is swapped back in.
 */
static inline void arch_do_swap_page_nr(struct mm_struct *mm,
					struct vm_area_struct *vma,
					unsigned long addr,
					pte_t pte, pte_t oldpte,
					int nr)
{
	for (int i = 0; i < nr; i++) {
		arch_do_swap_page(vma->vm_mm, vma, addr + i * PAGE_SIZE,
				pte_advance_pfn(pte, i),
				pte_advance_pfn(oldpte, i));
	}
}
#endif

#ifndef __HAVE_ARCH_UNMAP_ONE
/*
 * Some architectures support metadata associated with a page. When a
 * page is being swapped out, this metadata must be saved so it can be
 * restored when the page is swapped back in. SPARC M7 and newer
 * processors support an ADI (Application Data Integrity) tag for the
 * page as metadata for the page. arch_unmap_one() can save this
 * metadata on a swap-out of a page.
 */
static inline int arch_unmap_one(struct mm_struct *mm,
				  struct vm_area_struct *vma,
				  unsigned long addr,
				  pte_t orig_pte)
{
	return 0;
}
#endif

/*
 * Allow architectures to preserve additional metadata associated with
 * swapped-out pages. The corresponding __HAVE_ARCH_SWAP_* macros and function
 * prototypes must be defined in the arch-specific asm/pgtable.h file.
 */
#ifndef __HAVE_ARCH_PREPARE_TO_SWAP
static inline int arch_prepare_to_swap(struct folio *folio)
{
	return 0;
}
#endif

#ifndef __HAVE_ARCH_SWAP_INVALIDATE
static inline void arch_swap_invalidate_page(int type, pgoff_t offset)
{
}

static inline void arch_swap_invalidate_area(int type)
{
}
#endif

#ifndef __HAVE_ARCH_SWAP_RESTORE
static inline void arch_swap_restore(swp_entry_t entry, struct folio *folio)
{
}
#endif

#ifndef __HAVE_ARCH_MOVE_PTE
#define move_pte(pte, old_addr, new_addr)	(pte)
#endif

#ifndef pte_accessible
# define pte_accessible(mm, pte)	((void)(pte), 1)
#endif

#ifndef flush_tlb_fix_spurious_fault
#define flush_tlb_fix_spurious_fault(vma, address, ptep) flush_tlb_page(vma, address)
#endif

/*
 * When walking page tables, get the address of the next boundary,
 * or the end address of the range if that comes earlier.  Although no
 * vma end wraps to 0, rounded up __boundary may wrap to 0 throughout.
 */

#define pgd_addr_end(addr, end)						\
({	unsigned long __boundary = ((addr) + PGDIR_SIZE) & PGDIR_MASK;	\
	(__boundary - 1 < (end) - 1)? __boundary: (end);		\
})

#ifndef p4d_addr_end
#define p4d_addr_end(addr, end)						\
({	unsigned long __boundary = ((addr) + P4D_SIZE) & P4D_MASK;	\
	(__boundary - 1 < (end) - 1)? __boundary: (end);		\
})
#endif

#ifndef pud_addr_end
#define pud_addr_end(addr, end)						\
({	unsigned long __boundary = ((addr) + PUD_SIZE) & PUD_MASK;	\
	(__boundary - 1 < (end) - 1)? __boundary: (end);		\
})
#endif

#ifndef pmd_addr_end
#define pmd_addr_end(addr, end)						\
({	unsigned long __boundary = ((addr) + PMD_SIZE) & PMD_MASK;	\
	(__boundary - 1 < (end) - 1)? __boundary: (end);		\
})
#endif

/*
 * When walking page tables, we usually want to skip any p?d_none entries;
 * and any p?d_bad entries - reporting the error before resetting to none.
 * Do the tests inline, but report and clear the bad entry in mm/memory.c.
 */
void pgd_clear_bad(pgd_t *);

#ifndef __PAGETABLE_P4D_FOLDED
void p4d_clear_bad(p4d_t *);
#else
#define p4d_clear_bad(p4d)        do { } while (0)
#endif

#ifndef __PAGETABLE_PUD_FOLDED
void pud_clear_bad(pud_t *);
#else
#define pud_clear_bad(p4d)        do { } while (0)
#endif

void pmd_clear_bad(pmd_t *);

static inline int pgd_none_or_clear_bad(pgd_t *pgd)
{
	if (pgd_none(*pgd))
		return 1;
	if (unlikely(pgd_bad(*pgd))) {
		pgd_clear_bad(pgd);
		return 1;
	}
	return 0;
}

static inline int p4d_none_or_clear_bad(p4d_t *p4d)
{
	if (p4d_none(*p4d))
		return 1;
	if (unlikely(p4d_bad(*p4d))) {
		p4d_clear_bad(p4d);
		return 1;
	}
	return 0;
}

static inline int pud_none_or_clear_bad(pud_t *pud)
{
	if (pud_none(*pud))
		return 1;
	if (unlikely(pud_bad(*pud))) {
		pud_clear_bad(pud);
		return 1;
	}
	return 0;
}

static inline int pmd_none_or_clear_bad(pmd_t *pmd)
{
	if (pmd_none(*pmd))
		return 1;
	if (unlikely(pmd_bad(*pmd))) {
		pmd_clear_bad(pmd);
		return 1;
	}
	return 0;
}

static inline pte_t __ptep_modify_prot_start(struct vm_area_struct *vma,
					     unsigned long addr,
					     pte_t *ptep)
{
	/*
	 * Get the current pte state, but zero it out to make it
	 * non-present, preventing the hardware from asynchronously
	 * updating it.
	 */
	return ptep_get_and_clear(vma->vm_mm, addr, ptep);
}

static inline void __ptep_modify_prot_commit(struct vm_area_struct *vma,
					     unsigned long addr,
					     pte_t *ptep, pte_t pte)
{
	/*
	 * The pte is non-present, so there's no hardware state to
	 * preserve.
	 */
	set_pte_at(vma->vm_mm, addr, ptep, pte);
}

#ifndef __HAVE_ARCH_PTEP_MODIFY_PROT_TRANSACTION
/*
 * Start a pte protection read-modify-write transaction, which
 * protects against asynchronous hardware modifications to the pte.
 * The intention is not to prevent the hardware from making pte
 * updates, but to prevent any updates it may make from being lost.
 *
 * This does not protect against other software modifications of the
 * pte; the appropriate pte lock must be held over the transaction.
 *
 * Note that this interface is intended to be batchable, meaning that
 * ptep_modify_prot_commit may not actually update the pte, but merely
 * queue the update to be done at some later time.  The update must be
 * actually committed before the pte lock is released, however.
 */
static inline pte_t ptep_modify_prot_start(struct vm_area_struct *vma,
					   unsigned long addr,
					   pte_t *ptep)
{
	return __ptep_modify_prot_start(vma, addr, ptep);
}

/*
 * Commit an update to a pte, leaving any hardware-controlled bits in
 * the PTE unmodified. The pte returned from ptep_modify_prot_start() may
 * additionally have young and/or dirty bits set where previously they were not,
 * so the updated pte may have these additional changes.
 */
static inline void ptep_modify_prot_commit(struct vm_area_struct *vma,
					   unsigned long addr,
					   pte_t *ptep, pte_t old_pte, pte_t pte)
{
	__ptep_modify_prot_commit(vma, addr, ptep, pte);
}
#endif /* __HAVE_ARCH_PTEP_MODIFY_PROT_TRANSACTION */

/**
 * modify_prot_start_ptes - Start a pte protection read-modify-write transaction
 * over a batch of ptes, which protects against asynchronous hardware
 * modifications to the ptes. The intention is not to prevent the hardware from
 * making pte updates, but to prevent any updates it may make from being lost.
 * Please see the comment above ptep_modify_prot_start() for full description.
 *
 * @vma: The virtual memory area the pages are mapped into.
 * @addr: Address the first page is mapped at.
 * @ptep: Page table pointer for the first entry.
 * @nr: Number of entries.
 *
 * May be overridden by the architecture; otherwise, implemented as a simple
 * loop over ptep_modify_prot_start(), collecting the a/d bits from each pte
 * in the batch.
 *
 * Note that PTE bits in the PTE batch besides the PFN can differ.
 *
 * Context: The caller holds the page table lock.  The PTEs map consecutive
 * pages that belong to the same folio. All other PTE bits must be identical for
 * all PTEs in the batch except for young and dirty bits.  The PTEs are all in
 * the same PMD.
 */
#ifndef modify_prot_start_ptes
static inline pte_t modify_prot_start_ptes(struct vm_area_struct *vma,
		unsigned long addr, pte_t *ptep, unsigned int nr)
{
	pte_t pte, tmp_pte;

	pte = ptep_modify_prot_start(vma, addr, ptep);
	while (--nr) {
		ptep++;
		addr += PAGE_SIZE;
		tmp_pte = ptep_modify_prot_start(vma, addr, ptep);
		if (pte_dirty(tmp_pte))
			pte = pte_mkdirty(pte);
		if (pte_young(tmp_pte))
			pte = pte_mkyoung(pte);
	}
	return pte;
}
#endif

/**
 * modify_prot_commit_ptes - Commit an update to a batch of ptes, leaving any
 * hardware-controlled bits in the PTE unmodified.
 *
 * @vma: The virtual memory area the pages are mapped into.
 * @addr: Address the first page is mapped at.
 * @ptep: Page table pointer for the first entry.
 * @old_pte: Old page table entry (for the first entry) which is now cleared.
 * @pte: New page table entry to be set.
 * @nr: Number of entries.
 *
 * May be overridden by the architecture; otherwise, implemented as a simple
 * loop over ptep_modify_prot_commit().
 *
 * Context: The caller holds the page table lock. The PTEs are all in the same
 * PMD. On exit, the set ptes in the batch map the same folio. The ptes set by
 * ptep_modify_prot_start() may additionally have young and/or dirty bits set
 * where previously they were not, so the updated ptes may have these
 * additional changes.
 */
#ifndef modify_prot_commit_ptes
static inline void modify_prot_commit_ptes(struct vm_area_struct *vma, unsigned long addr,
		pte_t *ptep, pte_t old_pte, pte_t pte, unsigned int nr)
{
	int i;

	for (i = 0; i < nr; ++i, ++ptep, addr += PAGE_SIZE) {
		ptep_modify_prot_commit(vma, addr, ptep, old_pte, pte);

		/* Advance PFN only, set same prot */
		old_pte = pte_next_pfn(old_pte);
		pte = pte_next_pfn(pte);
	}
}
#endif

#endif /* CONFIG_MMU */

/*
 * No-op macros that just return the current protection value. Defined here
 * because these macros can be used even if CONFIG_MMU is not defined.
 */

#ifndef pgprot_nx
#define pgprot_nx(prot)	(prot)
#endif

#ifndef pgprot_noncached
#define pgprot_noncached(prot)	(prot)
#endif

#ifndef pgprot_writecombine
#define pgprot_writecombine pgprot_noncached
#endif

#ifndef pgprot_writethrough
#define pgprot_writethrough pgprot_noncached
#endif

#ifndef pgprot_device
#define pgprot_device pgprot_noncached
#endif

#ifndef pgprot_mhp
#define pgprot_mhp(prot)	(prot)
#endif

#ifdef CONFIG_MMU
#ifndef pgprot_modify
#define pgprot_modify pgprot_modify
static inline pgprot_t pgprot_modify(pgprot_t oldprot, pgprot_t newprot)
{
	if (pgprot_val(oldprot) == pgprot_val(pgprot_noncached(oldprot)))
		newprot = pgprot_noncached(newprot);
	if (pgprot_val(oldprot) == pgprot_val(pgprot_writecombine(oldprot)))
		newprot = pgprot_writecombine(newprot);
	if (pgprot_val(oldprot) == pgprot_val(pgprot_device(oldprot)))
		newprot = pgprot_device(newprot);
	return newprot;
}
#endif
#endif /* CONFIG_MMU */

#ifndef pgprot_encrypted
#define pgprot_encrypted(prot)	(prot)
#endif

#ifndef pgprot_decrypted
#define pgprot_decrypted(prot)	(prot)
#endif

/*
 * A facility to provide batching of the reload of page tables and
 * other process state with the actual context switch code for
 * paravirtualized guests.  By convention, only one of the batched
 * update (lazy) modes (CPU, MMU) should be active at any given time,
 * entry should never be nested, and entry and exits should always be
 * paired.  This is for sanity of maintaining and reasoning about the
 * kernel code.  In this case, the exit (end of the context switch) is
 * in architecture-specific code, and so doesn't need a generic
 * definition.
 */
#ifndef __HAVE_ARCH_START_CONTEXT_SWITCH
#define arch_start_context_switch(prev)	do {} while (0)
#endif

#ifdef CONFIG_HAVE_ARCH_SOFT_DIRTY
#ifndef CONFIG_ARCH_ENABLE_THP_MIGRATION
static inline pmd_t pmd_swp_mksoft_dirty(pmd_t pmd)
{
	return pmd;
}

static inline int pmd_swp_soft_dirty(pmd_t pmd)
{
	return 0;
}

static inline pmd_t pmd_swp_clear_soft_dirty(pmd_t pmd)
{
	return pmd;
}
#endif
#else /* !CONFIG_HAVE_ARCH_SOFT_DIRTY */
static inline int pte_soft_dirty(pte_t pte)
{
	return 0;
}

static inline int pmd_soft_dirty(pmd_t pmd)
{
	return 0;
}

static inline pte_t pte_mksoft_dirty(pte_t pte)
{
	return pte;
}

static inline pmd_t pmd_mksoft_dirty(pmd_t pmd)
{
	return pmd;
}

static inline pte_t pte_clear_soft_dirty(pte_t pte)
{
	return pte;
}

static inline pmd_t pmd_clear_soft_dirty(pmd_t pmd)
{
	return pmd;
}

static inline pte_t pte_swp_mksoft_dirty(pte_t pte)
{
	return pte;
}

static inline int pte_swp_soft_dirty(pte_t pte)
{
	return 0;
}

static inline pte_t pte_swp_clear_soft_dirty(pte_t pte)
{
	return pte;
}

static inline pmd_t pmd_swp_mksoft_dirty(pmd_t pmd)
{
	return pmd;
}

static inline int pmd_swp_soft_dirty(pmd_t pmd)
{
	return 0;
}

static inline pmd_t pmd_swp_clear_soft_dirty(pmd_t pmd)
{
	return pmd;
}
#endif

#ifndef __HAVE_PFNMAP_TRACKING
/*
 * Interfaces that can be used by architecture code to keep track of
 * memory type of pfn mappings specified by the remap_pfn_range,
 * vmf_insert_pfn.
 */

static inline int pfnmap_setup_cachemode(unsigned long pfn, unsigned long size,
		pgprot_t *prot)
{
	return 0;
}

static inline int pfnmap_track(unsigned long pfn, unsigned long size,
		pgprot_t *prot)
{
	return 0;
}

static inline void pfnmap_untrack(unsigned long pfn, unsigned long size)
{
}
#else
/**
 * pfnmap_setup_cachemode - setup the cachemode in the pgprot for a pfn range
 * @pfn: the start of the pfn range
 * @size: the size of the pfn range in bytes
 * @prot: the pgprot to modify
 *
 * Lookup the cachemode for the pfn range starting at @pfn with the size
 * @size and store it in @prot, leaving other data in @prot unchanged.
 *
 * This allows for a hardware implementation to have fine-grained control of
 * memory cache behavior at page level granularity. Without a hardware
 * implementation, this function does nothing.
 *
 * Currently there is only one implementation for this - x86 Page Attribute
 * Table (PAT). See Documentation/arch/x86/pat.rst for more details.
 *
 * This function can fail if the pfn range spans pfns that require differing
 * cachemodes. If the pfn range was previously verified to have a single
 * cachemode, it is sufficient to query only a single pfn. The assumption is
 * that this is the case for drivers using the vmf_insert_pfn*() interface.
 *
 * Returns 0 on success and -EINVAL on error.
 */
int pfnmap_setup_cachemode(unsigned long pfn, unsigned long size,
		pgprot_t *prot);

/**
 * pfnmap_track - track a pfn range
 * @pfn: the start of the pfn range
 * @size: the size of the pfn range in bytes
 * @prot: the pgprot to track
 *
 * Requested the pfn range to be 'tracked' by a hardware implementation and
 * setup the cachemode in @prot similar to pfnmap_setup_cachemode().
 *
 * This allows for fine-grained control of memory cache behaviour at page
 * level granularity. Tracking memory this way is persisted across VMA splits
 * (VMA merging does not apply for VM_PFNMAP).
 *
 * Currently, there is only one implementation for this - x86 Page Attribute
 * Table (PAT). See Documentation/arch/x86/pat.rst for more details.
 *
 * Returns 0 on success and -EINVAL on error.
 */
int pfnmap_track(unsigned long pfn, unsigned long size, pgprot_t *prot);

/**
 * pfnmap_untrack - untrack a pfn range
 * @pfn: the start of the pfn range
 * @size: the size of the pfn range in bytes
 *
 * Untrack a pfn range previously tracked through pfnmap_track().
 */
void pfnmap_untrack(unsigned long pfn, unsigned long size);
#endif

/**
 * pfnmap_setup_cachemode_pfn - setup the cachemode in the pgprot for a pfn
 * @pfn: the pfn
 * @prot: the pgprot to modify
 *
 * Lookup the cachemode for @pfn and store it in @prot, leaving other
 * data in @prot unchanged.
 *
 * See pfnmap_setup_cachemode() for details.
 */
static inline void pfnmap_setup_cachemode_pfn(unsigned long pfn, pgprot_t *prot)
{
	pfnmap_setup_cachemode(pfn, PAGE_SIZE, prot);
}

#ifdef CONFIG_MMU
#ifdef __HAVE_COLOR_ZERO_PAGE
static inline int is_zero_pfn(unsigned long pfn)
{
	extern unsigned long zero_pfn;
	unsigned long offset_from_zero_pfn = pfn - zero_pfn;
	return offset_from_zero_pfn <= (zero_page_mask >> PAGE_SHIFT);
}

#define my_zero_pfn(addr)	page_to_pfn(ZERO_PAGE(addr))

#else
static inline int is_zero_pfn(unsigned long pfn)
{
	extern unsigned long zero_pfn;
	return pfn == zero_pfn;
}

static inline unsigned long my_zero_pfn(unsigned long addr)
{
	extern unsigned long zero_pfn;
	return zero_pfn;
}
#endif
#else
static inline int is_zero_pfn(unsigned long pfn)
{
	return 0;
}

static inline unsigned long my_zero_pfn(unsigned long addr)
{
	return 0;
}
#endif /* CONFIG_MMU */

#ifdef CONFIG_MMU

#ifndef CONFIG_TRANSPARENT_HUGEPAGE
static inline int pmd_trans_huge(pmd_t pmd)
{
	return 0;
}
#ifndef pmd_write
static inline int pmd_write(pmd_t pmd)
{
	BUG();
	return 0;
}
#endif /* pmd_write */
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */

#ifndef pud_write
static inline int pud_write(pud_t pud)
{
	BUG();
	return 0;
}
#endif /* pud_write */

#if !defined(CONFIG_TRANSPARENT_HUGEPAGE) || \
	!defined(CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD)
static inline int pud_trans_huge(pud_t pud)
{
	return 0;
}
#endif

static inline int pud_trans_unstable(pud_t *pud)
{
#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && \
	defined(CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD)
	pud_t pudval = READ_ONCE(*pud);

	if (pud_none(pudval) || pud_trans_huge(pudval))
		return 1;
	if (unlikely(pud_bad(pudval))) {
		pud_clear_bad(pud);
		return 1;
	}
#endif
	return 0;
}

#ifndef CONFIG_NUMA_BALANCING
/*
 * In an inaccessible (PROT_NONE) VMA, pte_protnone() may indicate "yes". It is
 * perfectly valid to indicate "no" in that case, which is why our default
 * implementation defaults to "always no".
 *
 * In an accessible VMA, however, pte_protnone() reliably indicates PROT_NONE
 * page protection due to NUMA hinting. NUMA hinting faults only apply in
 * accessible VMAs.
 *
 * So, to reliably identify PROT_NONE PTEs that require a NUMA hinting fault,
 * looking at the VMA accessibility is sufficient.
 */
static inline int pte_protnone(pte_t pte)
{
	return 0;
}

static inline int pmd_protnone(pmd_t pmd)
{
	return 0;
}
#endif /* CONFIG_NUMA_BALANCING */

#endif /* CONFIG_MMU */

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP

#ifndef __PAGETABLE_P4D_FOLDED
int p4d_set_huge(p4d_t *p4d, phys_addr_t addr, pgprot_t prot);
void p4d_clear_huge(p4d_t *p4d);
#else
static inline int p4d_set_huge(p4d_t *p4d, phys_addr_t addr, pgprot_t prot)
{
	return 0;
}
static inline void p4d_clear_huge(p4d_t *p4d) { }
#endif /* !__PAGETABLE_P4D_FOLDED */

int pud_set_huge(pud_t *pud, phys_addr_t addr, pgprot_t prot);
int pmd_set_huge(pmd_t *pmd, phys_addr_t addr, pgprot_t prot);
int pud_clear_huge(pud_t *pud);
int pmd_clear_huge(pmd_t *pmd);
int p4d_free_pud_page(p4d_t *p4d, unsigned long addr);
int pud_free_pmd_page(pud_t *pud, unsigned long addr);
int pmd_free_pte_page(pmd_t *pmd, unsigned long addr);
#else	/* !CONFIG_HAVE_ARCH_HUGE_VMAP */
static inline int p4d_set_huge(p4d_t *p4d, phys_addr_t addr, pgprot_t prot)
{
	return 0;
}
static inline int pud_set_huge(pud_t *pud, phys_addr_t addr, pgprot_t prot)
{
	return 0;
}
static inline int pmd_set_huge(pmd_t *pmd, phys_addr_t addr, pgprot_t prot)
{
	return 0;
}
static inline void p4d_clear_huge(p4d_t *p4d) { }
static inline int pud_clear_huge(pud_t *pud)
{
	return 0;
}
static inline int pmd_clear_huge(pmd_t *pmd)
{
	return 0;
}
static inline int p4d_free_pud_page(p4d_t *p4d, unsigned long addr)
{
	return 0;
}
static inline int pud_free_pmd_page(pud_t *pud, unsigned long addr)
{
	return 0;
}
static inline int pmd_free_pte_page(pmd_t *pmd, unsigned long addr)
{
	return 0;
}
#endif	/* CONFIG_HAVE_ARCH_HUGE_VMAP */

#ifndef __HAVE_ARCH_FLUSH_PMD_TLB_RANGE
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/*
 * ARCHes with special requirements for evicting THP backing TLB entries can
 * implement this. Otherwise also, it can help optimize normal TLB flush in
 * THP regime. Stock flush_tlb_range() typically has optimization to nuke the
 * entire TLB if flush span is greater than a threshold, which will
 * likely be true for a single huge page. Thus a single THP flush will
 * invalidate the entire TLB which is not desirable.
 * e.g. see arch/arc: flush_pmd_tlb_range
 */
#define flush_pmd_tlb_range(vma, addr, end)	flush_tlb_range(vma, addr, end)
#define flush_pud_tlb_range(vma, addr, end)	flush_tlb_range(vma, addr, end)
#else
#define flush_pmd_tlb_range(vma, addr, end)	BUILD_BUG()
#define flush_pud_tlb_range(vma, addr, end)	BUILD_BUG()
#endif
#endif

struct file;
int phys_mem_access_prot_allowed(struct file *file, unsigned long pfn,
			unsigned long size, pgprot_t *vma_prot);

#ifndef CONFIG_X86_ESPFIX64
static inline void init_espfix_bsp(void) { }
#endif

extern void __init pgtable_cache_init(void);

#ifndef __HAVE_ARCH_PFN_MODIFY_ALLOWED
static inline bool pfn_modify_allowed(unsigned long pfn, pgprot_t prot)
{
	return true;
}

static inline bool arch_has_pfn_modify_check(void)
{
	return false;
}
#endif /* !_HAVE_ARCH_PFN_MODIFY_ALLOWED */

/*
 * Architecture PAGE_KERNEL_* fallbacks
 *
 * Some architectures don't define certain PAGE_KERNEL_* flags. This is either
 * because they really don't support them, or the port needs to be updated to
 * reflect the required functionality. Below are a set of relatively safe
 * fallbacks, as best effort, which we can count on in lieu of the architectures
 * not defining them on their own yet.
 */

#ifndef PAGE_KERNEL_RO
# define PAGE_KERNEL_RO PAGE_KERNEL
#endif

#ifndef PAGE_KERNEL_EXEC
# define PAGE_KERNEL_EXEC PAGE_KERNEL
#endif

/*
 * Page Table Modification bits for pgtbl_mod_mask.
 *
 * These are used by the p?d_alloc_track*() set of functions an in the generic
 * vmalloc/ioremap code to track at which page-table levels entries have been
 * modified. Based on that the code can better decide when vmalloc and ioremap
 * mapping changes need to be synchronized to other page-tables in the system.
 */
#define		__PGTBL_PGD_MODIFIED	0
#define		__PGTBL_P4D_MODIFIED	1
#define		__PGTBL_PUD_MODIFIED	2
#define		__PGTBL_PMD_MODIFIED	3
#define		__PGTBL_PTE_MODIFIED	4

#define		PGTBL_PGD_MODIFIED	BIT(__PGTBL_PGD_MODIFIED)
#define		PGTBL_P4D_MODIFIED	BIT(__PGTBL_P4D_MODIFIED)
#define		PGTBL_PUD_MODIFIED	BIT(__PGTBL_PUD_MODIFIED)
#define		PGTBL_PMD_MODIFIED	BIT(__PGTBL_PMD_MODIFIED)
#define		PGTBL_PTE_MODIFIED	BIT(__PGTBL_PTE_MODIFIED)

/* Page-Table Modification Mask */
typedef unsigned int pgtbl_mod_mask;

#endif /* !__ASSEMBLY__ */

#if !defined(MAX_POSSIBLE_PHYSMEM_BITS) && !defined(CONFIG_64BIT)
#ifdef CONFIG_PHYS_ADDR_T_64BIT
/*
 * ZSMALLOC needs to know the highest PFN on 32-bit architectures
 * with physical address space extension, but falls back to
 * BITS_PER_LONG otherwise.
 */
#error Missing MAX_POSSIBLE_PHYSMEM_BITS definition
#else
#define MAX_POSSIBLE_PHYSMEM_BITS 32
#endif
#endif

#ifndef has_transparent_hugepage
#define has_transparent_hugepage() IS_BUILTIN(CONFIG_TRANSPARENT_HUGEPAGE)
#endif

#ifndef has_transparent_pud_hugepage
#define has_transparent_pud_hugepage() IS_BUILTIN(CONFIG_HAVE_ARCH_TRANSPARENT_HUGEPAGE_PUD)
#endif
/*
 * On some architectures it depends on the mm if the p4d/pud or pmd
 * layer of the page table hierarchy is folded or not.
 */
#ifndef mm_p4d_folded
#define mm_p4d_folded(mm)	__is_defined(__PAGETABLE_P4D_FOLDED)
#endif

#ifndef mm_pud_folded
#define mm_pud_folded(mm)	__is_defined(__PAGETABLE_PUD_FOLDED)
#endif

#ifndef mm_pmd_folded
#define mm_pmd_folded(mm)	__is_defined(__PAGETABLE_PMD_FOLDED)
#endif

#ifndef p4d_offset_lockless
#define p4d_offset_lockless(pgdp, pgd, address) p4d_offset(&(pgd), address)
#endif
#ifndef pud_offset_lockless
#define pud_offset_lockless(p4dp, p4d, address) pud_offset(&(p4d), address)
#endif
#ifndef pmd_offset_lockless
#define pmd_offset_lockless(pudp, pud, address) pmd_offset(&(pud), address)
#endif

/*
 * pXd_leaf() is the API to check whether a pgtable entry is a huge page
 * mapping.  It should work globally across all archs, without any
 * dependency on CONFIG_* options.  For architectures that do not support
 * huge mappings on specific levels, below fallbacks will be used.
 *
 * A leaf pgtable entry should always imply the following:
 *
 * - It is a "present" entry.  IOW, before using this API, please check it
 *   with pXd_present() first. NOTE: it may not always mean the "present
 *   bit" is set.  For example, PROT_NONE entries are always "present".
 *
 * - It should _never_ be a swap entry of any type.  Above "present" check
 *   should have guarded this, but let's be crystal clear on this.
 *
 * - It should contain a huge PFN, which points to a huge page larger than
 *   PAGE_SIZE of the platform.  The PFN format isn't important here.
 *
 * - It should cover all kinds of huge mappings (i.e. pXd_trans_huge()
 *   or hugetlb mappings).
 */
#ifndef pgd_leaf
#define pgd_leaf(x)	false
#endif
#ifndef p4d_leaf
#define p4d_leaf(x)	false
#endif
#ifndef pud_leaf
#define pud_leaf(x)	false
#endif
#ifndef pmd_leaf
#define pmd_leaf(x)	false
#endif

#ifndef pgd_leaf_size
#define pgd_leaf_size(x) (1ULL << PGDIR_SHIFT)
#endif
#ifndef p4d_leaf_size
#define p4d_leaf_size(x) P4D_SIZE
#endif
#ifndef pud_leaf_size
#define pud_leaf_size(x) PUD_SIZE
#endif
#ifndef pmd_leaf_size
#define pmd_leaf_size(x) PMD_SIZE
#endif
#ifndef __pte_leaf_size
#ifndef pte_leaf_size
#define pte_leaf_size(x) PAGE_SIZE
#endif
#define __pte_leaf_size(x,y) pte_leaf_size(y)
#endif

/*
 * We always define pmd_pfn for all archs as it's used in lots of generic
 * code.  Now it happens too for pud_pfn (and can happen for larger
 * mappings too in the future; we're not there yet).  Instead of defining
 * it for all archs (like pmd_pfn), provide a fallback.
 *
 * Note that returning 0 here means any arch that didn't define this can
 * get severely wrong when it hits a real pud leaf.  It's arch's
 * responsibility to properly define it when a huge pud is possible.
 */
#ifndef pud_pfn
#define pud_pfn(x) 0
#endif

/*
 * Some architectures have MMUs that are configurable or selectable at boot
 * time. These lead to variable PTRS_PER_x. For statically allocated arrays it
 * helps to have a static maximum value.
 */

#ifndef MAX_PTRS_PER_PTE
#define MAX_PTRS_PER_PTE PTRS_PER_PTE
#endif

#ifndef MAX_PTRS_PER_PMD
#define MAX_PTRS_PER_PMD PTRS_PER_PMD
#endif

#ifndef MAX_PTRS_PER_PUD
#define MAX_PTRS_PER_PUD PTRS_PER_PUD
#endif

#ifndef MAX_PTRS_PER_P4D
#define MAX_PTRS_PER_P4D PTRS_PER_P4D
#endif

#ifndef pte_pgprot
#define pte_pgprot(x) ((pgprot_t) {0})
#endif

#ifndef pmd_pgprot
#define pmd_pgprot(x) ((pgprot_t) {0})
#endif

#ifndef pud_pgprot
#define pud_pgprot(x) ((pgprot_t) {0})
#endif

/* description of effects of mapping type and prot in current implementation.
 * this is due to the limited x86 page protection hardware.  The expected
 * behavior is in parens:
 *
 * map_type	prot
 *		PROT_NONE	PROT_READ	PROT_WRITE	PROT_EXEC
 * MAP_SHARED	r: (no) no	r: (yes) yes	r: (no) yes	r: (no) yes
 *		w: (no) no	w: (no) no	w: (yes) yes	w: (no) no
 *		x: (no) no	x: (no) yes	x: (no) yes	x: (yes) yes
 *
 * MAP_PRIVATE	r: (no) no	r: (yes) yes	r: (no) yes	r: (no) yes
 *		w: (no) no	w: (no) no	w: (copy) copy	w: (no) no
 *		x: (no) no	x: (no) yes	x: (no) yes	x: (yes) yes
 *
 * On arm64, PROT_EXEC has the following behaviour for both MAP_SHARED and
 * MAP_PRIVATE (with Enhanced PAN supported):
 *								r: (no) no
 *								w: (no) no
 *								x: (yes) yes
 */
#define DECLARE_VM_GET_PAGE_PROT					\
pgprot_t vm_get_page_prot(vm_flags_t vm_flags)				\
{									\
		return protection_map[vm_flags &			\
			(VM_READ | VM_WRITE | VM_EXEC | VM_SHARED)];	\
}									\
EXPORT_SYMBOL(vm_get_page_prot);

#endif /* _LINUX_PGTABLE_H */
