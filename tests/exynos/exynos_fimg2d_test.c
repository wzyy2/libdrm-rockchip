/*
 * Copyright (C) 2013 Samsung Electronics Co.Ltd
 * Authors:
 *	Inki Dae <inki.dae@samsung.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <linux/stddef.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libkms.h>
#include <drm_fourcc.h>

#include "exynos_drm.h"
#include "exynos_drmif.h"
#include "exynos_fimg2d.h"

#define DRM_MODULE_NAME		"exynos"
#define MAX_TEST_CASE		8

static unsigned int screen_width, screen_height;

/*
 * A structure to test fimg2d hw.
 *
 * @solid_fill: fill given color data to source buffer.
 * @copy: copy source to destination buffer.
 * @copy_with_scale: copy source to destination buffer scaling up or
 *	down properly.
 * @blend: blend source to destination buffer.
 */
struct fimg2d_test_case {
	int (*solid_fill)(struct exynos_device *dev, struct exynos_bo *dst);
	int (*copy)(struct exynos_device *dev, struct exynos_bo *src,
			struct exynos_bo *dst, enum e_g2d_buf_type);
	int (*copy_with_scale)(struct exynos_device *dev,
				struct exynos_bo *src, struct exynos_bo *dst,
				enum e_g2d_buf_type);
	int (*blend)(struct exynos_device *dev,
				struct exynos_bo *src, struct exynos_bo *dst,
				enum e_g2d_buf_type);
	int (*checkerboard)(struct exynos_device *dev,
				struct exynos_bo *src, struct exynos_bo *dst,
				enum e_g2d_buf_type);
};

struct connector {
	uint32_t id;
	char mode_str[64];
	char format_str[5];
	unsigned int fourcc;
	drmModeModeInfo *mode;
	drmModeEncoder *encoder;
	int crtc;
	int pipe;
	int plane_zpos;
	unsigned int fb_id[2], current_fb_id;
	struct timeval start;

	int swap_count;
};

static void connector_find_mode(int fd, struct connector *c,
				drmModeRes *resources)
{
	drmModeConnector *connector;
	int i, j;

	/* First, find the connector & mode */
	c->mode = NULL;
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(fd, resources->connectors[i]);

		if (!connector) {
			fprintf(stderr, "could not get connector %i: %s\n",
				resources->connectors[i], strerror(errno));
			drmModeFreeConnector(connector);
			continue;
		}

		if (!connector->count_modes) {
			drmModeFreeConnector(connector);
			continue;
		}

		if (connector->connector_id != c->id) {
			drmModeFreeConnector(connector);
			continue;
		}

		for (j = 0; j < connector->count_modes; j++) {
			c->mode = &connector->modes[j];
			if (!strcmp(c->mode->name, c->mode_str))
				break;
		}

		/* Found it, break out */
		if (c->mode)
			break;

		drmModeFreeConnector(connector);
	}

	if (!c->mode) {
		fprintf(stderr, "failed to find mode \"%s\"\n", c->mode_str);
		return;
	}

	/* Now get the encoder */
	for (i = 0; i < resources->count_encoders; i++) {
		c->encoder = drmModeGetEncoder(fd, resources->encoders[i]);

		if (!c->encoder) {
			fprintf(stderr, "could not get encoder %i: %s\n",
				resources->encoders[i], strerror(errno));
			drmModeFreeEncoder(c->encoder);
			continue;
		}

		if (c->encoder->encoder_id  == connector->encoder_id)
			break;

		drmModeFreeEncoder(c->encoder);
	}

	if (c->crtc == -1)
		c->crtc = c->encoder->crtc_id;
}

