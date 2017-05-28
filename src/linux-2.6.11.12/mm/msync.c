/*
 *	linux/mm/msync.c
 *
 * Copyright (C) 1994-1999  Linus Torvalds
 */

/*
 * The msync() system call.
 */
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/hugetlb.h>
#include <linux/syscalls.h>

#include <asm/pgtable.h>
#include <asm/tlbflush.h>

/*
 * Called with mm->page_table_lock held to protect against other
 * threads/the swapper from ripping pte's out from under us.
 */
static int filemap_sync_pte(pte_t *ptep, struct vm_area_struct *vma,
	unsigned long address, unsigned int flags)
{
	pte_t pte = *ptep;
	unsigned long pfn = pte_pfn(pte);
	struct page *page;

	if (pte_present(pte) && pfn_valid(pfn)) {
		page = pfn_to_page(pfn);
		if (!PageReserved(page) &&
		    (ptep_clear_flush_dirty(vma, address, ptep) ||
		     page_test_and_clear_dirty(page))) /*★*/
			set_page_dirty(page); /*★*/
	}
	return 0;
}

static int filemap_sync_pte_range(pmd_t * pmd,
	unsigned long address, unsigned long end, 
	struct vm_area_struct *vma, unsigned int flags)
{
	pte_t *pte;
	int error;

	if (pmd_none(*pmd))
		return 0;
	if (pmd_bad(*pmd)) {
		pmd_ERROR(*pmd);
		pmd_clear(pmd);
		return 0;
	}
	pte = pte_offset_map(pmd, address);
	if ((address & PMD_MASK) != (end & PMD_MASK))
		end = (address & PMD_MASK) + PMD_SIZE;
	error = 0;
	do {
		error |= filemap_sync_pte(pte, vma, address, flags);/*★*/
		address += PAGE_SIZE;
		pte++;
	} while (address && (address < end));

	pte_unmap(pte - 1);

	return error;
}

static inline int filemap_sync_pmd_range(pud_t * pud,
	unsigned long address, unsigned long end, 
	struct vm_area_struct *vma, unsigned int flags)
{
	pmd_t * pmd;
	int error;

	if (pud_none(*pud))
		return 0;
	if (pud_bad(*pud)) {
		pud_ERROR(*pud);
		pud_clear(pud);
		return 0;
	}
	pmd = pmd_offset(pud, address);
	if ((address & PUD_MASK) != (end & PUD_MASK))
		end = (address & PUD_MASK) + PUD_SIZE;
	error = 0;
	do {
		error |= filemap_sync_pte_range(pmd, address, end, vma, flags); /*★*/
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address && (address < end));
	return error;
}

static inline int filemap_sync_pud_range(pgd_t *pgd,
	unsigned long address, unsigned long end,
	struct vm_area_struct *vma, unsigned int flags)
{
	pud_t *pud;
	int error;

	if (pgd_none(*pgd))
		return 0;
	if (pgd_bad(*pgd)) {
		pgd_ERROR(*pgd);
		pgd_clear(pgd);
		return 0;
	}
	pud = pud_offset(pgd, address);
	if ((address & PGDIR_MASK) != (end & PGDIR_MASK))
		end = (address & PGDIR_MASK) + PGDIR_SIZE;
	error = 0;
	do {
		error |= filemap_sync_pmd_range(pud, address, end, vma, flags); /*★*/
		address = (address + PUD_SIZE) & PUD_MASK;
		pud++;
	} while (address && (address < end));
	return error;
}

