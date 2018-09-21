/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2016 Pinecone Inc.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/rpmsg.h>

#define RPMSG_CLK_ENABLE	0
#define RPMSG_CLK_DISABLE	1
#define RPMSG_CLK_SETRATE	2
#define RPMSG_CLK_SETPHASE	3
#define RPMSG_CLK_GETPHASE	4
#define RPMSG_CLK_GETRATE	5
#define RPMSG_CLK_ROUNDRATE	6
#define RPMSG_CLK_ISENABLED	7

struct rpmsg_clk_header {
	uint32_t		command;
	uint32_t		response;
	int64_t			result;
	uint64_t		cookie;
} __packed;

struct rpmsg_clk_enable {
	struct rpmsg_clk_header	header;
	char			name[0];
} __packed;

#define rpmsg_clk_disable	rpmsg_clk_enable
#define rpmsg_clk_isenabled	rpmsg_clk_enable

struct rpmsg_clk_setrate {
	struct rpmsg_clk_header	header;
	uint64_t		rate;
	char			name[0];
} __packed;

#define rpmsg_clk_getrate	rpmsg_clk_enable
#define rpmsg_clk_roundrate	rpmsg_clk_setrate

struct rpmsg_clk_setphase {
	struct rpmsg_clk_header	header;
	uint32_t		degrees;
	char			name[0];
} __packed;

#define rpmsg_clk_getphase	rpmsg_clk_enable

struct rpmsg_clk_res {
	struct clk		*clk;
	atomic_t		count;
};

static void rpmsg_clk_release(struct device *dev, void *res)
{
	struct rpmsg_clk_res *clkres = res;
	int count = atomic_read(&clkres->count);

	while (count-- > 0)
		clk_disable_unprepare(clkres->clk);

	clk_put(clkres->clk);
}

static int rpmsg_clk_match(struct device *dev, void *res, void *data)
{
	struct rpmsg_clk_res *clkres = res;

	return !strcmp(__clk_get_name(clkres->clk), data);
}

static struct rpmsg_clk_res *rpmsg_clk_get_res(struct rpmsg_device *rpdev, const char *name)
{
	struct rpmsg_clk_res *clkres;
	struct clk *clk;

	clkres = devres_find(&rpdev->dev, rpmsg_clk_release, rpmsg_clk_match, (void *)name);
	if (clkres)
		return clkres;

	clkres = devres_alloc(rpmsg_clk_release, sizeof(*clkres), GFP_KERNEL);
	if (!clkres)
		return ERR_PTR(-ENOMEM);

	clk = clk_get(&rpdev->dev, name);
	if (!IS_ERR(clk)) {
		clkres->clk = clk;
		atomic_set(&clkres->count, 0);
		devres_add(&rpdev->dev, clkres);
		return clkres;
	} else {
		devres_free(clkres);
		return ERR_CAST(clk);
	}
}

