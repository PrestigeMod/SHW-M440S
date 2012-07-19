/*
 * Copyright (C) 2012 Samsung Electronics Co.Ltd
 * Authors: YoungJun Cho <yj44.cho@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundationr
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>

#include "drmP.h"
#include "exynos_drm.h"
#include "exynos_drm_drv.h"
#include "exynos_drm_ipp.h"

/* Configuration */
#define ROT_CONFIG			0x00
#define ROT_CONFIG_IRQ			(3 << 8)

/* Image Control */
#define ROT_CONTROL			0x10
#define ROT_CONTROL_PATTERN_WRITE	(1 << 16)
#define ROT_CONTROL_FMT_YCBCR420_2P	(1 << 8)
#define ROT_CONTROL_FMT_RGB888		(6 << 8)
#define ROT_CONTROL_FMT_MASK		(7 << 8)
#define ROT_CONTROL_FLIP_VERTICAL	(2 << 6)
#define ROT_CONTROL_FLIP_HORIZONTAL	(3 << 6)
#define ROT_CONTROL_FLIP_MASK		(3 << 6)
#define ROT_CONTROL_ROT_90		(1 << 4)
#define ROT_CONTROL_ROT_180		(2 << 4)
#define ROT_CONTROL_ROT_270		(3 << 4)
#define ROT_CONTROL_ROT_MASK		(3 << 4)
#define ROT_CONTROL_START		(1 << 0)

/* Status */
#define ROT_STATUS			0x20
#define ROT_STATUS_IRQ_PENDING(x)	(1 << (x))
#define ROT_STATUS_IRQ(x)		(((x) >> 8) & 0x3)
#define ROT_STATUS_IRQ_VAL_COMPLETE	1
#define ROT_STATUS_IRQ_VAL_ILLEGAL	2

/* Sourc Buffer Address */
#define ROT_SRC_BUF_ADDR(n)		(0x30 + ((n) << 2))

/* Source Buffer Size */
#define ROT_SRC_BUF_SIZE		0x3c
#define ROT_SRC_BUF_SIZE_H(x)		((x) << 16)
#define ROT_SRC_BUF_SIZE_W(x)		((x) << 0)

/* Source Crop Position */
#define ROT_SRC_CROP_POS		0x40
#define ROT_SRC_CROP_POS_Y(x)		((x) << 16)
#define ROT_SRC_CROP_POS_X(x)		((x) << 0)

/* Source Crop Size */
#define ROT_SRC_CROP_SIZE		0x44
#define ROT_SRC_CROP_SIZE_H(x)		((x) << 16)
#define ROT_SRC_CROP_SIZE_W(x)		((x) << 0)

/* Destination Buffer Address */
#define ROT_DST_BUF_ADDR(n)		(0x50 + ((n) << 2))

/* Destination Buffer Size */
#define ROT_DST_BUF_SIZE		0x5c
#define ROT_DST_BUF_SIZE_H(x)		((x) << 16)
#define ROT_DST_BUF_SIZE_W(x)		((x) << 0)

/* Destination Crop Position */
#define ROT_DST_CROP_POS		0x60
#define ROT_DST_CROP_POS_Y(x)		((x) << 16)
#define ROT_DST_CROP_POS_X(x)		((x) << 0)

/* Round to nearest aligned value */
#define ROT_ALIGN(x, align, mask)	(((x) + (1 << ((align) - 1))) & (mask))
/* Minimum limit value */
#define ROT_MIN(min, mask)		(((min) + ~(mask)) & (mask))
/* Maximum limit value */
#define ROT_MAX(max, mask)		((max) & (mask))

enum rot_irq_status {
	ROT_IRQ_STATUS_COMPLETE	= 8,
	ROT_IRQ_STATUS_ILLEGAL	= 9,
};

struct rot_limit {
	u32	min_w;
	u32	min_h;
	u32	max_w;
	u32	max_h;
	u32	align;
};