static int connector_find_plane(int fd, unsigned int *plane_id)
{
	drmModePlaneRes *plane_resources;
	drmModePlane *ovr;
	int i;

	plane_resources = drmModeGetPlaneResources(fd);
	if (!plane_resources) {
		fprintf(stderr, "drmModeGetPlaneResources failed: %s\n",
			strerror(errno));
		return -1;
	}

	for (i = 0; i < plane_resources->count_planes; i++) {
		plane_id[i] = 0;

		ovr = drmModeGetPlane(fd, plane_resources->planes[i]);
		if (!ovr) {
			fprintf(stderr, "drmModeGetPlane failed: %s\n",
				strerror(errno));
			continue;
		}

		if (ovr->possible_crtcs & (1 << 0))
			plane_id[i] = ovr->plane_id;
		drmModeFreePlane(ovr);
	}

	return 0;
}

static int drm_set_crtc(struct exynos_device *dev, struct connector *c,
			unsigned int fb_id)
{
	int ret;

	ret = drmModeSetCrtc(dev->fd, c->crtc,
			fb_id, 0, 0, &c->id, 1, c->mode);
	if (ret) {
		drmMsg("failed to set mode: %s\n", strerror(errno));
		goto err;
	}

	return 0;

err:
	return ret;
}

static struct exynos_bo *exynos_create_buffer(struct exynos_device *dev,
						unsigned long size,
						unsigned int flags)
{
	struct exynos_bo *bo;

	bo = exynos_bo_create(dev, size, flags);
	if (!bo)
		return bo;

	if (!exynos_bo_map(bo)) {
		exynos_bo_destroy(bo);
		return NULL;
	}

	return bo;
}

/* Allocate buffer and fill it with checkerboard pattern, where the tiles *
 * have a random color. The caller has to free the buffer.                */
static void *create_checkerboard_pattern(unsigned int num_tiles_x,
						unsigned int num_tiles_y, unsigned int tile_size)
{
	unsigned int *buf;
	unsigned int x, y, i, j;
	const unsigned int stride = num_tiles_x * tile_size;

	if (posix_memalign((void*)&buf, 64, num_tiles_y * tile_size * stride * 4) != 0)
		return NULL;

	for (x = 0; x < num_tiles_x; ++x) {
		for (y = 0; y < num_tiles_y; ++y) {
			const unsigned int color = 0xff000000 + (random() & 0xffffff);

			for (i = 0; i < tile_size; ++i) {
				for (j = 0; j < tile_size; ++j) {
					buf[x * tile_size + y * stride * tile_size + i + j * stride] = color;
				}
			}
		}
	}

	return buf;
}

static void exynos_destroy_buffer(struct exynos_bo *bo)
{
	exynos_bo_destroy(bo);
}

static void wait_for_user_input(int last)
{
	printf("press <ENTER> to %s\n", last ? "exit test application" :
			"skip to next test");

	getchar();
}

static int g2d_solid_fill_test(struct exynos_device *dev, struct exynos_bo *dst)
{
	struct g2d_context *ctx;
	struct g2d_image img = {0};
	unsigned int count, img_w, img_h;
	int ret = 0;

	ctx = g2d_init(dev->fd);
	if (!ctx)
		return -EFAULT;

	img.bo[0] = dst->handle;

	printf("solid fill test.\n");

	srand(time(NULL));
	img_w = screen_width;
	img_h = screen_height;

	for (count = 0; count < 2; count++) {
		unsigned int x, y, w, h;

		x = rand() % (img_w / 2);
		y = rand() % (img_h / 2);
		w = rand() % (img_w - x);
		h = rand() % (img_h - y);

		img.width = img_w;
		img.height = img_h;
		img.stride = img.width * 4;
		img.color_mode = G2D_COLOR_FMT_ARGB8888 | G2D_ORDER_AXRGB;
		img.color = 0xff000000 + (random() & 0xffffff);

		ret = g2d_solid_fill(ctx, &img, x, y, w, h);
		if (ret < 0)
			goto err_fini;

		ret = g2d_exec(ctx);
		if (ret < 0)
			break;
	}

err_fini:
	g2d_fini(ctx);

	return ret;
}

static int g2d_copy_test(struct exynos_device *dev, struct exynos_bo *src,
				struct exynos_bo *dst,
				enum e_g2d_buf_type type)
{
	struct g2d_context *ctx;
	struct g2d_image src_img = {0}, dst_img = {0};
	unsigned int src_x, src_y, dst_x, dst_y, img_w, img_h;
	unsigned long userptr, size;
	int ret;