static int rpmsg_clk_enable_handler(struct rpmsg_device *rpdev,
				    void *data, int len, void *priv, u32 src)
{
	struct rpmsg_clk_enable *msg = data;
	struct rpmsg_clk_res *clkres = rpmsg_clk_get_res(rpdev, msg->name);

	if (!IS_ERR(clkres)) {
		msg->header.result = clk_prepare_enable(clkres->clk);
		if (!msg->header.result) {
			atomic_inc(&clkres->count);
		}
	} else
		msg->header.result = PTR_ERR(clkres);

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_clk_disable_handler(struct rpmsg_device *rpdev,
				     void *data, int len, void *priv, u32 src)
{
	struct rpmsg_clk_disable *msg = data;
	struct rpmsg_clk_res *clkres = rpmsg_clk_get_res(rpdev, msg->name);

	if (!IS_ERR(clkres)) {
		msg->header.result = 0;
		if (atomic_dec_return(&clkres->count) >= 0)
			clk_disable_unprepare(clkres->clk);
		else
			atomic_inc(&clkres->count);
	} else
		msg->header.result = PTR_ERR(clkres);

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_clk_getrate_handler(struct rpmsg_device *rpdev,
				     void *data, int len, void *priv, u32 src)
{
	struct rpmsg_clk_getrate *msg = data;
	struct rpmsg_clk_res *clkres = rpmsg_clk_get_res(rpdev, msg->name);

	if (!IS_ERR(clkres))
		msg->header.result = clk_get_rate(clkres->clk);
	else
		msg->header.result = PTR_ERR(clkres);

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_clk_roundrate_handler(struct rpmsg_device *rpdev,
				       void *data, int len, void *priv, u32 src)
{
	struct rpmsg_clk_roundrate *msg = data;
	struct rpmsg_clk_res *clkres = rpmsg_clk_get_res(rpdev, msg->name);

	if (!IS_ERR(clkres))
		msg->header.result = clk_round_rate(clkres->clk, msg->rate);
	else
		msg->header.result = PTR_ERR(clkres);

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_clk_setrate_handler(struct rpmsg_device *rpdev,
				     void *data, int len, void *priv, u32 src)
{
	struct rpmsg_clk_setrate *msg = data;
	struct rpmsg_clk_res *clkres = rpmsg_clk_get_res(rpdev, msg->name);

	if (!IS_ERR(clkres))
		msg->header.result = clk_set_rate(clkres->clk, msg->rate);
	else
		msg->header.result = PTR_ERR(clkres);

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_clk_setphase_handler(struct rpmsg_device *rpdev,
				      void *data, int len, void *priv, u32 src)
{
	struct rpmsg_clk_setphase *msg = data;
	struct rpmsg_clk_res *clkres = rpmsg_clk_get_res(rpdev, msg->name);

	if (!IS_ERR(clkres))
		msg->header.result = clk_set_phase(clkres->clk, msg->degrees);
	else
		msg->header.result = PTR_ERR(clkres);

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_clk_getphase_handler(struct rpmsg_device *rpdev,
				      void *data, int len, void *priv, u32 src)
{
	struct rpmsg_clk_getphase *msg = data;
	struct rpmsg_clk_res *clkres = rpmsg_clk_get_res(rpdev, msg->name);

	if (!IS_ERR(clkres))
		msg->header.result = clk_get_phase(clkres->clk);
	else
		msg->header.result = PTR_ERR(clkres);

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static int rpmsg_clk_isenabled_handler(struct rpmsg_device *rpdev,
				       void *data, int len, void *priv, u32 src)
{
	struct rpmsg_clk_isenabled *msg = data;
	struct rpmsg_clk_res *clkres = rpmsg_clk_get_res(rpdev, msg->name);

	if (!IS_ERR(clkres)) {
		msg->header.result = __clk_get_enable_count(clkres->clk);
		if (msg->header.result == 0)
			msg->header.result = __clk_is_enabled(clkres->clk);
	} else
		msg->header.result = PTR_ERR(clkres);

	return rpmsg_send(rpdev->ept, msg, sizeof(*msg));
}

static const rpmsg_rx_cb_t rpmsg_clk_handler[] = {
	[RPMSG_CLK_ENABLE]    = rpmsg_clk_enable_handler,
	[RPMSG_CLK_DISABLE]   = rpmsg_clk_disable_handler,
	[RPMSG_CLK_SETRATE]   = rpmsg_clk_setrate_handler,
	[RPMSG_CLK_SETPHASE]  = rpmsg_clk_setphase_handler,
	[RPMSG_CLK_GETPHASE]  = rpmsg_clk_getphase_handler,
	[RPMSG_CLK_GETRATE]   = rpmsg_clk_getrate_handler,
	[RPMSG_CLK_ROUNDRATE] = rpmsg_clk_roundrate_handler,
	[RPMSG_CLK_ISENABLED] = rpmsg_clk_isenabled_handler,
};

static int rpmsg_clk_callback(struct rpmsg_device *rpdev,
			      void *data, int len, void *priv, u32 src)
{
	struct rpmsg_clk_header *hdr = data;
	uint32_t cmd = hdr->command;
	int ret = -EINVAL;

	if ((cmd < ARRAY_SIZE(rpmsg_clk_handler)) && rpmsg_clk_handler[cmd]) {
		hdr->response = 1;
		ret = rpmsg_clk_handler[cmd](rpdev, data, len, priv, src);
	} else
		dev_err(&rpdev->dev, "invalid command %u\n", cmd);

	return ret;
}

static int rpmsg_clk_probe(struct rpmsg_device *rpdev)
{
	return 0;
}

static void rpmsg_clk_remove(struct rpmsg_device *rpdev)
{
}

static const struct rpmsg_device_id rpmsg_clk_id_table[] = {
	{ .name = "rpmsg-clk" },
	{ },
};
MODULE_DEVICE_TABLE(rpmsg, rpmsg_clk_id_table);

static struct rpmsg_driver rpmsg_clk_driver = {
	.drv = {
		.name	= "rpmsg_clk",
		.owner	= THIS_MODULE,
	},

	.id_table	= rpmsg_clk_id_table,
	.probe		= rpmsg_clk_probe,
	.callback	= rpmsg_clk_callback,
	.remove		= rpmsg_clk_remove,
};

module_driver(rpmsg_clk_driver,
		register_rpmsg_driver,
		unregister_rpmsg_driver);

MODULE_ALIAS("rpmsg:rpmsg_clk");
MODULE_AUTHOR("Yanlin Zhu <zhuyanlin@xiaomi.com>");
MODULE_DESCRIPTION("rpmsg clock API redirection driver");
MODULE_LICENSE("GPL v2");
