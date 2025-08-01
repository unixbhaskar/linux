// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/memblock.h>
#include <linux/page_ext.h>
#include <linux/memory.h>
#include <linux/vmalloc.h>
#include <linux/kmemleak.h>
#include <linux/page_owner.h>
#include <linux/page_idle.h>
#include <linux/page_table_check.h>
#include <linux/rcupdate.h>
#include <linux/pgalloc_tag.h>

/*
 * struct page extension
 *
 * This is the feature to manage memory for extended data per page.
 *
 * Until now, we must modify struct page itself to store extra data per page.
 * This requires rebuilding the kernel and it is really time consuming process.
 * And, sometimes, rebuild is impossible due to third party module dependency.
 * At last, enlarging struct page could cause un-wanted system behaviour change.
 *
 * This feature is intended to overcome above mentioned problems. This feature
 * allocates memory for extended data per page in certain place rather than
 * the struct page itself. This memory can be accessed by the accessor
 * functions provided by this code. During the boot process, it checks whether
 * allocation of huge chunk of memory is needed or not. If not, it avoids
 * allocating memory at all. With this advantage, we can include this feature
 * into the kernel in default and can avoid rebuild and solve related problems.
 *
 * To help these things to work well, there are two callbacks for clients. One
 * is the need callback which is mandatory if user wants to avoid useless
 * memory allocation at boot-time. The other is optional, init callback, which
 * is used to do proper initialization after memory is allocated.
 *
 * The need callback is used to decide whether extended memory allocation is
 * needed or not. Sometimes users want to deactivate some features in this
 * boot and extra memory would be unnecessary. In this case, to avoid
 * allocating huge chunk of memory, each clients represent their need of
 * extra memory through the need callback. If one of the need callbacks
 * returns true, it means that someone needs extra memory so that
 * page extension core should allocates memory for page extension. If
 * none of need callbacks return true, memory isn't needed at all in this boot
 * and page extension core can skip to allocate memory. As result,
 * none of memory is wasted.
 *
 * When need callback returns true, page_ext checks if there is a request for
 * extra memory through size in struct page_ext_operations. If it is non-zero,
 * extra space is allocated for each page_ext entry and offset is returned to
 * user through offset in struct page_ext_operations.
 *
 * The init callback is used to do proper initialization after page extension
 * is completely initialized. In sparse memory system, extra memory is
 * allocated some time later than memmap is allocated. In other words, lifetime
 * of memory for page extension isn't same with memmap for struct page.
 * Therefore, clients can't store extra data until page extension is
 * initialized, even if pages are allocated and used freely. This could
 * cause inadequate state of extra data per page, so, to prevent it, client
 * can utilize this callback to initialize the state of it correctly.
 */

#ifdef CONFIG_SPARSEMEM
#define PAGE_EXT_INVALID       (0x1)
#endif

#if defined(CONFIG_PAGE_IDLE_FLAG) && !defined(CONFIG_64BIT)
static bool need_page_idle(void)
{
	return true;
}
static struct page_ext_operations page_idle_ops __initdata = {
	.need = need_page_idle,
	.need_shared_flags = true,
};
#endif

static struct page_ext_operations *page_ext_ops[] __initdata = {
#ifdef CONFIG_PAGE_OWNER
	&page_owner_ops,
#endif
#if defined(CONFIG_PAGE_IDLE_FLAG) && !defined(CONFIG_64BIT)
	&page_idle_ops,
#endif
#ifdef CONFIG_MEM_ALLOC_PROFILING
	&page_alloc_tagging_ops,
#endif
#ifdef CONFIG_PAGE_TABLE_CHECK
	&page_table_check_ops,
#endif
};

unsigned long page_ext_size;

static unsigned long total_usage;

#ifdef CONFIG_MEM_ALLOC_PROFILING_DEBUG
/*
 * To ensure correct allocation tagging for pages, page_ext should be available
 * before the first page allocation. Otherwise early task stacks will be
 * allocated before page_ext initialization and missing tags will be flagged.
 */
bool early_page_ext __meminitdata = true;
#else
bool early_page_ext __meminitdata;
#endif
static int __init setup_early_page_ext(char *str)
{
	early_page_ext = true;
	return 0;
}
early_param("early_page_ext", setup_early_page_ext);