	ctx = g2d_init(dev->fd);
	if (!ctx)
		return -EFAULT;

	dst_img.bo[0] = dst->handle;

	src_x = 0;
	src_y = 0;
	dst_x = 0;
	dst_y = 0;
	img_w = screen_width;
	img_h = screen_height;

	switch (type) {
	case G2D_IMGBUF_GEM:
		src_img.bo[0] = src->handle;
		break;
	case G2D_IMGBUF_USERPTR:
		size = img_w * img_h * 4;

		userptr = (unsigned long)malloc(size);
		if (!userptr) {
			fprintf(stderr, "failed to allocate userptr.\n");
			return -EFAULT;
		}

		src_img.user_ptr[0].userptr = userptr;
		src_img.user_ptr[0].size = size;
		break;
	default:
		type = G2D_IMGBUF_GEM;
		break;
	}

	printf("copy test with %s.\n",
			type == G2D_IMGBUF_GEM ? "gem" : "userptr");

	src_img.width = img_w;
	src_img.height = img_h;
	src_img.stride = src_img.width * 4;
	src_img.buf_type = type;
	src_img.color_mode = G2D_COLOR_FMT_ARGB8888 | G2D_ORDER_AXRGB;
	src_img.color = 0xffff0000;
	ret = g2d_solid_fill(ctx, &src_img, src_x, src_y, img_w, img_h);
	if (ret < 0)
		goto err_free_userptr;

	dst_img.width = img_w;
	dst_img.height = img_h;
	dst_img.stride = dst_img.width * 4;
	dst_img.buf_type = G2D_IMGBUF_GEM;
	dst_img.color_mode = G2D_COLOR_FMT_ARGB8888 | G2D_ORDER_AXRGB;

	ret = g2d_copy(ctx, &src_img, &dst_img, src_x, src_y, dst_x, dst_y,
			img_w - 4, img_h - 4);
	if (ret < 0)
		goto err_free_userptr;

	g2d_exec(ctx);

err_free_userptr:
	if (type == G2D_IMGBUF_USERPTR)
		if (userptr)
			free((void *)userptr);

	g2d_fini(ctx);

	return ret;
}

static int g2d_copy_with_scale_test(struct exynos_device *dev,
					struct exynos_bo *src,
					struct exynos_bo *dst,
					enum e_g2d_buf_type type)
{
	struct g2d_context *ctx;
	struct g2d_image src_img = {0}, dst_img = {0};
	unsigned int src_x, src_y, img_w, img_h;
	unsigned long userptr, size;
	int ret;

	ctx = g2d_init(dev->fd);
	if (!ctx)
		return -EFAULT;

	dst_img.bo[0] = dst->handle;

	src_x = 0;
	src_y = 0;
	img_w = screen_width;
	img_h = screen_height;

	switch (type) {
	case G2D_IMGBUF_GEM:
		src_img.bo[0] = src->handle;
		break;
	case G2D_IMGBUF_USERPTR:
		size = img_w * img_h * 4;

		userptr = (unsigned long)malloc(size);
		if (!userptr) {
			fprintf(stderr, "failed to allocate userptr.\n");
			return -EFAULT;
		}

		src_img.user_ptr[0].userptr = userptr;
		src_img.user_ptr[0].size = size;
		break;
	default:
		type = G2D_IMGBUF_GEM;
		break;
	}

	printf("copy and scale test with %s.\n",
			type == G2D_IMGBUF_GEM ? "gem" : "userptr");

	src_img.width = img_w;
	src_img.height = img_h;
	src_img.stride = src_img.width * 4;
	src_img.buf_type = type;
	src_img.color_mode = G2D_COLOR_FMT_ARGB8888 | G2D_ORDER_AXRGB;
	src_img.color = 0xffffffff;
	ret = g2d_solid_fill(ctx, &src_img, src_x, src_y, img_w ,  img_h);
	if (ret < 0)
		goto err_free_userptr;

