/*
 * Copyright (C) 2013 Samsung Electronics Co.Ltd
 * Authors:
 *	Yakir Yang <ykk@rock-chips.com>
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

#include "rockchip_drm.h"
#include "rockchip_drmif.h"
#include "rockchip_rga.h"

#define DRM_MODULE_NAME		"rockchip"
#define MAX_TEST_CASE		1

struct rga_context *ctx;

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

static void connector_find_mode(int fd, struct connector *c, drmModeRes *resources)
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

static int drm_set_crtc(struct rockchip_device *dev, struct connector *c,
			unsigned int fb_id)
{
	int ret;

	ret = drmModeSetCrtc(dev->fd, c->crtc, fb_id, 0, 0, &c->id, 1, c->mode);
	if (ret) {
		printf("failed to set mode: %s\n", strerror(errno));
		goto err;
	}

	return 0;

err:
	return ret;
}

static struct rockchip_bo *rockchip_create_buffer(struct rockchip_device *dev,
						  unsigned long size,
						  unsigned int flags)
{
	struct rockchip_bo *bo;

	bo = rockchip_bo_create(dev, size, flags);
	if (!bo)
		return bo;

	if (!rockchip_bo_map(bo)) {
		rockchip_bo_destroy(bo);
		return NULL;
	}

	return bo;
}

static void rockchip_destroy_buffer(struct rockchip_bo *bo)
{
	rockchip_bo_destroy(bo);
}

static int rga_test(struct rockchip_device *dev, struct rga_image *src_img, struct rga_image *dst_img)
{
	int ret;

	/*
	 * RGA API Related:
	 *
	 * Initialize the source framebuffer and dest framebuffer with BLACK color.
	 *
	 * The "->fill_color" variable is corresponding to RGA target color, and it's
	 * ARGB8888 format, like if you want the source framebuffer filled with
	 * RED COLOR, then you should fill the "->fill_color" with 0x00ff0000.
	 */
	src_img->fill_color = 0x00;
	ret = rga_solid_fill(ctx, src_img, 0, 0, src_img->width, src_img->height);

	dst_img->fill_color = 0xff;
	ret = rga_solid_fill(ctx, dst_img, 0, 0, dst_img->width, dst_img->height);
	rga_exec(ctx);
	getchar();

	/*
	 * Created an rectangle color bar
	 */
	src_img->fill_color = 0xff00;
	ret = rga_solid_fill(ctx, src_img, 5, 5, 1000, 220);

	src_img->fill_color = 0xff;
	ret = rga_solid_fill(ctx, src_img, 5, 225, 1000, 220);

	src_img->fill_color = 0xff0000;
	ret = rga_solid_fill(ctx, src_img, 5, 445, 1000, 220);

	src_img->fill_color = 0xffffffff;
	ret = rga_solid_fill(ctx, src_img, 20, 5, 50, 700);

	/*
	 * RGA API Related:
	 *
	 * This is an important API, when your program called the rga_exec(),
	 * then RGA driver would start to complete all the previous changes
	 * you want, for here it's src_fb / dst_fb solid operations.
	 */
	rga_exec(ctx);

	/*
	 * RGA API Related:
	 *
	 * This code would SCALING the source framebuffer, and place
	 * the output to dest framebuffer, and the window size is:
	 *
	 * Source Window		Dest Window
	 * Start  -  End		Start  -  End
	 * (0, 0) -  (1088, 720)	(0.0)  -  (200, 400)
	 */
	ret = rga_multiple_transform(ctx, src_img, dst_img,
				     0, 0, 1080, 720,
				     0, 0, 720, 480,
				     0, 0, 0);
	rga_exec(ctx);
	getchar();


	/*
	 * RGA API Related:
	 *
	 * This code would SCALING and RORATE 90 Degree the source
	 * framebuffer, and place the output to dest framebuffer,
	 * and the window size is:
	 */
	ret = rga_multiple_transform(ctx, src_img, dst_img,
				     0, 0, 1088, 720,
				     0, 0, 720, 480,
				     90, 0, 0);
	rga_exec(ctx);
	getchar();


	/*
	 * RGA API Related:
	 *
	 * This code would SCALING and RORATE 180 Degree the source
	 * framebuffer, and place the output to dest framebuffer,
	 * and the window size is:
	 */
	ret = rga_multiple_transform(ctx, src_img, dst_img,
				     0, 0, 1088, 720,
				     0, 0, 720, 480,
				     180, 0, 0);
	rga_exec(ctx);
	getchar();


	/*
	 * RGA API Related:
	 *
	 * This code would SCALING and RORATE 270 Degree the source
	 * framebuffer, and place the output to dest framebuffer,
	 * and the window size is:
	 */
	ret = rga_multiple_transform(ctx, src_img, dst_img,
				     0, 0, 1088, 720,
				     0, 0, 720, 480,
				     270, 0, 0);
	rga_exec(ctx);
	getchar();


	/*
	 * RGA API Related:
	 *
	 * This code would SCALING and RORATE 270 Degree the source
	 * framebuffer, and place the output to dest framebuffer,
	 * and the window size is:
	 */
	ret = rga_multiple_transform(ctx, src_img, dst_img,
				     0, 0, 1088, 720,
				     720, 0, 200, 200,
				     270, 0, 0);
	rga_exec(ctx);
	getchar();

	return 0;
}

