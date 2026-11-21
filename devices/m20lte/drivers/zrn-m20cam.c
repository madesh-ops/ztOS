// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-m20cam: the Galaxy M20 camera bring-up's frame memory.
 *
 * The receivers (CSIS v4.0, 0x14400000-0x14430000) write frames by DMA into
 * physically contiguous memory, which only the kernel can hand out. This
 * module does only that: one DMA-coherent buffer below 4 GiB, its bus
 * address in /sys/kernel/debug/zrn-m20cam/phys, and the buffer itself through
 * mmap of /dev/zrn-m20cam. Everything else of the bring-up (power, clocks,
 * PHY, receiver, its DMA) is programmed from user space
 * (devices/m20lte/camera/) until it is known to work.
 *
 *   modprobe zrn-m20cam [size_mb=24]
 *   cat /sys/kernel/debug/zrn-m20cam/phys
 *
 * The buffer is filled with 0xa5 at load, so a frame that lands shows.
 * Nothing here touches the camera hardware.
 */
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/string.h>

static unsigned int size_mb = 24;
module_param(size_mb, uint, 0444);
MODULE_PARM_DESC(size_mb, "frame buffer size in MiB (default 24: two 13 MP RAW10 frames at 16 bits a pixel)");

static struct platform_device *pdev;
static void *cpu;
static dma_addr_t bus;
static size_t size;
static struct dentry *dir;
static u64 phys_shown, size_shown;

static int m20cam_mmap(struct file *f, struct vm_area_struct *vma)
{
	size_t len = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff || len > size)
		return -EINVAL;
	return dma_mmap_coherent(&pdev->dev, vma, cpu, bus, len);
}

static const struct file_operations m20cam_fops = {
	.owner = THIS_MODULE,
	.mmap = m20cam_mmap,
};

static struct miscdevice m20cam_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "zrn-m20cam",
	.fops = &m20cam_fops,
	.mode = 0600,
};

static int __init m20cam_init(void)
{
	int ret;

	size = (size_t)size_mb << 20;
	pdev = platform_device_register_simple("zrn-m20cam", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_dev;
	cpu = dma_alloc_coherent(&pdev->dev, size, &bus, GFP_KERNEL);
	if (!cpu) {
		pr_err("zrn-m20cam: no %u MiB of contiguous memory below 4 GiB\n", size_mb);
		ret = -ENOMEM;
		goto err_dev;
	}
	memset(cpu, 0xa5, size);
	ret = misc_register(&m20cam_misc);
	if (ret)
		goto err_mem;
	phys_shown = bus;
	size_shown = size;
	dir = debugfs_create_dir("zrn-m20cam", NULL);
	debugfs_create_x64("phys", 0400, dir, &phys_shown);
	debugfs_create_u64("size", 0400, dir, &size_shown);
	pr_info("zrn-m20cam: %u MiB at bus address %pad\n", size_mb, &bus);
	return 0;

err_mem:
	dma_free_coherent(&pdev->dev, size, cpu, bus);
err_dev:
	platform_device_unregister(pdev);
	return ret;
}

static void __exit m20cam_exit(void)
{
	debugfs_remove_recursive(dir);
	misc_deregister(&m20cam_misc);
	dma_free_coherent(&pdev->dev, size, cpu, bus);
	platform_device_unregister(pdev);
}

module_init(m20cam_init);
module_exit(m20cam_exit);
MODULE_DESCRIPTION("Galaxy M20 camera bring-up: frame memory for the CSIS DMA");
MODULE_LICENSE("GPL");
