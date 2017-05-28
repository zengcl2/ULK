/* Copyright (C) 2002 by James.Bottomley@HansenPartnership.com 
 *
 * Implements the generic device dma API via the existing pci_ one
 * for unconverted architectures
 */

#ifndef _ASM_GENERIC_DMA_MAPPING_H
#define _ASM_GENERIC_DMA_MAPPING_H

#include <linux/config.h>

#ifdef CONFIG_PCI

/* we implement the API below in terms of the existing PCI one,
 * so include it */
#include <linux/pci.h>
/* need struct page definitions */
#include <linux/mm.h>

static inline int
dma_supported(struct device *dev, u64 mask)
{
	BUG_ON(dev->bus != &pci_bus_type);

	return pci_dma_supported(to_pci_dev(dev), mask);
}

/**
 * 设置一个设备的DMA寻址范围。
 */
/*
 * 用于检查总线是否可以接受给定大小的总线地址，如果可以，则通知总线层给定的外围设备将使用该大小的总线地址
 */
static inline int
dma_set_mask(struct device *dev, u64 dma_mask)
{
	BUG_ON(dev->bus != &pci_bus_type);

	return pci_set_dma_mask(to_pci_dev(dev), dma_mask);
}

/*
 * 建立一致性映射。
 * 返回新缓冲区的线性地址和总线地址,在x86中，返回新缓冲区的线性地址和物理地址
 */
static inline void *
dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle,
		   int flag)
{
	BUG_ON(dev->bus != &pci_bus_type);

	return pci_alloc_consistent(to_pci_dev(dev), size, dma_handle);
}

/*
 * 释放映射和缓冲区
 */
static inline void
dma_free_coherent(struct device *dev, size_t size, void *cpu_addr,
		    dma_addr_t dma_handle)
{
	BUG_ON(dev->bus != &pci_bus_type);

	pci_free_consistent(to_pci_dev(dev), size, cpu_addr, dma_handle);
}

/**
 * 映射单个流式缓冲区。
 * 返回值是总线地址。
 */
static inline dma_addr_t
dma_map_single(struct device *dev, void *cpu_addr, size_t size,
	       enum dma_data_direction direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	return pci_map_single(to_pci_dev(dev), cpu_addr, size, (int)direction);
}

/**
 * 解除单个DMA流式映射。这个函数可能会处理回弹缓冲区。
 */
static inline void
dma_unmap_single(struct device *dev, dma_addr_t dma_addr, size_t size,
		 enum dma_data_direction direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	pci_unmap_single(to_pci_dev(dev), dma_addr, size, (int)direction);
}

/**
 * 将单页映射为一个流式DAM映射。
 * offset和size用于映射一页中的一部分。
 * 如果分配的页是缓存流水线的一部分，则映射部分页会引起一致性问题。
 */
static inline dma_addr_t
dma_map_page(struct device *dev, struct page *page,
	     unsigned long offset, size_t size,
	     enum dma_data_direction direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	return pci_map_page(to_pci_dev(dev), page, offset, size, (int)direction);
}

/**
 * 解除一个单页DMA映射。
 */
static inline void
dma_unmap_page(struct device *dev, dma_addr_t dma_address, size_t size,
	       enum dma_data_direction direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	pci_unmap_page(to_pci_dev(dev), dma_address, size, (int)direction);
}

static inline int
dma_map_sg(struct device *dev, struct scatterlist *sg, int nents,
	   enum dma_data_direction direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	return pci_map_sg(to_pci_dev(dev), sg, nents, (int)direction);
}

static inline void
dma_unmap_sg(struct device *dev, struct scatterlist *sg, int nhwentries,
	     enum dma_data_direction direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	pci_unmap_sg(to_pci_dev(dev), sg, nhwentries, (int)direction);
}

/**
 * 当驱动程序不经过撤销流式映射，而想访问DMA缓冲区中的内容时，使用本函数。
 * 这样CPU将暂时拥有该缓冲区。使相应的硬件高速缓存行无效
 */
static inline void
dma_sync_single_for_cpu(struct device *dev, dma_addr_t dma_handle, size_t size,
			enum dma_data_direction direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	pci_dma_sync_single_for_cpu(to_pci_dev(dev), dma_handle,
				    size, (int)direction);
}

/**
 * 将DMA流式缓冲区交还给设备。
 */
