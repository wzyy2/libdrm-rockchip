/*
 * Copyright (c) 2015 MediaTek Inc.
 * Author:JB TSAI <jb.tsai@mediatek.com>
 *
 * based on rockchip_drm.c
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <sys/mman.h>
#include <linux/stddef.h>

#include <xf86drm.h>

#include "mediatek_drm.h"
#include "mediatek_drmif.h"

/*
 * Create mediatek drm device object.
 *
 * @fd: file descriptor to mediatek drm driver opened.
 *
 * if true, return the device object else NULL.
 */
struct mediatek_device *mediatek_device_create(int fd)
{
	struct mediatek_device *dev;

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		fprintf(stderr, "failed to create device[%s].\n",
				strerror(errno));
		return NULL;
	}

	dev->fd = fd;

	return dev;
}

/*
 * Destroy mediatek drm device object
 *
 * @dev: mediatek drm device object.
 */
void mediatek_device_destroy(struct mediatek_device *dev)
{
	free(dev);
}

/*
 * Create a mediatek buffer object to mediatek drm device.
 *
 * @dev: mediatek drm device object.
 * @size: user-desired size.
 * flags: user-desired memory type.
 *	user can set one or more types among several types to memory
 *	allocation and cache attribute types. and as default,
 *	MEDIATEK_BO_NONCONTIG and MEDIATEK-BO_NONCACHABLE types would
 *	be used.
 *
 * if true, return a mediatek buffer object else NULL.
 */
struct mediatek_bo *mediatek_bo_create(struct mediatek_device *dev,
					size_t size, uint32_t flags)
{
	struct mediatek_bo *bo;
	struct drm_mtk_gem_create req = {
		.size = size,
		.flags = flags,
	};

	if (size == 0) {
		fprintf(stderr, "invalid size.\n");
		return NULL;
	}

	bo = calloc(1, sizeof(*bo));
	if (!bo) {
		fprintf(stderr, "failed to create bo[%s].\n",
				strerror(errno));
		goto fail;
	}

	bo->dev = dev;

	if (drmIoctl(dev->fd, DRM_IOCTL_MTK_GEM_CREATE, &req)){
		fprintf(stderr, "failed to create gem object[%s].\n",
				strerror(errno));
		goto err_free_bo;
	}

	bo->handle = req.handle;
	bo->size = size;
	bo->flags = flags;

	return bo;

err_free_bo:
	free(bo);
fail:
	return NULL;
}

/*
 * Destroy a mediatek buffer object.
 *
 * @bo: a mediatek buffer object to be destroyed.
 */
void mediatek_bo_destroy(struct mediatek_bo *bo)
{
	if (!bo)
		return;

	if (bo->vaddr)
		munmap(bo->vaddr, bo->size);

	if (bo->handle) {
		struct drm_gem_close req = {
			.handle = bo->handle,
		};

		drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_CLOSE, &req);
	}

	free(bo);
}


/*
 * Get a mediatek buffer object from a gem global object name.
 *
 * @dev: a mediatek device object.
 * @name: a gem global object name exported by another process.
 *
 * this interface is used to get a mediatek buffer object from a gem
 * global object name sent by another process for buffer sharing.
 *
 * if true, return a mediatek buffer object else NULL.
 *
 */
struct mediatek_bo *mediatek_bo_from_name(struct mediatek_device *dev,
						uint32_t name)
{
	struct mediatek_bo *bo;
	struct drm_gem_open req = {
		.name = name,
	};

	bo = calloc(1, sizeof(*bo));
	if (!bo) {
		fprintf(stderr, "failed to allocate bo[%s].\n",
				strerror(errno));
		return NULL;
	}

	if (drmIoctl(dev->fd, DRM_IOCTL_GEM_OPEN, &req)) {
		fprintf(stderr, "failed to open gem object[%s].\n",
				strerror(errno));
		goto err_free_bo;
	}

	bo->dev = dev;
	bo->name = name;
	bo->handle = req.handle;

	return bo;

err_free_bo:
	free(bo);
	return NULL;
}

/*
 * Get a gem global object name from a gem object handle.
 *
 * @bo: a mediatek buffer object including gem handle.
 * @name: a gem global object name to be got by kernel driver.
 *
 * this interface is used to get a gem global object name from a gem object
 * handle to a buffer that wants to share it with another process.
 *
 * if true, return 0 else negative.
 */
int mediatek_bo_get_name(struct mediatek_bo *bo, uint32_t *name)
{
	if (!bo->name) {
		struct drm_gem_flink req = {
			.handle = bo->handle,
		};
		int ret;

		ret = drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_FLINK, &req);
		if (ret) {
			fprintf(stderr, "failed to get gem global name[%s].\n",
					strerror(errno));
			return ret;
		}

		bo->name = req.name;
	}

	*name = bo->name;

	return 0;
}

uint32_t mediatek_bo_handle(struct mediatek_bo *bo)
{
	return bo->handle;
}

/*
 * Mmap a buffer to user space.
 *
 * @bo: a mediatek buffer object including a gem object handle to be mmapped
 *	to user space.
 *
 * if true, user pointer mmaped else NULL.
 */
void *mediatek_bo_map(struct mediatek_bo *bo)
{
	if (!bo->vaddr) {
		struct mediatek_device *dev = bo->dev;
		struct drm_mtk_gem_map_off req = {
			.handle = bo->handle,
		};
		int ret;

		ret = drmIoctl(dev->fd, DRM_IOCTL_MTK_GEM_MAP_OFFSET, &req);
		if (ret) {
			fprintf(stderr, "failed to ioctl gem map offset[%s].\n",
				strerror(errno));
			return NULL;
		}

		bo->vaddr = mmap(0, bo->size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, dev->fd, req.offset);
		if (bo->vaddr == MAP_FAILED) {
			fprintf(stderr, "failed to mmap buffer[%s].\n",
				strerror(errno));
			return NULL;
		}
	}

	return bo->vaddr;
}
