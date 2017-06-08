/*
 * Driver for Hisilicon Djtag r/w via System Controller.
 *
 * Copyright (C) 2013-2014 Hisilicon Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/platform_data/hisi-djtag.h>
#include <linux/bitops.h>

#include <asm/cacheflush.h>

#define SC_DJTAG_TIMEOUT		1000000	/* step: udelay(1) 1s */
#define SC_DJTAG_RESET			80000	/* step: udelay(100) 8s */

/* for 660 and 1382 totem */
#define SC_DJTAG_MSTR_EN		0x6800
#define DJTAG_NOR_CFG			BIT(1)	/* accelerate R,W */
#define DJTAG_MSTR_EN			BIT(0)
#define SC_DJTAG_MSTR_START_EN		0x6804
#define DJTAG_MSTR_START_EN		0x1
#define SC_DJTAG_DEBUG_MODULE_SEL	0x680c
#define SC_DJTAG_MSTR_WR		0x6810
#define DJTAG_MSTR_W			0x1
#define DJTAG_MSTR_R			0x0
#define SC_DJTAG_CHAIN_UNIT_CFG_EN	0x6814
#define CHAIN_UNIT_CFG_EN		0xFFFF
#define SC_DJTAG_MSTR_ADDR		0x6818
#define SC_DJTAG_MSTR_DATA		0x681c
#define SC_DJTAG_RD_DATA_BASE		0xe800
#define SC_DJTAG_OP_ST			0xe828
#define DJTAG_OP_DONE			BIT(8)

/* for 1382 totem and io */
#define SC_DJTAG_SEC_ACC_EN_EX		0xd800
#define DJTAG_SEC_ACC_EN_EX		0x1
#define SC_DJTAG_MSTR_CFG_EX		0xd818
#define DJTAG_MSTR_RW_SHIFT_EX		29
#define DJTAG_MSTR_RD_EX		(0x0 << DJTAG_MSTR_RW_SHIFT_EX)
#define DJTAG_MSTR_WR_EX		(0x1 << DJTAG_MSTR_RW_SHIFT_EX)
#define DEBUG_MODULE_SEL_SHIFT_EX	16
#define CHAIN_UNIT_CFG_EN_EX		0xFFFF
#define SC_DJTAG_MSTR_ADDR_EX		0xd810
#define SC_DJTAG_MSTR_DATA_EX		0xd814
#define SC_DJTAG_MSTR_START_EN_EX	0xd81c
#define DJTAG_MSTR_START_EN_EX		0x1
#define SC_DJTAG_RD_DATA_BASE_EX	0xe800
#define SC_DJTAG_OP_ST_EX		0xe828
#define DJTAG_OP_DONE_EX		BIT(8)

static LIST_HEAD(djtag_list);

enum hisi_soc_type {
	HISI_HIP05,
	HISI_HIP06,
	HISI_HI1382,
};

struct djtag_of_data {
	struct list_head list;
	struct regmap *scl_map;
	struct device_node *node;
	enum hisi_soc_type type;
	int (*mod_cfg_by_djtag)(struct regmap *map, u32 offset,
			u32 mod_sel, u32 mod_mask, bool is_w,
			u32 wval, int chain_id, u32 *rval);
	int (*djtag_request)(struct regmap *map);
	int (*djtag_release)(struct regmap *map);
};

static DEFINE_SPINLOCK(djtag_lock);

