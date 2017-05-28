/*
 * Macros for manipulating and testing page->flags
 */

#ifndef PAGE_FLAGS_H
#define PAGE_FLAGS_H

#include <linux/percpu.h>
#include <linux/cache.h>
#include <asm/pgtable.h>

/*
 * Various page->flags bits:
 *
 * PG_reserved is set for special pages, which can never be swapped out. Some
 * of them might not even exist (eg empty_bad_page)...
 *
 * The PG_private bitflag is set if page->private contains a valid value.
 *
 * During disk I/O, PG_locked is used. This bit is set before I/O and
 * reset when I/O completes. page_waitqueue(page) is a wait queue of all tasks
 * waiting for the I/O on this page to complete.
 *
 * PG_uptodate tells whether the page's contents is valid.  When a read
 * completes, the page becomes uptodate, unless a disk I/O error happened.
 *
 * For choosing which pages to swap out, inode pages carry a PG_referenced bit,
 * which is set any time the system accesses that page through the (mapping,
 * index) hash table.  This referenced bit, together with the referenced bit
 * in the page tables, is used to manipulate page->age and move the page across
 * the active, inactive_dirty and inactive_clean lists.
 *
 * Note that the referenced bit, the page->lru list_head and the active,
 * inactive_dirty and inactive_clean lists are protected by the
 * zone->lru_lock, and *NOT* by the usual PG_locked bit!
 *
 * PG_error is set to indicate that an I/O error occurred on this page.
 *
 * PG_arch_1 is an architecture specific page state bit.  The generic code
 * guarantees that this bit is cleared for a page when it first is entered into
 * the page cache.
 *
 * PG_highmem pages are not permanently mapped into the kernel virtual address
 * space, they need to be kmapped separately for doing IO on the pages.  The
 * struct page (these bits with information) are always mapped into kernel
 * address space...
 */

/*
 * Don't use the *_dontuse flags.  Use the macros.  Otherwise you'll break
 * locked- and dirty-page accounting.  The top eight bits of page->flags are
 * used for page->zone, so putting flag bits there doesn't work.
 */
/**
 * 页框状态标志
 */

/**
 * 页被锁定，例如：在磁盘IO操作中涉及的页。
 */
#define PG_locked	 	 0	/* Page is locked. Don't touch. */
/**
 * 当传输页时发生IO错误
 */
#define PG_error		 1
/**
 * 刚刚访问过的页。
 * 假定在非活动链隔开的一个页其PG_referenced标志置为0，则第一次访问把这个标志置为1，但是这一页仍然留在非活动链表中。
 * 第二次访问时发现这一标志置位，因此将页移到活动链表。
 * 但是如果第一次访问之后在给定时间间隔内第二次访问没有发生，那么页框回收算法就可能重置该标志。
 */
#define PG_referenced		 2
/**
 * 在完成读操作后置位，除非发生磁盘IO错误。
 */
#define PG_uptodate		 3
/**
 * 页已经被修改。
 */
#define PG_dirty	 	 4
/**
 * 页在活动或非活动页链表中。一般是进程用户态地址空间或页高速缓存的页。
 */
#define PG_lru			 5
/**
 * 页在活动页链表中。
 */
#define PG_active		 6
/**
 * 包含在slab中的页框。
 */
#define PG_slab			 7	/* slab debug (Suparna wants this) */

/**
 * 页框属于ZONE_HIGHMEM管理区。
 */
#define PG_highmem		 8
/**
 * 由一些文件系统如ext2和ext3使用的标志。
 */
#define PG_checked		 9	/* kill me in 2.5.<early>. */
/**
 * 在x86上没有使用。
 */
#define PG_arch_1		10
/**
 * 页框保留给内核代码或者没有使用。
 */
#define PG_reserved		11

/**
 * 页描述符的private字段存放了有意义的数据
 */
#define PG_private		12	/* Has something at ->private */
/**
 * 正在使用writepage方法将页写到磁盘上。
 */
#define PG_writeback		13	/* Page is under writeback */
/**
 * 系统挂起、唤醒时使用。
 */
#define PG_nosave		14	/* Used for system suspend/resume */
/**
 * 通过扩展分页机制处理页框。
 */