static bool __init invoke_need_callbacks(void)
{
	int i;
	int entries = ARRAY_SIZE(page_ext_ops);
	bool need = false;

	for (i = 0; i < entries; i++) {
		if (page_ext_ops[i]->need()) {
			if (page_ext_ops[i]->need_shared_flags) {
				page_ext_size = sizeof(struct page_ext);
				break;
			}
		}
	}

	for (i = 0; i < entries; i++) {
		if (page_ext_ops[i]->need()) {
			page_ext_ops[i]->offset = page_ext_size;
			page_ext_size += page_ext_ops[i]->size;
			need = true;
		}
	}

	return need;
}

static void __init invoke_init_callbacks(void)
{
	int i;
	int entries = ARRAY_SIZE(page_ext_ops);

	for (i = 0; i < entries; i++) {
		if (page_ext_ops[i]->init)
			page_ext_ops[i]->init();
	}
}

static inline struct page_ext *get_entry(void *base, unsigned long index)
{
	return base + page_ext_size * index;
}

#ifndef CONFIG_SPARSEMEM
void __init page_ext_init_flatmem_late(void)
{
	invoke_init_callbacks();
}

void __meminit pgdat_page_ext_init(struct pglist_data *pgdat)
{
	pgdat->node_page_ext = NULL;
}

static struct page_ext *lookup_page_ext(const struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	unsigned long index;
	struct page_ext *base;

	WARN_ON_ONCE(!rcu_read_lock_held());
	base = NODE_DATA(page_to_nid(page))->node_page_ext;
	/*
	 * The sanity checks the page allocator does upon freeing a
	 * page can reach here before the page_ext arrays are
	 * allocated when feeding a range of pages to the allocator
	 * for the first time during bootup or memory hotplug.
	 */
	if (unlikely(!base))
		return NULL;
	index = pfn - round_down(node_start_pfn(page_to_nid(page)),
					MAX_ORDER_NR_PAGES);
	return get_entry(base, index);
}

static int __init alloc_node_page_ext(int nid)
{
	struct page_ext *base;
	unsigned long table_size;
	unsigned long nr_pages;

	nr_pages = NODE_DATA(nid)->node_spanned_pages;
	if (!nr_pages)
		return 0;

	/*
	 * Need extra space if node range is not aligned with
	 * MAX_ORDER_NR_PAGES. When page allocator's buddy algorithm
	 * checks buddy's status, range could be out of exact node range.
	 */
	if (!IS_ALIGNED(node_start_pfn(nid), MAX_ORDER_NR_PAGES) ||
		!IS_ALIGNED(node_end_pfn(nid), MAX_ORDER_NR_PAGES))
		nr_pages += MAX_ORDER_NR_PAGES;

	table_size = page_ext_size * nr_pages;

	base = memblock_alloc_try_nid(
			table_size, PAGE_SIZE, __pa(MAX_DMA_ADDRESS),
			MEMBLOCK_ALLOC_ACCESSIBLE, nid);
	if (!base)
		return -ENOMEM;
	NODE_DATA(nid)->node_page_ext = base;
	total_usage += table_size;
	memmap_boot_pages_add(DIV_ROUND_UP(table_size, PAGE_SIZE));
	return 0;
}

void __init page_ext_init_flatmem(void)
{

	int nid, fail;

	if (!invoke_need_callbacks())
		return;

	for_each_online_node(nid)  {
		fail = alloc_node_page_ext(nid);
		if (fail)
			goto fail;
	}
	pr_info("allocated %ld bytes of page_ext\n", total_usage);
	return;

fail:
	pr_crit("allocation of page_ext failed.\n");
	panic("Out of memory");
}

#else /* CONFIG_SPARSEMEM */
static bool page_ext_invalid(struct page_ext *page_ext)
{
	return !page_ext || (((unsigned long)page_ext & PAGE_EXT_INVALID) == PAGE_EXT_INVALID);
}