static int hip05_djtag_request(struct regmap *map)
{
	int i, flag, ret;
	u32 rd;

	/* djtag is available or not, and then request it. */
	flag = 0;
	for (i = 0; i < SC_DJTAG_RESET; i++) {
		ret = regmap_read(map, SC_DJTAG_DEBUG_MODULE_SEL, &rd);
		if (ret)
			return ret;

		/* First, reg value is 0x3A means available, otherwise
		   means busy. Then write 0x3B to lock if available. */
		if (!flag && rd == 0x3A) {
			ret = regmap_write(map, SC_DJTAG_DEBUG_MODULE_SEL,
					0x3B);
			if (ret)
				return ret;
			udelay(10);
			flag = 1;
			continue;
		}

		if (flag) {
			if (rd == 0x3B)
				return 0;
			flag = 0;
		}
		udelay(100);
	}

	/* If djtag is busy for 8s, it means deadlocked, so reset it. */
	pr_err("djtag: djtag is being used by other devices!\n");
	pr_info("djtag: (wait > 8s) djtag reset!\n");
	ret = regmap_write(map, SC_DJTAG_DEBUG_MODULE_SEL, 0x3A);

	return ret ? : -EAGAIN;
}

static int hip05_djtag_release(struct regmap *map)
{
	return regmap_write(map, SC_DJTAG_DEBUG_MODULE_SEL, 0x3A);
}

static int hi1382_djtag_request(struct regmap *map)
{
	int i, flag, ret;
	u32 rd;

	/* djtag is available or not, and then request it. */
	flag = 0;
	for (i = 0; i < SC_DJTAG_RESET; i++) {
		ret = regmap_read(map, SC_DJTAG_MSTR_CFG_EX, &rd);
		if (ret)
			return ret;

		rd = (rd >> DEBUG_MODULE_SEL_SHIFT_EX) & 0xFF;

		/* First, reg value is 0xAA means available, otherwise
		   means busy. Then write 0xAB to lock if available. */
		if (!flag && rd  == 0xAA) {
			ret = regmap_write(map, SC_DJTAG_MSTR_CFG_EX,
					0xAB << DEBUG_MODULE_SEL_SHIFT_EX);
			if (ret)
				return ret;
			udelay(10);
			flag = 1;
			continue;
		}

		if (flag) {
			if (rd == 0xAB)
				return 0;
			flag = 0;
		}
		udelay(100);
	}

	/* If djtag is busy for 8s, it means deadlocked, so reset it. */
	pr_err("djtag: djtag is being blocked by other devices!\n");
	pr_info("djtag: (wait > 8s) djtag reset!\n");
	ret = regmap_write(map, SC_DJTAG_MSTR_CFG_EX,
			0xAA << DEBUG_MODULE_SEL_SHIFT_EX);

	return ret ? : -EAGAIN;
}

static int hi1382_djtag_release(struct regmap *map)
{
	return regmap_write(map, SC_DJTAG_MSTR_CFG_EX,
			0xAA << DEBUG_MODULE_SEL_SHIFT_EX);
}

/**
 * mod_cfg_by_djtag: cfg mode via djtag
 * @node:	djtag node
 * @offset:	register's offset
 * @mod_sel:	module selection
 * @mod_mask:	mask to select specific modules
 * @is_w:	write -> true, read -> false
 * @wval:	value to write to register
 * @chain_id:	read value of which module
 * @rval:	value which read from register
 *
 * Return NULL if error, else return pointer of regmap.
 */