#define PG_compound		15	/* Part of a compound page */

/**
 * 页属于对换高速
 */
#define PG_swapcache		16	/* Swap page: swp_entry_t in private */
/**
 * 页框中的所有数据对应于磁盘上分配的块。
 */
#define PG_mappedtodisk		17	/* Has blocks allocated on-disk */
/**
 * 为回收内存对页已经做了写入磁盘的标记。
 */
#define PG_reclaim		18	/* To be reclaimed asap */
/**
 * 系统挂起、恢复时使用。
 */
#define PG_nosave_free		19	/* Free, should not be written */


/*
 * Global page accounting.  One instance per CPU.  Only unsigned longs are
 * allowed.
 */
struct page_state {
	unsigned long nr_dirty;		/* Dirty writeable pages */
	unsigned long nr_writeback;	/* Pages under writeback */
	unsigned long nr_unstable;	/* NFS unstable pages */
	unsigned long nr_page_table_pages;/* Pages used for pagetables */
	unsigned long nr_mapped;	/* mapped into pagetables */
	unsigned long nr_slab;		/* In slab */
#define GET_PAGE_STATE_LAST nr_slab

	/*
	 * The below are zeroed by get_page_state().  Use get_full_page_state()
	 * to add up all these.
	 */
	unsigned long pgpgin;		/* Disk reads */
	unsigned long pgpgout;		/* Disk writes */
	unsigned long pswpin;		/* swap reads */
	unsigned long pswpout;		/* swap writes */
	unsigned long pgalloc_high;	/* page allocations */

	unsigned long pgalloc_normal;
	unsigned long pgalloc_dma;
	unsigned long pgfree;		/* page freeings */
	unsigned long pgactivate;	/* pages moved inactive->active */
	unsigned long pgdeactivate;	/* pages moved active->inactive */

	unsigned long pgfault;		/* faults (major+minor) */
	unsigned long pgmajfault;	/* faults (major only) */
	unsigned long pgrefill_high;	/* inspected in refill_inactive_zone */
	unsigned long pgrefill_normal;
	unsigned long pgrefill_dma;

	unsigned long pgsteal_high;	/* total highmem pages reclaimed */
	unsigned long pgsteal_normal;
	unsigned long pgsteal_dma;
	unsigned long pgscan_kswapd_high;/* total highmem pages scanned */
	unsigned long pgscan_kswapd_normal;

	unsigned long pgscan_kswapd_dma;
	unsigned long pgscan_direct_high;/* total highmem pages scanned */
	unsigned long pgscan_direct_normal;
	unsigned long pgscan_direct_dma;
	unsigned long pginodesteal;	/* pages reclaimed via inode freeing */

	unsigned long slabs_scanned;	/* slab objects scanned */
	unsigned long kswapd_steal;	/* pages reclaimed by kswapd */
	unsigned long kswapd_inodesteal;/* reclaimed via kswapd inode freeing */
	unsigned long pageoutrun;	/* kswapd's calls to page reclaim */
	unsigned long allocstall;	/* direct reclaim calls */

	unsigned long pgrotated;	/* pages rotated to tail of the LRU */
};

extern void get_page_state(struct page_state *ret);
extern void get_full_page_state(struct page_state *ret);
extern unsigned long __read_page_state(unsigned offset);
extern void __mod_page_state(unsigned offset, unsigned long delta);

#define read_page_state(member) \
	__read_page_state(offsetof(struct page_state, member))

#define mod_page_state(member, delta)	\
	__mod_page_state(offsetof(struct page_state, member), (delta))

#define inc_page_state(member)	mod_page_state(member, 1UL)
#define dec_page_state(member)	mod_page_state(member, 0UL - 1)
#define add_page_state(member,delta) mod_page_state(member, (delta))
#define sub_page_state(member,delta) mod_page_state(member, 0UL - (delta))