static inline void
dma_sync_single_for_device(struct device *dev, dma_addr_t dma_handle, size_t size,
			   enum dma_data_direction direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	pci_dma_sync_single_for_device(to_pci_dev(dev), dma_handle,
				       size, (int)direction);
}

static inline void
dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg, int nelems,
		    enum dma_data_direction direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	pci_dma_sync_sg_for_cpu(to_pci_dev(dev), sg, nelems, (int)direction);
}

static inline void
dma_sync_sg_for_device(struct device *dev, struct scatterlist *sg, int nelems,
		       enum dma_data_direction direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	pci_dma_sync_sg_for_device(to_pci_dev(dev), sg, nelems, (int)direction);
}

static inline int
dma_mapping_error(dma_addr_t dma_addr)
{
	return pci_dma_mapping_error(dma_addr);
}


#else

static inline int
dma_supported(struct device *dev, u64 mask)
{
	return 0;
}

static inline int
dma_set_mask(struct device *dev, u64 dma_mask)
{
	BUG();
	return 0;
}

static inline void *
dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle,
		   int flag)
{
	BUG();
	return NULL;
}

static inline void
dma_free_coherent(struct device *dev, size_t size, void *cpu_addr,
		    dma_addr_t dma_handle)
{
	BUG();
}

static inline dma_addr_t
dma_map_single(struct device *dev, void *cpu_addr, size_t size,
	       enum dma_data_direction direction)
{
	BUG();
	return 0;
}

static inline void
dma_unmap_single(struct device *dev, dma_addr_t dma_addr, size_t size,
		 enum dma_data_direction direction)
{
	BUG();
}

static inline dma_addr_t
dma_map_page(struct device *dev, struct page *page,
	     unsigned long offset, size_t size,
	     enum dma_data_direction direction)
{
	BUG();
	return 0;
}

static inline void
dma_unmap_page(struct device *dev, dma_addr_t dma_address, size_t size,
	       enum dma_data_direction direction)
{
	BUG();
}

static inline int
dma_map_sg(struct device *dev, struct scatterlist *sg, int nents,
	   enum dma_data_direction direction)
{
	BUG();
	return 0;
}

static inline void
dma_unmap_sg(struct device *dev, struct scatterlist *sg, int nhwentries,
	     enum dma_data_direction direction)
{
	BUG();
}

static inline void
dma_sync_single_for_cpu(struct device *dev, dma_addr_t dma_handle, size_t size,
			enum dma_data_direction direction)
{
	BUG();
}

static inline void
dma_sync_single_for_device(struct device *dev, dma_addr_t dma_handle, size_t size,
			   enum dma_data_direction direction)
{
	BUG();
}

static inline void
dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg, int nelems,
		    enum dma_data_direction direction)
{
	BUG();
}

static inline void
dma_sync_sg_for_device(struct device *dev, struct scatterlist *sg, int nelems,
		       enum dma_data_direction direction)
{
	BUG();
}

static inline int
dma_error(dma_addr_t dma_addr)
{
	return 0;
}

#endif

/* Now for the API extensions over the pci_ one */

#define dma_alloc_noncoherent(d, s, h, f) dma_alloc_coherent(d, s, h, f)
#define dma_free_noncoherent(d, s, v, h) dma_free_coherent(d, s, v, h)
#define dma_is_consistent(d)	(1)

static inline int
dma_get_cache_alignment(void)
{
	/* no easy way to get cache size on all processors, so return
	 * the maximum possible, to be safe */
	return (1 << L1_CACHE_SHIFT_MAX);
}

static inline void
dma_sync_single_range_for_cpu(struct device *dev, dma_addr_t dma_handle,
			      unsigned long offset, size_t size,
			      enum dma_data_direction direction)
{
	/* just sync everything, that's all the pci API can do */
	dma_sync_single_for_cpu(dev, dma_handle, offset+size, direction);
}

static inline void
dma_sync_single_range_for_device(struct device *dev, dma_addr_t dma_handle,
				 unsigned long offset, size_t size,
				 enum dma_data_direction direction)
{
	/* just sync everything, that's all the pci API can do */
	dma_sync_single_for_device(dev, dma_handle, offset+size, direction);
}

static inline void
dma_cache_sync(void *vaddr, size_t size,
	       enum dma_data_direction direction)
{
	/* could define this in terms of the dma_cache ... operations,
	 * but if you get this on a platform, you should convert the platform
	 * to using the generic device DMA API */
	BUG();
}

#endif