static int hip05_mod_cfg_by_djtag(struct regmap *map,
		u32 offset, u32 mod_sel,
		u32 mod_mask, bool is_w,
		u32 wval, int chain_id,
		u32 *rval)
{
	u32 rd;
	int ret, timeout;

	BUG_ON(!map);

	if (!(mod_mask & CHAIN_UNIT_CFG_EN))
		mod_mask = CHAIN_UNIT_CFG_EN;

	/* djtag mster enable & accelerate R,W */
	ret = regmap_write(map, SC_DJTAG_MSTR_EN,
			DJTAG_NOR_CFG | DJTAG_MSTR_EN);
	if (ret)
		goto err;

	/* select module */
	ret = regmap_write(map, SC_DJTAG_DEBUG_MODULE_SEL,
			mod_sel);
	if (ret)
		goto err;

	ret = regmap_write(map, SC_DJTAG_CHAIN_UNIT_CFG_EN,
			mod_mask & CHAIN_UNIT_CFG_EN);
	if (ret)
		goto err;

	if (is_w) {
		ret = regmap_write(map, SC_DJTAG_MSTR_WR,
				DJTAG_MSTR_W);
		if (ret)
			goto err;

		ret = regmap_write(map, SC_DJTAG_MSTR_DATA,
				wval);
		if (ret)
			goto err;
	} else {
		ret = regmap_write(map, SC_DJTAG_MSTR_WR,
				DJTAG_MSTR_R);
		if (ret)
			goto err;
	}

	/* address offset */
	ret = regmap_write(map, SC_DJTAG_MSTR_ADDR, offset);
	if (ret)
		goto err;

	/* start to write to djtag register */
	timeout = SC_DJTAG_TIMEOUT;
	ret = regmap_write(map, SC_DJTAG_MSTR_START_EN,
			DJTAG_MSTR_START_EN);
	if (ret)
		goto err;

	do {
		ret = regmap_read(map, SC_DJTAG_MSTR_START_EN,
				&rd);
		if (ret)
			goto err;

		if (!(rd & DJTAG_MSTR_EN))
			break;

		udelay(1);
	} while (timeout--);

	if (timeout < 0)
		goto timeout;

	/* ensure the djtag read register is filled */
	timeout = SC_DJTAG_TIMEOUT;
	do {
		ret = regmap_read(map, SC_DJTAG_OP_ST_EX, &rd);
		if (ret)
			goto err;

		if (rd & DJTAG_OP_DONE_EX)
			break;

		udelay(1);
	} while (timeout--);

	if (timeout < 0)
		goto timeout;

	if (!is_w) {
		ret = regmap_read(map, SC_DJTAG_RD_DATA_BASE
				+ chain_id * 0x4, rval);
		if (ret)
			goto err;
	}

	goto out;

timeout:
	pr_err("djtag: %s timeout!\n", is_w ? "write" : "read");

	ret = -EBUSY;
err:
	pr_err("djtag: regmap_read/write error %d.\n", ret);
out:
	return ret;
}

static int hi1382_mod_cfg_by_djtag(struct regmap *map,
		u32 offset, u32 mod_sel,
		u32 mod_mask, bool is_w,
		u32 wval, int chain_id,
		u32 *rval)
{
	u32 rd;
	int ret, timeout = SC_DJTAG_TIMEOUT;

	BUG_ON(!map);

	if (!(mod_mask & CHAIN_UNIT_CFG_EN_EX))
		mod_mask = CHAIN_UNIT_CFG_EN_EX;

	/* djtag mster enable */
	ret = regmap_write(map, SC_DJTAG_SEC_ACC_EN_EX,
				DJTAG_SEC_ACC_EN_EX);
	if (ret)
		goto err;

	if (is_w) {
		ret = regmap_write(map, SC_DJTAG_MSTR_CFG_EX,
					DJTAG_MSTR_WR_EX | (mod_sel
					<< DEBUG_MODULE_SEL_SHIFT_EX)
					| (mod_mask
					& CHAIN_UNIT_CFG_EN_EX));
		if (ret)
			goto err;

		ret = regmap_write(map, SC_DJTAG_MSTR_DATA_EX,
					wval);
		if (ret)
			goto err;
	} else {
		ret = regmap_write(map, SC_DJTAG_MSTR_CFG_EX,
					DJTAG_MSTR_RD_EX | (mod_sel
					<< DEBUG_MODULE_SEL_SHIFT_EX)
					| (mod_mask
					& CHAIN_UNIT_CFG_EN_EX));
		if (ret)
			goto err;
	}

	/* address offset */
	ret = regmap_write(map, SC_DJTAG_MSTR_ADDR_EX, offset);
	if (ret)
		goto err;

	/* start to write to djtag register */
	ret = regmap_write(map, SC_DJTAG_MSTR_START_EN_EX,
				DJTAG_MSTR_START_EN_EX);
	if (ret)
		goto err;

	/* ensure the djtag operation is done */
	do {
		ret = regmap_read(map, SC_DJTAG_MSTR_START_EN_EX,
					&rd);
		if (ret)
			goto err;

		if (!(rd & DJTAG_MSTR_START_EN_EX))
			break;

		udelay(1);
	} while (timeout--);

	if (timeout < 0)
		goto timeout;

	/* ensure the djtag read register is filled */
	timeout = SC_DJTAG_TIMEOUT;
	do {
		ret = regmap_read(map, SC_DJTAG_OP_ST_EX, &rd);
		if (ret)
			goto err;

		if (rd & DJTAG_OP_DONE_EX)
			break;

		udelay(1);
	} while (timeout--);

	if (timeout < 0)
		goto timeout;

	if (!is_w) {
		ret = regmap_read(map, SC_DJTAG_RD_DATA_BASE_EX
				+ chain_id * 0x4, rval);
		if (ret)
			goto err;
	}

	goto out;

timeout:
	ret = -EBUSY;
	pr_err("djtag: %s timeout!\n", is_w ? "write" : "read");
err:
	pr_err("djtag: regmap_read/write error %d.\n", ret);
out:
	return ret;
}


