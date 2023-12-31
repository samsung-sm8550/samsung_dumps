/*
 * Copyright (c) 2021-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <drm/drm_crtc.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_probe_helper.h>
#include <linux/mem-buf.h>
#include <soc/qcom/secure_buffer.h>

#include "msm_drv.h"
#include "msm_kms.h"
#include "msm_gem.h"
#include "sde_dbg.h"

struct msm_framebuffer {
	struct drm_framebuffer base;
	const struct msm_format *format;
	u32 cache_flags;
	u32 cache_rd_type;
	u32 cache_wr_type;
};
#define to_msm_framebuffer(x) container_of(x, struct msm_framebuffer, base)

static const struct drm_framebuffer_funcs msm_framebuffer_funcs = {
	.create_handle = drm_gem_fb_create_handle,
	.destroy = drm_gem_fb_destroy,
	.dirty = drm_atomic_helper_dirtyfb,
};

/* prepare/pin all the fb's bo's for scanout.  Note that it is not valid
 * to prepare an fb more multiple different initiator 'id's.  But that
 * should be fine, since only the scanout (mdpN) side of things needs
 * this, the gpu doesn't care about fb's.
 */
int msm_framebuffer_prepare(struct drm_framebuffer *fb,
		struct msm_gem_address_space *aspace)
{
	struct msm_framebuffer *msm_fb;
	int ret, i, n;
	uint64_t iova;

	if (!fb) {
		DRM_ERROR("from:%pS null fb\n", __builtin_return_address(0));
		return -EINVAL;
	}

	msm_fb = to_msm_framebuffer(fb);
	n = fb->format->num_planes;
	for (i = 0; i < n; i++) {
		ret = msm_gem_get_iova(fb->obj[i], aspace, &iova);
		DBG("FB[%u]: iova[%d]: %08llx (%d)", fb->base.id, i, iova, ret);
		if (ret)
			return ret;
	}

	return 0;
}

void msm_framebuffer_cleanup(struct drm_framebuffer *fb,
		struct msm_gem_address_space *aspace)
{
	struct msm_framebuffer *msm_fb;
	int i, n;

	if (fb == NULL) {
		DRM_ERROR("from:%pS null fb\n", __builtin_return_address(0));
		return;
	}

	msm_fb = to_msm_framebuffer(fb);
	n = fb->format->num_planes;

	for (i = 0; i < n; i++)
		msm_gem_put_iova(fb->obj[i], aspace);
}

uint32_t msm_framebuffer_iova(struct drm_framebuffer *fb,
		struct msm_gem_address_space *aspace, int plane)
{

	if (!fb) {
		DRM_ERROR("from:%pS null fb\n", __builtin_return_address(0));
		return -EINVAL;
	}

	if (!fb->obj[plane])
		return 0;

	return msm_gem_iova(fb->obj[plane], aspace) + fb->offsets[plane];
}

uint32_t msm_framebuffer_phys(struct drm_framebuffer *fb,
		int plane)
{
	struct msm_framebuffer *msm_fb;
	dma_addr_t phys_addr;

	if (!fb) {
		DRM_ERROR("from:%pS null fb\n", __builtin_return_address(0));
		return -EINVAL;
	}

	msm_fb = to_msm_framebuffer(fb);

	if (!msm_fb->base.obj[plane])
		return 0;

	phys_addr = msm_gem_get_dma_addr(msm_fb->base.obj[plane]);
	if (!phys_addr)
		return 0;

	return phys_addr + fb->offsets[plane];
}

struct drm_gem_object *msm_framebuffer_bo(struct drm_framebuffer *fb, int plane)
{
	if (!fb) {
		DRM_ERROR("from:%pS null fb\n", __builtin_return_address(0));
		return ERR_PTR(-EINVAL);
	}

	return drm_gem_fb_get_obj(fb, plane);
}

const struct msm_format *msm_framebuffer_format(struct drm_framebuffer *fb)
{
	return fb ? (to_msm_framebuffer(fb))->format : NULL;
}

struct drm_framebuffer *msm_framebuffer_create(struct drm_device *dev,
		struct drm_file *file, const struct drm_mode_fb_cmd2 *mode_cmd)
{
	const struct drm_format_info *info = drm_get_format_info(dev,
								mode_cmd);
	struct drm_gem_object *bos[4] = {0};
	struct drm_framebuffer *fb;
	int ret, i, n = info->num_planes;

	for (i = 0; i < n; i++) {
		bos[i] = drm_gem_object_lookup(file, mode_cmd->handles[i]);
		if (!bos[i]) {
			ret = -ENXIO;
			goto out_unref;
		}
	}

	fb = msm_framebuffer_init(dev, mode_cmd, bos);
	if (IS_ERR(fb)) {
		ret = PTR_ERR(fb);
		goto out_unref;
	}

	return fb;

out_unref:
	for (i = 0; i < n; i++)
		drm_gem_object_put(bos[i]);
	return ERR_PTR(ret);
}

struct drm_framebuffer *msm_framebuffer_init(struct drm_device *dev,
		const struct drm_mode_fb_cmd2 *mode_cmd,
		struct drm_gem_object **bos)
{
	const struct drm_format_info *info = drm_get_format_info(dev,
								mode_cmd);
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_kms *kms = priv->kms;
	struct msm_framebuffer *msm_fb = NULL;
	struct drm_framebuffer *fb;
	const struct msm_format *format;
	int ret, i, num_planes;
	bool is_modified = false;