	src_img.color = 0xff00ff00;
	ret = g2d_solid_fill(ctx, &src_img, 5, 5, 100, 100);
	if (ret < 0)
		goto err_free_userptr;

	dst_img.width = img_w;
	dst_img.height = img_h;
	dst_img.buf_type = G2D_IMGBUF_GEM;
	dst_img.stride = dst_img.width * 4;
	dst_img.color_mode = G2D_COLOR_FMT_ARGB8888 | G2D_ORDER_AXRGB;

	ret = g2d_copy_with_scale(ctx, &src_img, &dst_img, 5, 5, 100, 100,
					100, 100, 200, 200, 0);
	if (ret < 0)
		goto err_free_userptr;

	g2d_exec(ctx);

err_free_userptr:
	if (type == G2D_IMGBUF_USERPTR)
		if (userptr)
			free((void *)userptr);

	g2d_fini(ctx);

	return 0;
}

static int g2d_blend_test(struct exynos_device *dev,
					struct exynos_bo *src,
					struct exynos_bo *dst,
					enum e_g2d_buf_type type)
{
	struct g2d_context *ctx;
	struct g2d_image src_img = {0}, dst_img = {0};
	unsigned int src_x, src_y, dst_x, dst_y, img_w, img_h;
	unsigned long userptr, size;
	int ret;

	ctx = g2d_init(dev->fd);
	if (!ctx)
		return -EFAULT;

	dst_img.bo[0] = dst->handle;

	src_x = 0;
	src_y = 0;
	dst_x = 0;
	dst_y = 0;
	img_w = screen_width;
	img_h = screen_height;

	switch (type) {
	case G2D_IMGBUF_GEM:
		src_img.bo[0] = src->handle;
		break;
	case G2D_IMGBUF_USERPTR:
		size = img_w * img_h * 4;

		userptr = (unsigned long)malloc(size);
		if (!userptr) {
			fprintf(stderr, "failed to allocate userptr.\n");
			return -EFAULT;
		}

		src_img.user_ptr[0].userptr = userptr;
		src_img.user_ptr[0].size = size;
		break;
	default:
		type = G2D_IMGBUF_GEM;
		break;
	}

	printf("blend test with %s.\n",
			type == G2D_IMGBUF_GEM ? "gem" : "userptr");

	src_img.width = img_w;
	src_img.height = img_h;
	src_img.stride = src_img.width * 4;
	src_img.buf_type = type;
	src_img.select_mode = G2D_SELECT_MODE_NORMAL;
	src_img.color_mode = G2D_COLOR_FMT_ARGB8888 | G2D_ORDER_AXRGB;
	src_img.color = 0xffffffff;
	ret = g2d_solid_fill(ctx, &src_img, src_x, src_y, img_w, img_h);
	if (ret < 0)
		goto err_free_userptr;

	src_img.color = 0x770000ff;
	ret = g2d_solid_fill(ctx, &src_img, 5, 5, 200, 200);
	if (ret < 0)
		goto err_free_userptr;

	dst_img.width = img_w;
	dst_img.height = img_h;
	dst_img.stride = dst_img.width * 4;
	dst_img.buf_type = G2D_IMGBUF_GEM;
	dst_img.select_mode = G2D_SELECT_MODE_NORMAL;
	dst_img.color_mode = G2D_COLOR_FMT_ARGB8888 | G2D_ORDER_AXRGB;
	dst_img.color = 0xffffffff;
	ret = g2d_solid_fill(ctx, &dst_img, dst_x, dst_y, img_w, img_h);
	if (ret < 0)
		goto err_free_userptr;

	dst_img.color = 0x77ff0000;
	ret = g2d_solid_fill(ctx, &dst_img, 105, 105, 200, 200);
	if (ret < 0)
		goto err_free_userptr;

	ret = g2d_blend(ctx, &src_img, &dst_img, 5, 5, 105, 105, 200, 200,
			G2D_OP_OVER);
	if (ret < 0)
		goto err_free_userptr;