static int rga_copy_nv12_to_nv12_test(struct rockchip_device *dev,
					   struct rockchip_bo *src,
					   struct rockchip_bo *dst,
					   struct connector *src_con,
					   struct connector *dst_con,
					   enum e_rga_buf_type type)
{
	struct rga_image src_img = {0}, dst_img = {0};
	unsigned int img_w, img_h;
	int dst_fd, src_fd;

	/*
	 * RGA API Related:
	 *
	 * Due to RGA API only accept the fd of dma_buf, so we need
	 * to conver the dma_buf Handle to dma_buf FD.
	 *
	 * And then just assigned the src/dst framebuffer FD to the
	 * "struct rga_img".
	 *
	 * And for now, RGA driver only support GEM buffer type, so
	 * we also need to assign the src/dst buffer type to RGA_IMGBUF_GEM.
	 *
	 * For futher, I would try to add user point support.
	 */
	drmPrimeHandleToFD(dev->fd, dst->handle, 0 , &dst_fd);
	drmPrimeHandleToFD(dev->fd, src->handle, 0 , &src_fd);

	dst_img.bo[0] = dst_fd;
	src_img.bo[0] = src_fd;

	/*
	 * RGA API Related:
	 *
	 * Configure the source FB width / height / stride / color_mode.
	 * 
	 * The width / height is correspond to the framebuffer width /height
	 *
	 * The stride is equal to (width * pixel_width).
	 *
	 * The color_mode should configure to the standard DRM color format
	 * which defined in "/user/include/drm/drm_fourcc.h"
	 *
	 */
	img_w = src_con->mode->hdisplay;
	img_h = src_con->mode->vdisplay;

	src_img.width = img_w;
	src_img.height = img_h;
	src_img.stride = img_w;
	src_img.buf_type = type;
	src_img.color_mode = DRM_FORMAT_NV12;


	/*
	 * RGA API Related:
	 *
	 * Configure the dest FB width / height / stride / color_mode.
	 * 
	 * The width / height is correspond to the framebuffer width /height
	 *
	 * The stride is equal to (width * pixel_width).
	 *
	 * The color_mode should configure to the standard DRM color format
	 * which defined in "/user/include/drm/drm_fourcc.h"
	 *
	 */
	img_w = dst_con->mode->hdisplay;
	img_h = dst_con->mode->vdisplay;

	dst_img.width = img_w;
	dst_img.height = img_h;
	dst_img.stride = img_w;
	dst_img.buf_type = type;
	dst_img.color_mode = DRM_FORMAT_NV12;


	/*
	 * RGA Tested Related:
	 *
	 * Start to run test between source FB and dest FB
	 */
	rga_test(dev, &src_img, &dst_img);


	close(src_fd);
	close(dst_fd);

	return 0;
}

static int rga_copy_argb8888_to_nv12_test(struct rockchip_device *dev, struct rockchip_bo *src,
					  struct rockchip_bo *dst, struct connector *src_con,
					  struct connector *dst_con, enum e_rga_buf_type type)
{
	struct rga_image src_img = {0}, dst_img = {0};
	unsigned int img_w, img_h;
	int dst_fd, src_fd;

	drmPrimeHandleToFD(dev->fd, dst->handle, 0 , &dst_fd);
	drmPrimeHandleToFD(dev->fd, src->handle, 0 , &src_fd);