	DBG("create framebuffer: dev=%pK, mode_cmd=%pK (%dx%d@%4.4s)",
			dev, mode_cmd, mode_cmd->width, mode_cmd->height,
			(char *)&mode_cmd->pixel_format);

	num_planes = info->num_planes;

	format = kms->funcs->get_format(kms, mode_cmd->pixel_format,
			mode_cmd->modifier[0]);
	if (!format) {
		DISP_DEV_ERR(dev->dev, "unsupported pixel format: %4.4s\n",
				(char *)&mode_cmd->pixel_format);
		ret = -EINVAL;
		goto fail;
	}

	msm_fb = kzalloc(sizeof(*msm_fb), GFP_KERNEL);
	if (!msm_fb) {
		ret = -ENOMEM;
		goto fail;
	}

	fb = &msm_fb->base;

	msm_fb->format = format;

	if (mode_cmd->flags & DRM_MODE_FB_MODIFIERS) {
		for (i = 0; i < ARRAY_SIZE(mode_cmd->modifier); i++) {
			if (mode_cmd->modifier[i]) {
				is_modified = true;
				break;
			}
		}
	}

	if (num_planes > ARRAY_SIZE(fb->obj)) {
		ret = -EINVAL;
		goto fail;
	}

	if (is_modified) {
		if (!kms->funcs->check_modified_format) {
			DISP_DEV_ERR(dev->dev, "can't check modified fb format\n");
			ret = -EINVAL;
			goto fail;
		} else {
			ret = kms->funcs->check_modified_format(
					kms, msm_fb->format, mode_cmd, bos);
			if (ret)
				goto fail;
		}
	} else {
		const struct drm_format_info *info;

		info = drm_format_info(mode_cmd->pixel_format);
		if (!info || num_planes > ARRAY_SIZE(info->cpp)) {
			ret = -EINVAL;
			goto fail;
		}

		for (i = 0; i < num_planes; i++) {
			unsigned int width = mode_cmd->width / (i ?
					info->hsub : 1);
			unsigned int height = mode_cmd->height / (i ?
					info->vsub : 1);
			unsigned int min_size;

			min_size = (height - 1) * mode_cmd->pitches[i]
				+ width * info->cpp[i]
				+ mode_cmd->offsets[i];

			if (!bos[i] || bos[i]->size < min_size) {
				ret = -EINVAL;
				goto fail;
			}
		}
	}

	for (i = 0; i < num_planes; i++)
		msm_fb->base.obj[i] = bos[i];

	drm_helper_mode_fill_fb_struct(dev, fb, mode_cmd);

	ret = drm_framebuffer_init(dev, fb, &msm_framebuffer_funcs);
	if (ret) {
		DISP_DEV_ERR(dev->dev, "framebuffer init failed: %d\n", ret);
		goto fail;
	}

	DBG("create: FB ID: %d (%pK)", fb->base.id, fb);

	return fb;

fail:
	kfree(msm_fb);

	return ERR_PTR(ret);
}

int msm_framebuffer_set_cache_hint(struct drm_framebuffer *fb,
		u32 flags, u32 rd_type, u32 wr_type)
{
	struct msm_framebuffer *msm_fb;

	if (!fb)
		return -EINVAL;

	msm_fb = to_msm_framebuffer(fb);
	msm_fb->cache_flags = flags;
	msm_fb->cache_rd_type = rd_type;
	msm_fb->cache_wr_type = wr_type;

	return 0;
}

int  msm_framebuffer_get_cache_hint(struct drm_framebuffer *fb,
		u32 *flags, u32 *rd_type, u32 *wr_type)
{
	struct msm_framebuffer *msm_fb;

	if (!fb)
		return -EINVAL;

	msm_fb = to_msm_framebuffer(fb);
	*flags = msm_fb->cache_flags;
	*rd_type = msm_fb->cache_rd_type;
	*wr_type = msm_fb->cache_wr_type;

	return 0;
}

int msm_fb_obj_get_attrs(struct drm_gem_object *obj, int *fb_ns,
		int *fb_sec, int *fb_sec_dir)
{
	struct msm_gem_object *msm_obj = to_msm_bo(obj);
	struct dma_buf *dma_buf;
	int *vmid_list, *perms_list;
	int nelems = 0;
	int i, ret = 0;

	if (!(msm_obj->flags & MSM_BO_EXTBUF))
		return 0;

	if (!obj->import_attach) {
		DRM_DEBUG("NULL attachment in drm gem object flags:0x%x\n",
			msm_obj->flags);
		return -EINVAL;
	}

	dma_buf = obj->import_attach->dmabuf;
	if (!dma_buf) {
		DRM_DEBUG("dma_buf NULL in drm gem object\n");
		return -EINVAL;
	}

	ret = mem_buf_dma_buf_copy_vmperm(dma_buf, &vmid_list,
			&perms_list, &nelems);
	if (ret) {
		DRM_ERROR("mem_buf_dma_buf_copy_vmperm failure, err=%d\n", ret);
		return ret;
	}

	/* obtain the VMIDs of a buffer */
	if (mem_buf_dma_buf_exclusive_owner(dma_buf))
		*fb_ns = 1;
	else {
		for (i = 0; i < nelems; i++) {
			if (vmid_list[i] == VMID_CP_PIXEL)
				*fb_sec = 1;
			else if (vmid_list[i] & (VMID_CP_SEC_DISPLAY |
					VMID_CP_CAMERA_PREVIEW))
				*fb_sec_dir = 1;
		}
	}

	kfree(vmid_list);
	kfree(perms_list);

	return ret;
}