/**
 * djtag_writel - write registers via djtag
 * @node:	djtag node
 * @offset:	register's offset
 * @mod_sel:	module selection
 * @mod_mask:	mask to select specific modules
 * @val:	value to write to register
 *
 * If error return errno, otherwise return 0.
 */
int djtag_writel(struct device_node *node, u32 offset,
		u32 mod_sel, u32 mod_mask, u32 val)
{
	struct regmap *map;
	unsigned long flags;
	struct djtag_of_data *tmp, *p;
	int ret;

	ret = -ENODEV;
	list_for_each_entry_safe(tmp, p, &djtag_list, list) {
		if (tmp->node == node) {
			map = tmp->scl_map;

			spin_lock_irqsave(&djtag_lock, flags);

			if (tmp->djtag_request) {
				ret = tmp->djtag_request(map);
				if (ret)
					goto unlock;
			}

			ret = tmp->mod_cfg_by_djtag(map, offset, mod_sel,
							mod_mask, true,
							val, 0, NULL);
			if (ret)
				pr_err("djtag_writel: %s: error %d!\n",
						node->full_name, ret);

			if (tmp->djtag_release && tmp->djtag_release(map))
				ret = ret ? : -EIO;

			goto unlock;
		}
	}

	goto out;

unlock:
	spin_unlock_irqrestore(&djtag_lock, flags);
out:
	return ret;
}
EXPORT_SYMBOL_GPL(djtag_writel);

/**
 * djtag_readl - read registers via djtag
 * @node:	djtag node
 * @offset:	register's offset
 * @mod_sel:	module type selection
 * @chain_id:	chain_id number, mostly is 0
 * @val:	register's value
 *
 * If error return errno, otherwise return 0.
 */
int djtag_readl(struct device_node *node, u32 offset,
		u32 mod_sel, int chain_id, u32 *val)
{
	struct regmap *map;
	unsigned long flags;
	struct djtag_of_data *tmp, *p;
	int ret;

	ret = -ENODEV;
	list_for_each_entry_safe(tmp, p, &djtag_list, list) {
		if (tmp->node == node) {
			map = tmp->scl_map;

			spin_lock_irqsave(&djtag_lock, flags);

			if (tmp->djtag_request) {
				ret = tmp->djtag_request(map);
				if (ret)
					goto unlock;
			}

			ret = tmp->mod_cfg_by_djtag(map, offset, mod_sel,
							0, false, 0,
							chain_id, val);
			if (ret)
				pr_err("djtag_readl: %s: error %d!\n",
						node->full_name, ret);

			if (tmp->djtag_release && tmp->djtag_release(map))
				ret = ret ? : -EIO;

			goto unlock;
		}
	}

	goto out;

unlock:
	spin_unlock_irqrestore(&djtag_lock, flags);
out:
	return ret;
}
EXPORT_SYMBOL_GPL(djtag_readl);