struct rot_limit_table {
	struct rot_limit	ycbcr420_2p;
	struct rot_limit	rgb888;
};

struct rot_context {
	struct rot_limit_table		*limit_tbl;
	struct clk			*clock;
	struct resource			*regs_res;
	void __iomem			*regs;
	int				irq;
	struct exynos_drm_ippdrv	ippdrv;
	struct drm_exynos_ipp_property	property;
	bool				suspended;
};

static void rotator_reg_set_irq(struct rot_context *rot, bool enable)
{
	u32 value = readl(rot->regs + ROT_CONFIG);

	if (enable == true)
		value |= ROT_CONFIG_IRQ;
	else
		value &= ~ROT_CONFIG_IRQ;

	writel(value, rot->regs + ROT_CONFIG);
}

static void rotator_reg_set_format(struct rot_context *rot, u32 img_fmt)
{
	u32 value = readl(rot->regs + ROT_CONTROL);
	value &= ~ROT_CONTROL_FMT_MASK;

	switch (img_fmt) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV12M:
		value |= ROT_CONTROL_FMT_YCBCR420_2P;
		break;
	case DRM_FORMAT_XRGB8888:
		value |= ROT_CONTROL_FMT_RGB888;
		break;
	default:
		DRM_ERROR("invalid image format\n");
		return;
	}

	writel(value, rot->regs + ROT_CONTROL);
}

static void rotator_reg_set_flip(struct rot_context *rot,
						enum drm_exynos_flip flip)
{
	u32 value = readl(rot->regs + ROT_CONTROL);
	value &= ~ROT_CONTROL_FLIP_MASK;

	switch (flip) {
	case EXYNOS_DRM_FLIP_VERTICAL:
		value |= ROT_CONTROL_FLIP_VERTICAL;
		break;
	case EXYNOS_DRM_FLIP_HORIZONTAL:
		value |= ROT_CONTROL_FLIP_HORIZONTAL;
		break;
	default:
		/* Flip None */
		break;
	}

	writel(value, rot->regs + ROT_CONTROL);
}

static void rotator_reg_set_rotation(struct rot_context *rot,
					enum drm_exynos_degree degree)
{
	u32 value = readl(rot->regs + ROT_CONTROL);
	value &= ~ROT_CONTROL_ROT_MASK;

	switch (degree) {
	case EXYNOS_DRM_DEGREE_90:
		value |= ROT_CONTROL_ROT_90;
		break;
	case EXYNOS_DRM_DEGREE_180:
		value |= ROT_CONTROL_ROT_180;
		break;
	case EXYNOS_DRM_DEGREE_270:
		value |= ROT_CONTROL_ROT_270;
		break;
	default:
		/* Rotation 0 Degree */
		break;
	}

	writel(value, rot->regs + ROT_CONTROL);
}

static void rotator_reg_set_start(struct rot_context *rot)
{
	u32 value = readl(rot->regs + ROT_CONTROL);

	value |= ROT_CONTROL_START;

	writel(value, rot->regs + ROT_CONTROL);
}

static enum rot_irq_status rotator_reg_get_irq_status(struct rot_context *rot)
{
	u32 value = readl(rot->regs + ROT_STATUS);
	value = ROT_STATUS_IRQ(value);

	if (value == ROT_STATUS_IRQ_VAL_COMPLETE)
		return ROT_IRQ_STATUS_COMPLETE;
	else
		return ROT_IRQ_STATUS_ILLEGAL;
}

static void rotator_reg_set_irq_status_clear(struct rot_context *rot,
						enum rot_irq_status status)
{
	u32 value = readl(rot->regs + ROT_STATUS);

	value |= ROT_STATUS_IRQ_PENDING((u32)status);

	writel(value, rot->regs + ROT_STATUS);
}

static void rotator_reg_set_src_buf_addr(struct rot_context *rot,
							dma_addr_t addr, int i)
{
	writel(addr, rot->regs + ROT_SRC_BUF_ADDR(i));
}