static struct page_ext *lookup_page_ext(const struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	struct mem_section *section = __pfn_to_section(pfn);
	struct page_ext *page_ext = READ_ONCE(section->page_ext);

	WARN_ON_ONCE(!rcu_read_lock_held());
	/*
	 * The sanity checks the page allocator does upon freeing a
	 * page can reach here before the page_ext arrays are
	 * allocated when feeding a range of pages to the allocator
	 * for the first time during bootup or memory hotplug.
	 */
	if (page_ext_invalid(page_ext))
		return NULL;
	return get_entry(page_ext, pfn);
}

static void *__meminit alloc_page_ext(size_t size, int nid)
{
	gfp_t flags = GFP_KERNEL | __GFP_ZERO | __GFP_NOWARN;
	void *addr = NULL;

	addr = alloc_pages_exact_nid(nid, size, flags);
	if (addr)
		kmemleak_alloc(addr, size, 1, flags);
	else
		addr = vzalloc_node(size, nid);

	if (addr)
		memmap_pages_add(DIV_ROUND_UP(size, PAGE_SIZE));

	return addr;
}

static int __meminit init_section_page_ext(unsigned long pfn, int nid)
{
	struct mem_section *section;
	struct page_ext *base;
	unsigned long table_size;

	section = __pfn_to_section(pfn);

	if (section->page_ext)
		return 0;

	table_size = page_ext_size * PAGES_PER_SECTION;
	base = alloc_page_ext(table_size, nid);

	/*
	 * The value stored in section->page_ext is (base - pfn)
	 * and it does not point to the memory block allocated above,
	 * causing kmemleak false positives.
	 */
	kmemleak_not_leak(base);

	if (!base) {
		pr_err("page ext allocation failure\n");
		return -ENOMEM;
	}

	/*
	 * The passed "pfn" may not be aligned to SECTION.  For the calculation
	 * we need to apply a mask.
	 */
	pfn &= PAGE_SECTION_MASK;
	section->page_ext = (void *)base - page_ext_size * pfn;
	total_usage += table_size;
	return 0;
}

static void free_page_ext(void *addr)
{
	size_t table_size;
	struct page *page;

	table_size = page_ext_size * PAGES_PER_SECTION;
	memmap_pages_add(-1L * (DIV_ROUND_UP(table_size, PAGE_SIZE)));

	if (is_vmalloc_addr(addr)) {
		vfree(addr);
	} else {
		page = virt_to_page(addr);
		BUG_ON(PageReserved(page));
		kmemleak_free(addr);
		free_pages_exact(addr, table_size);
	}
}

static void __free_page_ext(unsigned long pfn)
{
	struct mem_section *ms;
	struct page_ext *base;

	ms = __pfn_to_section(pfn);
	if (!ms || !ms->page_ext)
		return;

	base = READ_ONCE(ms->page_ext);
	/*
	 * page_ext here can be valid while doing the roll back
	 * operation in online_page_ext().
	 */
	if (page_ext_invalid(base))
		base = (void *)base - PAGE_EXT_INVALID;
	WRITE_ONCE(ms->page_ext, NULL);

	base = get_entry(base, pfn);
	free_page_ext(base);
}

static void __invalidate_page_ext(unsigned long pfn)
{
	struct mem_section *ms;
	void *val;

	ms = __pfn_to_section(pfn);
	if (!ms || !ms->page_ext)
		return;
	val = (void *)ms->page_ext + PAGE_EXT_INVALID;
	WRITE_ONCE(ms->page_ext, val);
}

static int __meminit online_page_ext(unsigned long start_pfn,
				unsigned long nr_pages)
{
	int nid = pfn_to_nid(start_pfn);
	unsigned long start, end, pfn;
	int fail = 0;

	start = SECTION_ALIGN_DOWN(start_pfn);
	end = SECTION_ALIGN_UP(start_pfn + nr_pages);

	for (pfn = start; !fail && pfn < end; pfn += PAGES_PER_SECTION)
		fail = init_section_page_ext(pfn, nid);
	if (!fail)
		return 0;

	/* rollback */
	end = pfn - PAGES_PER_SECTION;
	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION)
		__free_page_ext(pfn);

	return -ENOMEM;
}