#define mod_page_state_zone(zone, member, delta)				\
	do {									\
		unsigned offset;						\
		if (is_highmem(zone))						\
			offset = offsetof(struct page_state, member##_high);	\
		else if (is_normal(zone))					\
			offset = offsetof(struct page_state, member##_normal);	\
		else								\
			offset = offsetof(struct page_state, member##_dma);	\
		__mod_page_state(offset, (delta));				\
	} while (0)

/*
 * Manipulation of page state flags
 */
#define PageLocked(page)		\
		test_bit(PG_locked, &(page)->flags)
#define SetPageLocked(page)		\
		set_bit(PG_locked, &(page)->flags)
#define TestSetPageLocked(page)		\
		test_and_set_bit(PG_locked, &(page)->flags)
#define ClearPageLocked(page)		\
		clear_bit(PG_locked, &(page)->flags)
#define TestClearPageLocked(page)	\
		test_and_clear_bit(PG_locked, &(page)->flags)

#define PageError(page)		test_bit(PG_error, &(page)->flags)
#define SetPageError(page)	set_bit(PG_error, &(page)->flags)
#define ClearPageError(page)	clear_bit(PG_error, &(page)->flags)

#define PageReferenced(page)	test_bit(PG_referenced, &(page)->flags)
#define SetPageReferenced(page)	set_bit(PG_referenced, &(page)->flags)
#define ClearPageReferenced(page)	clear_bit(PG_referenced, &(page)->flags)
#define TestClearPageReferenced(page) test_and_clear_bit(PG_referenced, &(page)->flags)
/**
 * 查看页是否为最新。PG_uptodate为0表示页不是最新的。
 */
#define PageUptodate(page)	test_bit(PG_uptodate, &(page)->flags)
#ifndef SetPageUptodate
#define SetPageUptodate(page)	set_bit(PG_uptodate, &(page)->flags)
#endif
#define ClearPageUptodate(page)	clear_bit(PG_uptodate, &(page)->flags)

#define PageDirty(page)		test_bit(PG_dirty, &(page)->flags)
#define SetPageDirty(page)	set_bit(PG_dirty, &(page)->flags)
#define TestSetPageDirty(page)	test_and_set_bit(PG_dirty, &(page)->flags)
#define ClearPageDirty(page)	clear_bit(PG_dirty, &(page)->flags)
#define TestClearPageDirty(page) test_and_clear_bit(PG_dirty, &(page)->flags)

#define SetPageLRU(page)	set_bit(PG_lru, &(page)->flags)
#define PageLRU(page)		test_bit(PG_lru, &(page)->flags)
#define TestSetPageLRU(page)	test_and_set_bit(PG_lru, &(page)->flags)
#define TestClearPageLRU(page)	test_and_clear_bit(PG_lru, &(page)->flags)

#define PageActive(page)	test_bit(PG_active, &(page)->flags)
#define SetPageActive(page)	set_bit(PG_active, &(page)->flags)
#define ClearPageActive(page)	clear_bit(PG_active, &(page)->flags)
#define TestClearPageActive(page) test_and_clear_bit(PG_active, &(page)->flags)
#define TestSetPageActive(page) test_and_set_bit(PG_active, &(page)->flags)

#define PageSlab(page)		test_bit(PG_slab, &(page)->flags)
#define SetPageSlab(page)	set_bit(PG_slab, &(page)->flags)
#define ClearPageSlab(page)	clear_bit(PG_slab, &(page)->flags)
#define TestClearPageSlab(page)	test_and_clear_bit(PG_slab, &(page)->flags)
#define TestSetPageSlab(page)	test_and_set_bit(PG_slab, &(page)->flags)

#ifdef CONFIG_HIGHMEM
#define PageHighMem(page)	test_bit(PG_highmem, &(page)->flags)
#else
#define PageHighMem(page)	0 /* needed to optimize away at compile time */
#endif

#define PageChecked(page)	test_bit(PG_checked, &(page)->flags)
#define SetPageChecked(page)	set_bit(PG_checked, &(page)->flags)
#define ClearPageChecked(page)	clear_bit(PG_checked, &(page)->flags)

#define PageReserved(page)	test_bit(PG_reserved, &(page)->flags)
#define SetPageReserved(page)	set_bit(PG_reserved, &(page)->flags)
#define ClearPageReserved(page)	clear_bit(PG_reserved, &(page)->flags)
#define __ClearPageReserved(page)	__clear_bit(PG_reserved, &(page)->flags)

#define SetPagePrivate(page)	set_bit(PG_private, &(page)->flags)
#define ClearPagePrivate(page)	clear_bit(PG_private, &(page)->flags)
/*检查page的PG_private标志。*/
#define PagePrivate(page)	test_bit(PG_private, &(page)->flags)
#define __SetPagePrivate(page)  __set_bit(PG_private, &(page)->flags)
#define __ClearPagePrivate(page) __clear_bit(PG_private, &(page)->flags)
/* 检查页是否正在写回，说明不能释放这些缓冲区，如果是返回1*/
#define PageWriteback(page)	test_bit(PG_writeback, &(page)->flags)
#define SetPageWriteback(page)						\
	do {								\
		if (!test_and_set_bit(PG_writeback,			\
				&(page)->flags))			\
			inc_page_state(nr_writeback);			\
	} while (0)
#define TestSetPageWriteback(page)					\
	({								\
		int ret;						\
		ret = test_and_set_bit(PG_writeback,			\
					&(page)->flags);		\
		if (!ret)						\
			inc_page_state(nr_writeback);			\
		ret;							\
	})
#define ClearPageWriteback(page)					\
	do {								\
		if (test_and_clear_bit(PG_writeback,			\
				&(page)->flags))			\
			dec_page_state(nr_writeback);			\
	} while (0)
#define TestClearPageWriteback(page)					\
	({								\
		int ret;						\
		ret = test_and_clear_bit(PG_writeback,			\
				&(page)->flags);			\
		if (ret)						\
			dec_page_state(nr_writeback);			\
		ret;							\
	})

#define PageNosave(page)	test_bit(PG_nosave, &(page)->flags)
#define SetPageNosave(page)	set_bit(PG_nosave, &(page)->flags)
#define TestSetPageNosave(page)	test_and_set_bit(PG_nosave, &(page)->flags)
#define ClearPageNosave(page)		clear_bit(PG_nosave, &(page)->flags)
#define TestClearPageNosave(page)	test_and_clear_bit(PG_nosave, &(page)->flags)

#define PageNosaveFree(page)	test_bit(PG_nosave_free, &(page)->flags)
#define SetPageNosaveFree(page)	set_bit(PG_nosave_free, &(page)->flags)
#define ClearPageNosaveFree(page)		clear_bit(PG_nosave_free, &(page)->flags)

#define PageMappedToDisk(page)	test_bit(PG_mappedtodisk, &(page)->flags)
#define SetPageMappedToDisk(page) set_bit(PG_mappedtodisk, &(page)->flags)
#define ClearPageMappedToDisk(page) clear_bit(PG_mappedtodisk, &(page)->flags)

#define PageReclaim(page)	test_bit(PG_reclaim, &(page)->flags)
#define SetPageReclaim(page)	set_bit(PG_reclaim, &(page)->flags)
#define ClearPageReclaim(page)	clear_bit(PG_reclaim, &(page)->flags)
#define TestClearPageReclaim(page) test_and_clear_bit(PG_reclaim, &(page)->flags)

#ifdef CONFIG_HUGETLB_PAGE
#define PageCompound(page)	test_bit(PG_compound, &(page)->flags)
#else
#define PageCompound(page)	0
#endif
#define SetPageCompound(page)	set_bit(PG_compound, &(page)->flags)
#define ClearPageCompound(page)	clear_bit(PG_compound, &(page)->flags)

#ifdef CONFIG_SWAP
#define PageSwapCache(page)	test_bit(PG_swapcache, &(page)->flags)
#define SetPageSwapCache(page)	set_bit(PG_swapcache, &(page)->flags)
#define ClearPageSwapCache(page) clear_bit(PG_swapcache, &(page)->flags)
#else
#define PageSwapCache(page)	0
#endif

struct page;	/* forward declaration */

int test_clear_page_dirty(struct page *page);
int __clear_page_dirty(struct page *page);
int test_clear_page_writeback(struct page *page);
int test_set_page_writeback(struct page *page);

static inline void clear_page_dirty(struct page *page)
{
	test_clear_page_dirty(page);
}

static inline void set_page_writeback(struct page *page)
{
	test_set_page_writeback(page);
}

#endif	/* PAGE_FLAGS_H */