static int __filemap_sync(struct vm_area_struct *vma, unsigned long address,
			size_t size, unsigned int flags)
{
	pgd_t *pgd;
	unsigned long end = address + size;
	unsigned long next;
	int i;
	int error = 0;

	/* Aquire the lock early; it may be possible to avoid dropping
	 * and reaquiring it repeatedly.
	 */
	spin_lock(&vma->vm_mm->page_table_lock);

	pgd = pgd_offset(vma->vm_mm, address);
	flush_cache_range(vma, address, end);

	/* For hugepages we can't go walking the page table normally,
	 * but that's ok, hugetlbfs is memory based, so we don't need
	 * to do anything more on an msync() */
	if (is_vm_hugetlb_page(vma))
		goto out;

	if (address >= end)
		BUG();
	for (i = pgd_index(address); i <= pgd_index(end-1); i++) {
		next = (address + PGDIR_SIZE) & PGDIR_MASK;
		if (next <= address || next > end)
			next = end;
		error |= filemap_sync_pud_range(pgd, address, next, vma, flags); /*★*/
		address = next;
		pgd++;
	}
	/*
	 * Why flush ? filemap_sync_pte already flushed the tlbs with the
	 * dirty bits.
	 */
	flush_tlb_range(vma, end - size, end);
 out:
	spin_unlock(&vma->vm_mm->page_table_lock);

	return error;
}

#ifdef CONFIG_PREEMPT
static int filemap_sync(struct vm_area_struct *vma, unsigned long address,
			size_t size, unsigned int flags)
{
	const size_t chunk = 64 * 1024;	/* bytes */
	int error = 0;

	while (size) {
		size_t sz = min(size, chunk);

		error |= __filemap_sync(vma, address, sz, flags);
		cond_resched();
		address += sz;
		size -= sz;
	}
	return error;
}
#else
static int filemap_sync(struct vm_area_struct *vma, unsigned long address,
			size_t size, unsigned int flags)
{
	return __filemap_sync(vma, address, size, flags);
}
#endif

/*
 * MS_SYNC syncs the entire file - including mappings.
 *
 * MS_ASYNC does not start I/O (it used to, up to 2.5.67).  Instead, it just
 * marks the relevant pages dirty.  The application may now run fsync() to
 * write out the dirty pages and wait on the writeout and check the result.
 * Or the application may run fadvise(FADV_DONTNEED) against the fd to start
 * async writeout immediately.
 * So my _not_ starting I/O in MS_ASYNC we provide complete flexibility to
 * applications.
 */
static int msync_interval(struct vm_area_struct * vma,
	unsigned long start, unsigned long end, int flags)
{
	/**
	 * 默认返回值为0.当不是共享映射时，就返回它。
	 */
	int ret = 0;
	struct file * file = vma->vm_file;

	if ((flags & MS_INVALIDATE) && (vma->vm_flags & VM_LOCKED))
		return -EBUSY;

	/**
	 * 只有线性区是文件映射并且是共享内存映射时，才进行处理。
	 */
	if (file && (vma->vm_flags & VM_SHARED)) {
		/**
		 * filemap_sync函数扫描包含在线性区中的线性地址区间所对应的页表项。
		 * 对于找到的每个页，重设对应页表项的Dirty标志，调用flush_tlb_page刷新相应的TLB。然后设置页描述符中的PG_dirty标志，将页标记为脏。
		 */
		ret = filemap_sync(vma, start, end-start, flags); /*★*/

        /*
         * 如果MS_ASYNC置位，它就返回。因此，MS_ASYNC标志的实际作用就是将线性区的页标志PG_dirty置位
         * 该系统调用并没有实际开始IO数据传输
         */

		/**
		 * 只有设置了MS_SYNC才继续进行处理。否则直接返回。
		 * MS_SYNC置位，函数必须将内存区的页刷新到磁盘，而且，当前进程必须睡眠一直到所有IO数据传输结束。
		 * 为做到这一点，函数要得到文件索引节点的信号量i_sem
		 */
		if (!ret && (flags & MS_SYNC)) {
			struct address_space *mapping = file->f_mapping;
			int err;

			/**
			 * 调用filemap_fdatawrite()函数，该函数必须接收的参数为文件的address_space对象的地址
			 * filemap_fdatawrite函数用WB_SYNC_ALL同步模式建立一个writeback_control描述符。
			 * 如果地址空间有内置的writepages方法就调用这个函数后函数。如果没有，就执行mpage_writepages函数。
			 */
			ret = filemap_fdatawrite(mapping); /*★*/
			if (file->f_op && file->f_op->fsync) {
				/*
				 * We don't take i_sem here because mmap_sem
				 * is already held.
				 */
				/**
				 * 如果定义了文件对象的fsync方法。如果定义了，就执行它。
				 * 对普通文件来说，这个方法限制自己把文件的索引节点对象刷新到磁盘。
				 * 对块设备文件，这个方法调用sync_blockdev，它会将该设备所有脏缓冲区的数据保存到磁盘中。
				 */
				err = file->f_op->fsync(file,file->f_dentry,1);
				if (err && !ret)
					ret = err;
			}
			/**
			 * 执行filemap_fdatawait函数，页高速缓存中的基树标识了所有通过PAGECHCHE_TAG_WRITEBACK标记正在往磁盘写的页。
			 * 函数快速地扫描覆盖给定线性地址空间的这一部分基树来寻找PG_writeback标志置位的页。并调用wait_on_page_bit等待每一页的PG_writeback标志清0.
			 * 也就是等到正在进行的该页的IO数据传输结束。
			 */
			err = filemap_fdatawait(mapping); PG_writeback
			if (!ret)
				ret = err;
		}
	}
	return ret;
}