static void rotator_reg_set_src_buf_size(struct rot_context *rot, u32 w, u32 h)
{
	u32 value = ROT_SRC_BUF_SIZE_H(h) | ROT_SRC_BUF_SIZE_W(w);

	writel(value, rot->regs + ROT_SRC_BUF_SIZE);
}

static void rotator_reg_set_src_crop_pos(struct rot_context *rot, u32 x, u32 y)
{
	u32 value = ROT_SRC_CROP_POS_Y(y) | ROT_SRC_CROP_POS_X(x);

	writel(value, rot->regs + ROT_SRC_CROP_POS);
}

static void rotator_reg_set_src_crop_size(struct rot_context *rot, u32 w, u32 h)
{
	u32 value = ROT_SRC_CROP_SIZE_H(h) | ROT_SRC_CROP_SIZE_W(w);

	writel(value, rot->regs + ROT_SRC_CROP_SIZE);
}

static void rotator_reg_set_dst_buf_addr(struct rot_context *rot,
							dma_addr_t addr, int i)
{
	writel(addr, rot->regs + ROT_DST_BUF_ADDR(i));
}

static void rotator_reg_set_dst_buf_size(struct rot_context *rot, u32 w, u32 h)
{
	u32 value = ROT_DST_BUF_SIZE_H(h) | ROT_DST_BUF_SIZE_W(w);

	writel(value, rot->regs + ROT_DST_BUF_SIZE);
}

static void rotator_reg_set_dst_crop_pos(struct rot_context *rot, u32 x, u32 y)
{
	u32 value = ROT_DST_CROP_POS_Y(y) | ROT_DST_CROP_POS_X(x);

	writel(value, rot->regs + ROT_DST_CROP_POS);
}

static void rotator_reg_get_dump(struct rot_context *rot)
{
	u32 value, i;

	for (i = 0; i <= ROT_DST_CROP_POS; i += 0x4) {
		value = readl(rot->regs + i);
		DRM_INFO("[%s] [0x%x] : 0x%x\n", __func__, i, value);
	}
}

static irqreturn_t rotator_irq_handler(int irq, void *arg)
{
	struct rot_context *rot = arg;
	struct exynos_drm_ippdrv *ippdrv = &rot->ippdrv;
	enum rot_irq_status irq_status;

	/* Get execution result */
	irq_status = rotator_reg_get_irq_status(rot);
	rotator_reg_set_irq_status_clear(rot, irq_status);

	if (irq_status != ROT_IRQ_STATUS_COMPLETE) {
		DRM_ERROR("the SFR is set illegally\n");
		rotator_reg_get_dump(rot);
	}

	if (irq_status == ROT_IRQ_STATUS_COMPLETE)
		ipp_send_event_handler(ippdrv, 0);

	return IRQ_HANDLED;
}

static void rotator_align_size(struct rot_context *rot, u32 fmt,
						struct drm_exynos_pos *pos)
{
	struct rot_limit_table *limit_tbl = rot->limit_tbl;
	struct rot_limit *limit;
	u32 mask, value;

	/* Get size limit */
	if (fmt == DRM_FORMAT_XRGB8888)
		limit = &limit_tbl->rgb888;
	else
		limit = &limit_tbl->ycbcr420_2p;

	/* Get mask for rounding to nearest aligned value */
	mask = ~((1 << limit->align) - 1);

	/* Set aligned width */
	value = ROT_ALIGN(pos->w, limit->align, mask);
	if (value < limit->min_w)
		pos->w = ROT_MIN(limit->min_w, mask);
	else if (value > limit->max_w)
		pos->w = ROT_MAX(limit->max_w, mask);
	else
		pos->w = value;

	/* Set aligned height */
	value = ROT_ALIGN(pos->h, limit->align, mask);
	if (value < limit->min_h)
		pos->h = ROT_MIN(limit->min_h, mask);
	else if (value > limit->max_h)
		pos->h = ROT_MAX(limit->max_h, mask);
	else
		pos->h = value;
}