static const struct of_device_id djtag_of_match[] = {
	/* for 660 and 1610 totem */
	{ .compatible = "hisilicon,hip05-djtag", .data = (void *)HISI_HIP05 },
	/* for 1382 totem and io */
	{ .compatible = "hisilicon,hi1382-djtag", .data = (void *)HISI_HI1382 },
	{},
};

MODULE_DEVICE_TABLE(of, djtag_of_match);

static int djtag_dev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct djtag_of_data *dg_data;
	const struct of_device_id *of_id;
	struct device_node *scl_node;

	of_id = of_match_device(djtag_of_match, dev);
	if (!of_id)
		return -EINVAL;

	dg_data = kzalloc(sizeof(struct djtag_of_data), GFP_KERNEL);
	if (dg_data == NULL)
		return -ENOMEM;

	dg_data->node = dev->of_node;
	dg_data->type = (enum hisi_soc_type)of_id->data;
	if (dg_data->type == HISI_HI1382)
		dg_data->mod_cfg_by_djtag = hi1382_mod_cfg_by_djtag;
	else if (dg_data->type == HISI_HIP05)
		dg_data->mod_cfg_by_djtag = hip05_mod_cfg_by_djtag;
	else {
		dev_err(dev, "djtag configure error.\n");
		kfree(dg_data);
		return -EINVAL;
	}

	INIT_LIST_HEAD(&dg_data->list);
	scl_node = of_parse_phandle(dev->of_node, "syscon", 0);
	if (!scl_node) {
		dev_warn(dev, "no hisilicon syscon.\n");
		kfree(dg_data);
		return -EINVAL;
	}

	dg_data->scl_map = syscon_node_to_regmap(scl_node);
	if (IS_ERR(dg_data->scl_map)) {
		dev_warn(dev, "wrong syscon register address.\n");
		kfree(dg_data);
		return -EINVAL;
	}

	list_add_tail(&dg_data->list, &djtag_list);

	/* djtag init */
	if (of_property_read_bool(dev->of_node,
				"djtag-mutex-access-protection")) {
		if (dg_data->type == HISI_HIP05) {
			regmap_write(dg_data->scl_map,
				SC_DJTAG_DEBUG_MODULE_SEL, 0x3A);

			dg_data->djtag_request = hip05_djtag_request;
			dg_data->djtag_release = hip05_djtag_release;
		} else {
			regmap_write(dg_data->scl_map, SC_DJTAG_MSTR_CFG_EX,
				0xAA << DEBUG_MODULE_SEL_SHIFT_EX);

			dg_data->djtag_request = hi1382_djtag_request;
			dg_data->djtag_release = hi1382_djtag_release;
		}
	}

	dev_info(dev, "%s init successfully.\n", dg_data->node->name);
	return 0;
}

static int djtag_dev_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct djtag_of_data *tmp, *p;

	list_for_each_entry_safe(tmp, p, &djtag_list, list) {
		list_del(&tmp->list);
		dev_info(dev, "%s remove successfully.\n",
						tmp->node->name);
		kfree(tmp);
	}

	return 0;
}

static struct platform_driver djtag_dev_driver = {
	.driver = {
		.name = "hisi-djtag",
		.of_match_table = djtag_of_match,
	},
	.probe = djtag_dev_probe,
	.remove = djtag_dev_remove,
};

static int __init djtag_dev_init(void)
{
	return platform_driver_register(&djtag_dev_driver);
}

static void __exit djtag_dev_exit(void)
{
	platform_driver_unregister(&djtag_dev_driver);
}

arch_initcall_sync(djtag_dev_init);
module_exit(djtag_dev_exit);

MODULE_DESCRIPTION("Hisilicon djtag driver");
MODULE_AUTHOR("Xiaojun Tan");
MODULE_LICENSE("GPL");
MODULE_VERSION("V1R1");