	dst_img.bo[0] = dst_fd;
	src_img.bo[0] = src_fd;

	/*
	 * Source Framebuffer OPS
	 */
	img_w = src_con->mode->hdisplay;
	img_h = src_con->mode->vdisplay;

	src_img.width = img_w;
	src_img.height = img_h;
	src_img.stride = img_w * 4;
	src_img.buf_type = type;
	src_img.color_mode = DRM_FORMAT_ARGB8888;

	/*
	 * Dest Framebuffer OPS
	 */
	img_w = dst_con->mode->hdisplay;
	img_h = dst_con->mode->vdisplay;

	dst_img.width = img_w;
	dst_img.height = img_h;
	dst_img.stride = img_w;
	dst_img.buf_type = type;
	dst_img.color_mode = DRM_FORMAT_NV12;


	/*
	 * RGA Tested Related:
	 *
	 * Start to run test between source FB and dest FB
	 */
	rga_test(dev, &src_img, &dst_img);


	close(src_fd);
	close(dst_fd);

	return 0;
}

static struct rockchip_bo *init_crtc(struct connector *con,
				     struct rockchip_device *dev)
{
	struct rockchip_bo *bo;
	unsigned int screen_width, screen_height;
	drmModeRes *resources;

	resources = drmModeGetResources(dev->fd);
	if (!resources) {
		fprintf(stderr, "drmModeGetResources failed: %s\n",
				strerror(errno));
		return NULL;
	}

	connector_find_mode(dev->fd, con, resources);
	drmModeFreeResources(resources);
	if (!con->mode) {
		fprintf(stderr, "failed to find usable connector\n");
		return NULL;
	}

	screen_width = con->mode->hdisplay;
	screen_height = con->mode->vdisplay;

	if (screen_width == 0 || screen_height == 0) {
		fprintf(stderr, "failed to find sane resolution on connector\n");
		return NULL;
	}

	printf("screen width = %d, screen height = %d\n", screen_width, screen_height);

	bo = rockchip_create_buffer(dev, screen_width * screen_height * 4, 0);
	if (!bo) {
		return NULL;
	}

	con->plane_zpos = -1;

	return bo;
}

static int rga_argb8888_to_nv12_test(struct rockchip_device *dev,
		struct rockchip_bo *src_bo, struct rockchip_bo *dst_bo,
		struct connector *src_con, struct connector *dst_con)
{
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
	unsigned int dst_fb_id;
	int ret, modes;

	/*
	 * Dest FB Displayed Related:
	 *
	 * Add the dest framebuffer to DRM connector, note that for NV12
	 * display, the virtual stride is (width), that's why pitches[0]
	 * is hdisplay.
	 */
	modes = DRM_FORMAT_NV12;
	handles[0] = dst_bo->handle;
	pitches[0] = dst_con->mode->hdisplay;
	offsets[0] = 0;
	handles[1] = dst_bo->handle;
	pitches[1] = dst_con->mode->hdisplay;
	offsets[1] = dst_con->mode->hdisplay * dst_con->mode->vdisplay;
        
	ret = drmModeAddFB2(dev->fd, dst_con->mode->hdisplay, dst_con->mode->vdisplay,
			    modes, handles, pitches, offsets, &dst_fb_id, 0);
	if (ret < 0)
		return -EFAULT;

	ret = drm_set_crtc(dev, dst_con, dst_fb_id);
	if (ret < 0)
		return -EFAULT;


	/*
	 * TEST RGA Related:
	 *
	 * Start to configure the RGA module and run test
	 */
	ret = rga_copy_argb8888_to_nv12_test(dev, src_bo, dst_bo, src_con, dst_con, RGA_IMGBUF_GEM);
	if (ret < 0)
		return -EFAULT;


	/*
	 * Display Related:
	 *
	 * Released the display framebufffer refer which hold
	 * by DRM display framework
	 */
	drmModeRmFB(dev->fd, dst_fb_id);

	return 0;
}

static int rga_nv12_to_nv12_test(struct rockchip_device *dev,
		struct rockchip_bo *src_bo, struct rockchip_bo *dst_bo,
		struct connector *src_con, struct connector *dst_con)
{
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
	unsigned int dst_fb_id;
	int ret, modes;