static int rotator_src_set_transf(struct device *dev,
					enum drm_exynos_degree degree,
					enum drm_exynos_flip flip)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	struct drm_exynos_ipp_property *property = &rot->property;
	struct drm_exynos_ipp_config *config =
					&property->config[EXYNOS_DRM_OPS_SRC];

	/* Check validity */
	if (degree != EXYNOS_DRM_DEGREE_0) {
		DRM_ERROR("invalid degree\n");
		return -EINVAL;
	}
	if (flip != EXYNOS_DRM_FLIP_NONE) {
		DRM_ERROR("invalid flip\n");
		return -EINVAL;
	}

	/* Set transform configuration */
	config->ops_id = EXYNOS_DRM_OPS_SRC;
	config->degree = degree;
	config->flip = flip;

	return 0;
}

static int rotator_src_set_size(struct device *dev, int swap,
						struct drm_exynos_pos *pos,
						struct drm_exynos_sz *sz)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	struct drm_exynos_ipp_property *property = &rot->property;
	struct drm_exynos_ipp_config *config =
					&property->config[EXYNOS_DRM_OPS_SRC];

	/* Check boundary */
	if ((pos->x + pos->w > sz->hsize) || (pos->y + pos->h > sz->vsize)) {
		DRM_ERROR("out of bound\n");
		return -EINVAL;
	}

	/* Set buffer size configuration */
	config->sz.hsize = sz->hsize;
	config->sz.vsize = sz->vsize;

	rotator_reg_set_src_buf_size(rot, config->sz.hsize, config->sz.vsize);

	/* Set crop image position configuration */
	config->pos.x = pos->x;
	config->pos.y = pos->y;
	config->pos.w = pos->w;
	config->pos.h = pos->h;

	rotator_reg_set_src_crop_pos(rot, config->pos.x, config->pos.y);
	rotator_reg_set_src_crop_size(rot, config->pos.w, config->pos.h);

	return 0;
}

static int rotator_src_set_fmt(struct device *dev, u32 fmt)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	struct drm_exynos_ipp_property *property = &rot->property;
	struct drm_exynos_ipp_config *config =
					&property->config[EXYNOS_DRM_OPS_SRC];

	/* Check validity */
	switch (fmt) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV12M:
		/* No problem */
		break;
	default:
		DRM_ERROR("invalid format\n");
		return -EINVAL;
	}

	/* Align size */
	rotator_align_size(rot, fmt, &config->pos);

	/* Set format configuration */
	config->fmt = fmt;

	return 0;
}

static int rotator_src_set_addr(struct device *dev, dma_addr_t *base, u32 id,
					enum drm_exynos_ipp_buf_ctrl ctrl)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	struct drm_exynos_ipp_property *property = &rot->property;
	dma_addr_t addr[EXYNOS_DRM_PLANER_MAX];
	struct drm_exynos_ipp_config *config =
					&property->config[EXYNOS_DRM_OPS_SRC];
	int i;

	/* Check ctrl */
	switch (ctrl) {
	case IPP_BUF_CTRL_MAP:
	case IPP_BUF_CTRL_UNMAP:
		/* Set address configuration */
		for (i = 0; i < EXYNOS_DRM_PLANER_MAX; i++)
			addr[i] = base[i];

		/* Re-set address of CB(CR) for NV12 format case */
		if ((ctrl == IPP_BUF_CTRL_MAP) &&
			(config->fmt == DRM_FORMAT_NV12))
			addr[EXYNOS_DRM_PLANER_CB] =
						addr[EXYNOS_DRM_PLANER_Y] +
						config->pos.w * config->pos.h;

		for (i = 0; i < EXYNOS_DRM_PLANER_MAX; i++)
			rotator_reg_set_src_buf_addr(rot, addr[i], i);
		break;
	default:
		/* Nothing to do */
		break;
	}

	return 0;
}