	g2d_exec(ctx);

err_free_userptr:
	if (type == G2D_IMGBUF_USERPTR)
		if (userptr)
			free((void *)userptr);

	g2d_fini(ctx);

	return 0;
}

static int g2d_checkerboard_test(struct exynos_device *dev,
					struct exynos_bo *src,
					struct exynos_bo *dst,
					enum e_g2d_buf_type type)
{
	struct g2d_context *ctx;
	struct g2d_image src_img = {0}, dst_img = {0};
	unsigned int src_x, src_y, dst_x, dst_y, img_w, img_h;
	void *checkerboard = NULL;
	int ret;

	ctx = g2d_init(dev->fd);
	if (!ctx)
		return -EFAULT;

	dst_img.bo[0] = dst->handle;

	src_x = 0;
	src_y = 0;
	dst_x = 0;
	dst_y = 0;

	checkerboard = create_checkerboard_pattern(screen_width / 32, screen_height / 32, 32);
	if (checkerboard == NULL) {
		ret = -1;
		goto fail;
	}

	img_w = screen_width - (screen_width % 32);
	img_h = screen_height - (screen_height % 32);

	switch (type) {
	case G2D_IMGBUF_GEM:
		memcpy(src->vaddr, checkerboard, img_w * img_h * 4);
		src_img.bo[0] = src->handle;
		break;
	case G2D_IMGBUF_USERPTR:
		src_img.user_ptr[0].userptr = (unsigned long)checkerboard;
		src_img.user_ptr[0].size = img_w * img_h * 4;
		break;
	default:
		ret = -EFAULT;
		goto fail;
	}

	printf("checkerboard test with %s.\n",
			type == G2D_IMGBUF_GEM ? "gem" : "userptr");

	src_img.width = img_w;
	src_img.height = img_h;
	src_img.stride = src_img.width * 4;
	src_img.buf_type = type;
	src_img.color_mode = G2D_COLOR_FMT_ARGB8888 | G2D_ORDER_AXRGB;

	dst_img.width = screen_width;
	dst_img.height = screen_height;
	dst_img.stride = dst_img.width * 4;
	dst_img.buf_type = G2D_IMGBUF_GEM;
	dst_img.color_mode = G2D_COLOR_FMT_ARGB8888 | G2D_ORDER_AXRGB;
	src_img.color = 0xff000000;
	ret = g2d_solid_fill(ctx, &dst_img, src_x, src_y, screen_width, screen_height);
	if (ret < 0)
		goto fail;

	ret = g2d_copy(ctx, &src_img, &dst_img, src_x, src_y, dst_x, dst_y,
			img_w, img_h);
	if (ret < 0)
		goto fail;

	g2d_exec(ctx);

fail:
	free(checkerboard);
	g2d_fini(ctx);

	return ret;
}

static struct fimg2d_test_case test_case = {
	.solid_fill = &g2d_solid_fill_test,
	.copy = &g2d_copy_test,
	.copy_with_scale = &g2d_copy_with_scale_test,
	.blend = &g2d_blend_test,
	.checkerboard = &g2d_checkerboard_test,
};

static void usage(char *name)
{
	fprintf(stderr, "usage: %s [-s]\n", name);
	fprintf(stderr, "-s <connector_id>@<crtc_id>:<mode>\n");
	exit(0);
}

extern char *optarg;
static const char optstr[] = "s:";