/**
 * msync系统调用的实现。把属于共享内存映射的脏页刷新到磁盘。
 * 		start:	一个线性地址区间的起始地址。
 *		len:	区间的长度。
 *		flags:	标志。
 *			MS_SYNC-要求这个系统调用挂起进程，直到IO操作完成为止。这样，调用者可以认为当系统调用完成时，内存映射中的所有页都已经被刷新到磁盘。
 *			MS_ASYNC-要求系统调用立即返回而不用挂起调用进程。
 *			MS_INVALIDATE-要求系统调用使同一文件的其他内存映射无效。LINUX并没有真正实现它。
 */
asmlinkage long sys_msync(unsigned long start, size_t len, int flags)
{
	unsigned long end;
	struct vm_area_struct * vma;
	int unmapped_error, error = -EINVAL;

	if (flags & MS_SYNC)
		current->flags |= PF_SYNCWRITE;

	down_read(&current->mm->mmap_sem);
	if (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC))
		goto out;
	if (start & ~PAGE_MASK)
		goto out;
	if ((flags & MS_ASYNC) && (flags & MS_SYNC))
		goto out;
	error = -ENOMEM;
	len = (len + ~PAGE_MASK) & PAGE_MASK;
	end = start + len;
	if (end < start)
		goto out;
	error = 0;
	if (end == start)
		goto out;
	/*
	 * If the interval [start,end) covers some unmapped address ranges,
	 * just ignore them, but return -ENOMEM at the end.
	 */
	vma = find_vma(current->mm, start);
	unmapped_error = 0;
	/**
	 * 循环处理区域中的每一个线性区。
	 */
	for (;;) {
		/* Still start < end. */
		error = -ENOMEM;
		if (!vma)
			goto out;
		/* Here start < vma->vm_end. */
		if (start < vma->vm_start) {
			unmapped_error = -ENOMEM;
			start = vma->vm_start;
		}
		/* Here vma->vm_start <= start < vma->vm_end. */
		if (end <= vma->vm_end) {
			if (start < end) {
				error = msync_interval(vma, start, end, flags); /*★*/
				if (error)
					goto out;
			}
			error = unmapped_error;
			goto out;
		}
		/* Here vma->vm_start <= start < vma->vm_end < end. */
		/**
		 * 对每个线性区，调用msync_interval实现直接的刷新操作。
		 */
		error = msync_interval(vma, start, vma->vm_end, flags); /*★*/
		if (error)
			goto out;
		start = vma->vm_end;
		vma = vma->vm_next;
	}
out:
	up_read(&current->mm->mmap_sem);
	current->flags &= ~PF_SYNCWRITE;
	return error;
}