static int rotator_dst_set_transf(struct device *dev,
					enum drm_exynos_degree degree,
					enum drm_exynos_flip flip)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	struct drm_exynos_ipp_property *property = &rot->property;
	struct drm_exynos_ipp_config *config =
					&property->config[EXYNOS_DRM_OPS_DST];

	/* Check validity */
	switch (degree) {
	case EXYNOS_DRM_DEGREE_0:
	case EXYNOS_DRM_DEGREE_90:
	case EXYNOS_DRM_DEGREE_180:
	case EXYNOS_DRM_DEGREE_270:
		/* No problem */
		break;
	default:
		DRM_ERROR("invalid degree\n");
		return -EINVAL;
	}

	switch (flip) {
	case EXYNOS_DRM_FLIP_NONE:
	case EXYNOS_DRM_FLIP_VERTICAL:
	case EXYNOS_DRM_FLIP_HORIZONTAL:
		/* No problem */
		break;
	default:
		DRM_ERROR("invalid flip\n");
		return -EINVAL;
	}

	/* Set transform configuration */
	config->ops_id = EXYNOS_DRM_OPS_DST;
	config->degree = degree;
	config->flip = flip;

	rotator_reg_set_flip(rot, config->flip);
	rotator_reg_set_rotation(rot, config->degree);

	/* Check degree for setting buffer size swap */
	if ((degree == EXYNOS_DRM_DEGREE_90) ||
		(degree == EXYNOS_DRM_DEGREE_270))
		return 1;
	else
		return 0;
}

static int rotator_dst_set_size(struct device *dev, int swap,
						struct drm_exynos_pos *pos,
						struct drm_exynos_sz *sz)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	struct drm_exynos_ipp_property *property = &rot->property;
	struct drm_exynos_ipp_config *src_config =
					&property->config[EXYNOS_DRM_OPS_SRC];
	struct drm_exynos_ipp_config *config =
					&property->config[EXYNOS_DRM_OPS_DST];

	/* Check crop image size for NO scale feature */
	if ((src_config->pos.w != pos->w) || (src_config->pos.h != pos->h)) {
		DRM_ERROR("different size\n");
		return -EINVAL;
	}

	/* Check boundary */
	if (swap) {
		if ((pos->x + pos->h > sz->vsize) ||
			(pos->y + pos->w > sz->hsize)) {
			DRM_ERROR("out of bound\n");
			return -EINVAL;
		}
	} else {
		if ((pos->x + pos->w > sz->hsize) ||
			(pos->y + pos->h > sz->vsize)) {
			DRM_ERROR("out of bound\n");
			return -EINVAL;
		}
	}

	/* Set buffer size configuration */
	if (swap) {
		config->sz.hsize = sz->vsize;
		config->sz.vsize = sz->hsize;
	} else {
		config->sz.hsize = sz->hsize;
		config->sz.vsize = sz->vsize;
	}

	rotator_reg_set_dst_buf_size(rot, config->sz.hsize, config->sz.vsize);

	/* Set crop image position configuration */
	config->pos.x = pos->x;
	config->pos.y = pos->y;

	rotator_reg_set_dst_crop_pos(rot, config->pos.x, config->pos.y);

	return 0;
}

static int rotator_dst_set_fmt(struct device *dev, u32 fmt)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	struct drm_exynos_ipp_property *property = &rot->property;
	struct drm_exynos_ipp_config *src_config =
					&property->config[EXYNOS_DRM_OPS_SRC];
	struct drm_exynos_ipp_config *config =
					&property->config[EXYNOS_DRM_OPS_DST];

	/* Check validity */
	switch (fmt) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV12M:
		/* No problem */
		break;
	default:
		DRM_ERROR("invalid format\n");
		return -EINVAL;
	}

	if (src_config->fmt != fmt) {
		DRM_ERROR("diffrent format\n");
		return -EINVAL;
	}

	/* Align size */
	rotator_align_size(rot, fmt, &config->pos);

	/* Set format configuration */
	config->fmt = fmt;

	rotator_reg_set_format(rot, config->fmt);

	return 0;
}