static void __meminit offline_page_ext(unsigned long start_pfn,
				unsigned long nr_pages)
{
	unsigned long start, end, pfn;

	start = SECTION_ALIGN_DOWN(start_pfn);
	end = SECTION_ALIGN_UP(start_pfn + nr_pages);

	/*
	 * Freeing of page_ext is done in 3 steps to avoid
	 * use-after-free of it:
	 * 1) Traverse all the sections and mark their page_ext
	 *    as invalid.
	 * 2) Wait for all the existing users of page_ext who
	 *    started before invalidation to finish.
	 * 3) Free the page_ext.
	 */
	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION)
		__invalidate_page_ext(pfn);

	synchronize_rcu();

	for (pfn = start; pfn < end; pfn += PAGES_PER_SECTION)
		__free_page_ext(pfn);
}

static int __meminit page_ext_callback(struct notifier_block *self,
			       unsigned long action, void *arg)
{
	struct memory_notify *mn = arg;
	int ret = 0;

	switch (action) {
	case MEM_GOING_ONLINE:
		ret = online_page_ext(mn->start_pfn, mn->nr_pages);
		break;
	case MEM_OFFLINE:
		offline_page_ext(mn->start_pfn,
				mn->nr_pages);
		break;
	case MEM_CANCEL_ONLINE:
		offline_page_ext(mn->start_pfn,
				mn->nr_pages);
		break;
	case MEM_GOING_OFFLINE:
		break;
	case MEM_ONLINE:
	case MEM_CANCEL_OFFLINE:
		break;
	}

	return notifier_from_errno(ret);
}

void __init page_ext_init(void)
{
	unsigned long pfn;
	int nid;

	if (!invoke_need_callbacks())
		return;

	for_each_node_state(nid, N_MEMORY) {
		unsigned long start_pfn, end_pfn;

		start_pfn = node_start_pfn(nid);
		end_pfn = node_end_pfn(nid);
		/*
		 * start_pfn and end_pfn may not be aligned to SECTION and the
		 * page->flags of out of node pages are not initialized.  So we
		 * scan [start_pfn, the biggest section's pfn < end_pfn) here.
		 */
		for (pfn = start_pfn; pfn < end_pfn;
			pfn = ALIGN(pfn + 1, PAGES_PER_SECTION)) {

			if (!pfn_valid(pfn))
				continue;
			/*
			 * Nodes's pfns can be overlapping.
			 * We know some arch can have a nodes layout such as
			 * -------------pfn-------------->
			 * N0 | N1 | N2 | N0 | N1 | N2|....
			 */
			if (pfn_to_nid(pfn) != nid)
				continue;
			if (init_section_page_ext(pfn, nid))
				goto oom;
			cond_resched();
		}
	}
	hotplug_memory_notifier(page_ext_callback, DEFAULT_CALLBACK_PRI);
	pr_info("allocated %ld bytes of page_ext\n", total_usage);
	invoke_init_callbacks();
	return;

oom:
	panic("Out of memory");
}

void __meminit pgdat_page_ext_init(struct pglist_data *pgdat)
{
}

#endif

/**
 * page_ext_lookup() - Lookup a page extension for a PFN.
 * @pfn: PFN of the page we're interested in.
 *
 * Must be called with RCU read lock taken and @pfn must be valid.
 *
 * Return: NULL if no page_ext exists for this page.
 */
struct page_ext *page_ext_lookup(unsigned long pfn)
{
	return lookup_page_ext(pfn_to_page(pfn));
}

/**
 * page_ext_get() - Get the extended information for a page.
 * @page: The page we're interested in.
 *
 * Ensures that the page_ext will remain valid until page_ext_put()
 * is called.
 *
 * Return: NULL if no page_ext exists for this page.
 * Context: Any context.  Caller may not sleep until they have called
 * page_ext_put().
 */
struct page_ext *page_ext_get(const struct page *page)
{
	struct page_ext *page_ext;

	rcu_read_lock();
	page_ext = lookup_page_ext(page);
	if (!page_ext) {
		rcu_read_unlock();
		return NULL;
	}

	return page_ext;
}

/**
 * page_ext_put() - Working with page extended information is done.
 * @page_ext: Page extended information received from page_ext_get().
 *
 * The page extended information of the page may not be valid after this
 * function is called.
 *
 * Return: None.
 * Context: Any context with corresponding page_ext_get() is called.
 */
void page_ext_put(struct page_ext *page_ext)
{
	if (unlikely(!page_ext))
		return;

	rcu_read_unlock();
}