int main(int argc, char **argv)
{
	struct exynos_device *dev;
	struct exynos_bo *bo, *src;
	struct connector con;
	unsigned int fb_id;
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
	drmModeRes *resources;
	int ret, fd, c;

	memset(&con, 0, sizeof(struct connector));

	if (argc != 3) {
		usage(argv[0]);
		return -EINVAL;
	}

	while ((c = getopt(argc, argv, optstr)) != -1) {
		switch (c) {
		case 's':
			con.crtc = -1;
			if (sscanf(optarg, "%d:0x%64s",
						&con.id,
						con.mode_str) != 2 &&
					sscanf(optarg, "%d@%d:%64s",
						&con.id,
						&con.crtc,
						con.mode_str) != 3)
				usage(argv[0]);
			break;
		default:
			usage(argv[0]);
			return -EINVAL;
		}
	}

	fd = drmOpen(DRM_MODULE_NAME, NULL);
	if (fd < 0) {
		fprintf(stderr, "failed to open.\n");
		return fd;
	}

	dev = exynos_device_create(fd);
	if (!dev) {
		drmClose(dev->fd);
		return -EFAULT;
	}

	resources = drmModeGetResources(dev->fd);
	if (!resources) {
		fprintf(stderr, "drmModeGetResources failed: %s\n",
				strerror(errno));
		ret = -EFAULT;
		goto err_drm_close;
	}

	connector_find_mode(dev->fd, &con, resources);
	drmModeFreeResources(resources);

	if (!con.mode) {
		fprintf(stderr, "failed to find usable connector\n");
		ret = -EFAULT;
		goto err_drm_close;
	}

	screen_width = con.mode->hdisplay;
	screen_height = con.mode->vdisplay;

	if (screen_width == 0 || screen_height == 0) {
		fprintf(stderr, "failed to find sane resolution on connector\n");
		ret = -EFAULT;
		goto err_drm_close;
	}

	printf("screen width = %d, screen height = %d\n", screen_width,
			screen_height);

	bo = exynos_create_buffer(dev, screen_width * screen_height * 4, 0);
	if (!bo) {
		ret = -EFAULT;
		goto err_drm_close;
	}

	handles[0] = bo->handle;
	pitches[0] = screen_width * 4;
	offsets[0] = 0;

	ret = drmModeAddFB2(dev->fd, screen_width, screen_height,
				DRM_FORMAT_RGBA8888, handles,
				pitches, offsets, &fb_id, 0);
	if (ret < 0)
		goto err_destroy_buffer;

	con.plane_zpos = -1;

	memset(bo->vaddr, 0xff, screen_width * screen_height * 4);

	ret = drm_set_crtc(dev, &con, fb_id);
	if (ret < 0)
		goto err_rm_fb;

	ret = test_case.solid_fill(dev, bo);
	if (ret < 0) {
		fprintf(stderr, "failed to solid fill operation.\n");
		goto err_rm_fb;
	}

	wait_for_user_input(0);

	src = exynos_create_buffer(dev, screen_width * screen_height * 4, 0);
	if (!src) {
		ret = -EFAULT;
		goto err_rm_fb;
	}

	ret = test_case.copy(dev, src, bo, G2D_IMGBUF_GEM);
	if (ret < 0) {
		fprintf(stderr, "failed to test copy operation.\n");
		goto err_free_src;
	}

	wait_for_user_input(0);

	ret = test_case.copy_with_scale(dev, src, bo, G2D_IMGBUF_GEM);
	if (ret < 0) {
		fprintf(stderr, "failed to test copy and scale operation.\n");
		goto err_free_src;
	}

	wait_for_user_input(0);

	ret = test_case.checkerboard(dev, src, bo, G2D_IMGBUF_GEM);
	if (ret < 0) {
		fprintf(stderr, "failed to issue checkerboard test.\n");
		goto err_free_src;
	}

	wait_for_user_input(1);

	/*
	 * The blend test uses the userptr functionality of exynos-drm, which
	 * is currently not safe to use. If the kernel hasn't been build with
	 * exynos-iommu support, then the blend test is going to produce (kernel)
	 * memory corruption, eventually leading to a system crash.
	 *
	 * Disable the test for now, until the kernel code has been sanitized.
	 */
#if 0
	ret  = test_case.blend(dev, src, bo, G2D_IMGBUF_USERPTR);
	if (ret < 0)
		fprintf(stderr, "failed to test blend operation.\n");

	getchar();
#endif

err_free_src:
	if (src)
		exynos_destroy_buffer(src);

err_rm_fb:
	drmModeRmFB(dev->fd, fb_id);

err_destroy_buffer:
	exynos_destroy_buffer(bo);

err_drm_close:
	drmClose(dev->fd);
	exynos_device_destroy(dev);

	return 0;
}