static int rotator_dst_set_addr(struct device *dev, dma_addr_t *base, u32 id,
					enum drm_exynos_ipp_buf_ctrl ctrl)
{
	struct rot_context *rot = dev_get_drvdata(dev);
	dma_addr_t addr[EXYNOS_DRM_PLANER_MAX];
	struct drm_exynos_ipp_property *property = &rot->property;
	struct drm_exynos_ipp_config *config =
					&property->config[EXYNOS_DRM_OPS_DST];
	int i;

	/* Check ctrl */
	switch (ctrl) {
	case IPP_BUF_CTRL_MAP:
	case IPP_BUF_CTRL_UNMAP:
		/* Set address configuration */
		for (i = 0; i < EXYNOS_DRM_PLANER_MAX; i++)
			addr[i] = base[i];

		/* Re-set address of CB(CR) for NV12 format case */
		if ((ctrl == IPP_BUF_CTRL_MAP) &&
			(config->fmt == DRM_FORMAT_NV12))
			addr[EXYNOS_DRM_PLANER_CB] =
						addr[EXYNOS_DRM_PLANER_Y] +
						config->pos.w * config->pos.h;

		for (i = 0; i < EXYNOS_DRM_PLANER_MAX; i++)
			rotator_reg_set_dst_buf_addr(rot, addr[i], i);
		break;
	default:
		/* Nothing to do */
		break;
	}

	return 0;
}

static struct exynos_drm_ipp_ops rot_src_ops = {
	.set_transf	=	rotator_src_set_transf,
	.set_size	=	rotator_src_set_size,
	.set_fmt	=	rotator_src_set_fmt,
	.set_addr	=	rotator_src_set_addr,
};

static struct exynos_drm_ipp_ops rot_dst_ops = {
	.set_transf	=	rotator_dst_set_transf,
	.set_size	=	rotator_dst_set_size,
	.set_fmt	=	rotator_dst_set_fmt,
	.set_addr	=	rotator_dst_set_addr,
};

static int rotator_ippdrv_open(struct drm_device *drm_dev, struct device *dev,
							struct drm_file *file)
{
	struct rot_context *rot = dev_get_drvdata(dev);

	clk_enable(rot->clock);

	return 0;
}

static void rotator_ippdrv_close(struct drm_device *drm_dev, struct device *dev,
							struct drm_file *file)
{
	struct rot_context *rot = dev_get_drvdata(dev);

	clk_disable(rot->clock);

	return;
}

static int rotator_ippdrv_start(struct device *dev, enum drm_exynos_ipp_cmd cmd)
{
	struct rot_context *rot = dev_get_drvdata(dev);

	if (rot->suspended) {
		DRM_ERROR("suspended state\n");
		return -EPERM;
	}

	/* Set interrupt enable */
	rotator_reg_set_irq(rot, true);

	/* start rotator operation */
	rotator_reg_set_start(rot);

	return 0;
}