	/*
	 * Dest FB Displayed Related:
	 *
	 * Add the dest framebuffer to DRM connector, note that for NV12
	 * display, the virtual stride is (width), that's why pitches[0]
	 * is hdisplay.
	 */
	modes = DRM_FORMAT_NV12;
	handles[0] = dst_bo->handle;
	pitches[0] = dst_con->mode->hdisplay;
	offsets[0] = 0;
        handles[1] = dst_bo->handle;
        pitches[1] = dst_con->mode->hdisplay;
        offsets[1] = dst_con->mode->hdisplay * dst_con->mode->vdisplay;
        
	ret = drmModeAddFB2(dev->fd, dst_con->mode->hdisplay, dst_con->mode->vdisplay,
			    modes, handles, pitches, offsets, &dst_fb_id, 0);
	if (ret < 0)
		return -EFAULT;

	ret = drm_set_crtc(dev, dst_con, dst_fb_id);
	if (ret < 0)
		return -EFAULT;


	/*
	 * TEST RGA Related:
	 *
	 * Start to configure the RGA module and run test
	 */
	ret = rga_copy_nv12_to_nv12_test(dev, src_bo, dst_bo, src_con, dst_con, RGA_IMGBUF_GEM);
	if (ret < 0) {
		fprintf(stderr, "failed to test copy operation.\n");
		return -EFAULT;
	}


	/*
	 * Display Related:
	 *
	 * Released the display framebufffer refer which hold
	 * by DRM display framework
	 */
	drmModeRmFB(dev->fd, dst_fb_id);

	return 0;
}

int main(int argc, char **argv)
{
	struct rockchip_device *dev;
	struct rockchip_bo *dst_bo, *src_bo;
	struct connector src_con, dst_con;
	int fd;

	fd = drmOpen(DRM_MODULE_NAME, NULL);
	if (fd < 0) {
		fprintf(stderr, "failed to open.\n");
		return fd;
	}

	dev = rockchip_device_create(fd);
	if (!dev) {
		drmClose(dev->fd);
		return -EFAULT;
	}

	/*
	 * RGA API Related:
	 *
	 * Open the RGA device
	 */
	ctx = rga_init(dev->fd);
	if (!ctx)
		return -EFAULT;


	/*
	 * Test Display Related:
	 *
	 * Source framebuffer display connector init. Just a
	 * hack. Directly use the eDP monitor, and force to
	 * use the 1920x1080 display mode.
	 */
	memset(&src_con, 0, sizeof(struct connector));
	src_con.crtc = -1;
	src_con.id = 33;
	src_con.mode = alloca(sizeof(drmModeModeInfo));
	src_con.mode->hdisplay = 1920;
	src_con.mode->vdisplay = 1080;
	src_con.plane_zpos = -1;

	src_bo = rockchip_create_buffer(dev, src_con.mode->hdisplay * src_con.mode->vdisplay * 4, 0);
	if (!src_bo) {
		fprintf(stderr, "Failed to create source fb!\n");
		return -EFAULT;
	}


	/*
	 * Test Display Related:
	 *
	 * Dest framebuffer display connector init. Just a
	 * hack. Directly use the eDP monitor, and force to
	 * use the 1280x800 display mode.
	 */
	memset(&dst_con, 0, sizeof(struct connector));
	dst_con.crtc = -1;
	dst_con.id = 30;
	strcpy(dst_con.mode_str, "1920x1080");
	dst_bo = init_crtc(&dst_con, dev);
	if (dst_bo == NULL) {
		printf("init dst crtc failed \n");
		return 0;
	}

	/*
	 * TEST RGA Related:
	 *
	 * This would start to run RGA test with those index:
	 *   Color format transform: ARGB8888 -> NV12
	 *   Rotatoion
	 *   Scaling
	 */
	printf("Satrting ARGB8888 to NV12 RGA test, [Press Enter to continue]\n");
	rga_argb8888_to_nv12_test(dev, src_bo, dst_bo, &src_con, &dst_con);

	printf("Satrting NV12 to NV12 RGA test, [Press Enter to continue]\n");
	rga_nv12_to_nv12_test(dev, src_bo, dst_bo, &src_con, &dst_con);

	/*
	 * RGA API Related:
	 *
	 * Close the RGA device
	 */
	rga_fini(ctx);

	rockchip_destroy_buffer(src_bo);
	rockchip_destroy_buffer(dst_bo);

	drmClose(dev->fd);
	rockchip_device_destroy(dev);

	return 0;
}