static int __devinit rotator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rot_context *rot;
	struct resource *res;
	struct exynos_drm_ippdrv *ippdrv;
	int ret;

	rot = kzalloc(sizeof(*rot), GFP_KERNEL);
	if (!rot) {
		dev_err(dev, "failed to allocate rot\n");
		return -ENOMEM;
	}

	rot->limit_tbl = (struct rot_limit_table *)
				platform_get_device_id(pdev)->driver_data;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "failed to find registers\n");
		ret = -ENOENT;
		goto err_get_resource;
	}

	rot->regs_res = request_mem_region(res->start, resource_size(res),
								dev_name(dev));
	if (!rot->regs_res) {
		dev_err(dev, "failed to claim register region\n");
		ret = -ENOENT;
		goto err_get_resource;
	}

	rot->regs = ioremap(res->start, resource_size(res));
	if (!rot->regs) {
		dev_err(dev, "failed to map register\n");
		ret = -ENXIO;
		goto err_ioremap;
	}

	rot->irq = platform_get_irq(pdev, 0);
	if (rot->irq < 0) {
		dev_err(dev, "faild to get irq\n");
		ret = rot->irq;
		goto err_get_irq;
	}

	ret = request_irq(rot->irq, rotator_irq_handler, 0, "drm_rotator", rot);
	if (ret < 0) {
		dev_err(dev, "failed to request irq\n");
		goto err_get_irq;
	}

	rot->clock = clk_get(dev, "rotator");
	if (IS_ERR_OR_NULL(rot->clock)) {
		dev_err(dev, "faild to get clock\n");
		ret = PTR_ERR(rot->clock);
		goto err_clk_get;
	}

	pm_runtime_enable(dev);

	ippdrv = &rot->ippdrv;
	ippdrv->dev = dev;
	ippdrv->ops[EXYNOS_DRM_OPS_SRC] = &rot_src_ops;
	ippdrv->ops[EXYNOS_DRM_OPS_DST] = &rot_dst_ops;
	ippdrv->open = rotator_ippdrv_open;
	ippdrv->close = rotator_ippdrv_close;
	ippdrv->start = rotator_ippdrv_start;

	platform_set_drvdata(pdev, rot);

	ret = exynos_drm_ippdrv_register(ippdrv);
	if (ret < 0) {
		dev_err(dev, "failed to register drm rotator device\n");
		goto err_ippdrv_register;
	}

	dev_info(dev, "The exynos rotator is probed successfully\n");

	return 0;

err_ippdrv_register:
	pm_runtime_disable(dev);
	clk_put(rot->clock);
err_clk_get:
	free_irq(rot->irq, rot);
err_get_irq:
	iounmap(rot->regs);
err_ioremap:
	release_resource(rot->regs_res);
	kfree(rot->regs_res);
err_get_resource:
	kfree(rot);
	return ret;
}

static int __devexit rotator_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rot_context *rot = dev_get_drvdata(dev);

	exynos_drm_ippdrv_unregister(&rot->ippdrv);

	pm_runtime_disable(dev);
	clk_put(rot->clock);

	free_irq(rot->irq, rot);

	iounmap(rot->regs);

	release_resource(rot->regs_res);
	kfree(rot->regs_res);

	kfree(rot);

	return 0;
}

struct rot_limit_table rot_limit_tbl = {
	.ycbcr420_2p = {
		.min_w = 32,
		.min_h = 32,
		.max_w = SZ_32K,
		.max_h = SZ_32K,
		.align = 3,
	},
	.rgb888 = {
		.min_w = 8,
		.min_h = 8,
		.max_w = SZ_8K,
		.max_h = SZ_8K,
		.align = 2,
	},
};

struct platform_device_id rotator_driver_ids[] = {
	{
		.name		= "exynos-rot",
		.driver_data	= (unsigned long)&rot_limit_tbl,
	},
	{},
};

#ifdef CONFIG_PM_SLEEP
static int rotator_suspend(struct device *dev)
{
	struct rot_context *rot = dev_get_drvdata(dev);

	rot->suspended = true;

	return 0;
}

static int rotator_resume(struct device *dev)
{
	struct rot_context *rot = dev_get_drvdata(dev);

	rot->suspended = false;

	return 0;
}
#endif

static const struct dev_pm_ops rotator_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(rotator_suspend, rotator_resume)
};

struct platform_driver rotator_driver = {
	.probe		= rotator_probe,
	.remove		= __devexit_p(rotator_remove),
	.id_table	= rotator_driver_ids,
	.driver		= {
		.name	= "exynos-rot",
		.owner	= THIS_MODULE,
		.pm	= &rotator_pm_ops,
	},
};
