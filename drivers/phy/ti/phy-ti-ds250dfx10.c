// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the TI DS250DF410 Retimer
 *
 * Copyright (C) 2022-2023 Josua Mayer <josua@solid-run.com>
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>

#define DS250DF410_REG_CHAN_CONFIG_ID 0xEF
#define DS250DF410_MASK_CHAN_CONFIG_ID GENMASK(3, 0)
#define DS250DF410_REG_VERSION 0xF0
#define DS250DF410_REG_DEVICE_ID 0xF1
#define DS250DF410_REG_CHAN_VERSION 0xF3
#define DS250DF410_MASK_CHAN_VERSION GENMASK(7, 4)
#define DS250DF410_MASK_SHARE_VERSION GENMASK(3, 0)

struct ds250dfx10_phy_priv {
	struct i2c_client *client;
	uint8_t channel;
};

struct ds250dfx10_priv {
	struct phy *phy[8];
	struct phy_provider *provider;
};

static int ds250dfx10_read_register(struct i2c_client *client, uint8_t address, uint8_t *value,
									uint8_t mask)
{
	s32 res;

	res = i2c_smbus_read_byte_data(client, address);
	if (res < 0) {
		dev_err(&client->dev, "failed to read register %#04x: %d\n", address,
				res);
		return -EIO;
	}

	*value = res & mask;
	return 0;
}

static int ds250dfx10_write_register(struct i2c_client *client, uint8_t address, uint8_t value,
									 uint8_t mask)
{
	int ret;
	uint8_t tmp;
	s32 res;

	// combine with current value according to mask
	if (mask != 0xFF) {
		ret = ds250dfx10_read_register(client, address, &tmp, ~mask);
		if (ret)
			return ret;

		value = (value & mask) | tmp;
	}

	// write new value
	res = i2c_smbus_write_byte_data(client, address, value);
	if (res < 0) {
		dev_err(&client->dev, "failed to write register %#04x=%#04x: %d\n",
				address, value, res);
		return -EIO;
	}

	return 0;
}

static void ds250dfx10_config_10g(struct i2c_client *client, uint8_t channel)
{
	int ret = 0;

	// enable smbus access to single channel
	ret |= ds250dfx10_write_register(client, 0xFF, 0x01, 0x03);

	// select channel
	ret |= ds250dfx10_write_register(client, 0xFC, 1 << channel, 0xFF);

	// reset channel registers
	ret |= ds250dfx10_write_register(client, 0x00, 0x04, 0x04);

	// assert cdr
	ret |= ds250dfx10_write_register(client, 0x0A, 0x0C, 0x0C);

	// select 10.3125 rate
	ret |= ds250dfx10_write_register(client, 0x2F, 0x00, 0xF0);

	// enable pre- and post-fir
	ret |= ds250dfx10_write_register(client, 0x3D, 0x80, 0x80);

	// set main cursor magnitude +15
	ret |= ds250dfx10_write_register(client, 0x3D, 0x00, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3D, 0x0F, 0x1F);

	// set pre cursor magnitude -4
	ret |= ds250dfx10_write_register(client, 0x3E, 0x40, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3E, 0x04, 0x0F);

	// set post cursor magnitude -4
	ret |= ds250dfx10_write_register(client, 0x3F, 0x40, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3F, 0x04, 0x0F);

	// deassert cdr
	ret |= ds250dfx10_write_register(client, 0x0A, 0x00, 0x0C);

	if (!ret)
		dev_info(&client->dev, "configured channel %u for 10G\n", channel);
}

static void ds250dfx10_config_25g(struct i2c_client *client, uint8_t channel)
{
	int ret = 0;

	// enable smbus access to single channel
	ret |= ds250dfx10_write_register(client, 0xFF, 0x01, 0x03);

	// select channel
	ret |= ds250dfx10_write_register(client, 0xFC, 1 << channel, 0xFF);

	// reset channel registers
	ret |= ds250dfx10_write_register(client, 0x00, 0x04, 0x04);

	// assert cdr
	ret |= ds250dfx10_write_register(client, 0x0A, 0x0C, 0x0C);

	// select 25.78125 rate
	ret |= ds250dfx10_write_register(client, 0x2F, 0x50, 0xF0);

	// enable pre- and post-fir
	ret |= ds250dfx10_write_register(client, 0x3D, 0x80, 0x80);

	// set main cursor magnitude +15
	ret |= ds250dfx10_write_register(client, 0x3D, 0x00, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3D, 0x0F, 0x1F);

	// set pre cursor magnitude -4
	ret |= ds250dfx10_write_register(client, 0x3E, 0x40, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3E, 0x04, 0x0F);

	// set post cursor magnitude -4
	ret |= ds250dfx10_write_register(client, 0x3F, 0x40, 0x40);
	ret |= ds250dfx10_write_register(client, 0x3F, 0x04, 0x0F);

	// deassert cdr
	ret |= ds250dfx10_write_register(client, 0x0A, 0x00, 0x0C);

	if (!ret)
		dev_info(&client->dev, "configured channel %u for 25G\n", channel);
}

static int ds250dfx10_phy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct ds250dfx10_phy_priv *priv = phy_get_drvdata(phy);

	if (mode != PHY_MODE_ETHERNET)
		return -EOPNOTSUPP;

	switch (submode) {
	case PHY_INTERFACE_MODE_10GBASER:
		ds250dfx10_config_10g(priv->client, priv->channel);
		break;
	case PHY_INTERFACE_MODE_25GBASER:
		ds250dfx10_config_25g(priv->client, priv->channel);
		break;
	default:
		dev_err(&priv->client->dev, "unsupported interface submode %i\n",
				  submode);
		return -EOPNOTSUPP;
	}

	return 0;
}

static const struct phy_ops ds250dfx10_phy_ops = {
	.set_mode	= ds250dfx10_phy_set_mode,
	.owner		= THIS_MODULE,
};

static struct phy *ds250dfx10_of_xlate(struct device *dev, struct of_phandle_args *args)
{
	struct ds250dfx10_priv *phy_priv = dev_get_drvdata(dev);
	int channel;

	if (args->args_count != 1) {
		dev_err(phy_priv->provider->dev, "DT did not pass correct no of args\n");
		return ERR_PTR(-ENODEV);
	}

	channel = args->args[0];
	if (WARN_ON(channel >= ARRAY_SIZE(phy_priv->phy))
		|| !phy_priv->phy[channel])
		return ERR_PTR(-ENODEV);

	return phy_priv->phy[channel];
}

static int ds250dfx10_probe(struct i2c_client *client)
{
	struct ds250dfx10_priv *priv;
	struct ds250dfx10_phy_priv *phy_priv;
	struct device_node *child;
	uint8_t chan_config_id, device_id, version, chan_version, share_version, channels;
	uint8_t reg;
	int ret, i;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto no_phys;
	}
	i2c_set_clientdata(client, priv);

	/* read device identification */
	ret = ds250dfx10_read_register(client, DS250DF410_REG_DEVICE_ID, &reg, 0xFF);
	if (ret)
		goto no_phys;
	device_id = reg;

	/* read device version */
	ret = ds250dfx10_read_register(client, DS250DF410_REG_VERSION, &reg, 0xFF);
	if (ret)
		goto no_phys;
	version = reg;

    // report device id
	dev_info(&client->dev, "device id %#04x version %#04x\n", device_id, version);

	switch (device_id) {
	case 0x10:
		break;
	default:
		dev_warn(&client->dev, "unknown device id, expect problems!\n");
	}

	// read channel config id
	ret = ds250dfx10_read_register(client, DS250DF410_REG_CHAN_CONFIG_ID, &reg,
								   DS250DF410_MASK_CHAN_CONFIG_ID);
	if (ret)
		goto no_phys;
	chan_config_id = reg;

	switch (chan_config_id) {
	case 0xC:
		channels = 8;
		break;
	case 0xE:
		channels = 4;
		break;
	default:
		dev_err(&client->dev, "unknown channel configuration id %#03x\n", chan_config_id);
		ret = -EINVAL;
		goto no_phys;
	}
	dev_info(&client->dev, "%u channels\n", channels);

	// read channel version
	ret = ds250dfx10_read_register(client, DS250DF410_REG_CHAN_VERSION, &reg, 0xFF);
	if (ret)
		goto no_phys;
	chan_version = (reg & DS250DF410_MASK_CHAN_VERSION) >> 4;
	share_version = reg & DS250DF410_MASK_SHARE_VERSION;

	dev_info(&client->dev, "channel version %#03x share version %#03x\n",
			 chan_version, share_version);

	// create PHY objects for all channels
	for (i = 0; i < channels; i++) {
		priv->phy[i] = devm_phy_create(&client->dev, child, &ds250dfx10_phy_ops);
		if (IS_ERR(priv->phy[i])) {
			ret = PTR_ERR(priv->phy[i]);
			priv->phy[i] = NULL;
			of_node_put(child);
			goto no_provider;
		}

		phy_priv = devm_kzalloc(&client->dev, sizeof(*phy_priv), GFP_KERNEL);
		if (!phy_priv) {
			ret = -ENOMEM;
			goto no_provider;
		}
		phy_set_drvdata(priv->phy[i], phy_priv);

		phy_priv->client = client;
		phy_priv->channel = i;

		dev_info(&client->dev, "created phy for channel %u\n", i);
	}
	of_node_put(child);

	// register self as phy provider with generic lookup function
	priv->provider = devm_of_phy_provider_register(&client->dev, ds250dfx10_of_xlate);

	return 0;

	devm_of_phy_provider_unregister(&client->dev, priv->provider);
no_provider:
	for (i = 0; i < 8; i++) {
		if (priv->phy[i])
			devm_phy_destroy(&client->dev, priv->phy[i]);
	}
no_phys:
	return ret;
}

static int ds250dfx10_remove(struct i2c_client *client)
{
	struct ds250dfx10_priv *priv = i2c_get_clientdata(client);
	int i;

	for (i = 0; i < 8; i++)
		if (priv->phy[i])
			devm_phy_destroy(&client->dev, priv->phy[i]);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id ds250dfx10_dt_ids[] = {
	{ .compatible = "ti,ds250df410", },
	{ .compatible = "ti,ds250df810", },
	{ }
};
MODULE_DEVICE_TABLE(of, ds250dfx10_dt_ids);
#endif

static struct i2c_device_id ds250dfx10_idtable[] = {
	{ "ds250df410", 0 },
	{ "ds250df810", 1 },
	{ }
};

MODULE_DEVICE_TABLE(i2c, ds250dfx10_idtable);

static struct i2c_driver ds250dfx10_driver = {
	.driver = {
		.name   = "ds250dfx10",
		.of_match_table = of_match_ptr(ds250dfx10_dt_ids),
	},

	.id_table       = ds250dfx10_idtable,
	.probe_new      = ds250dfx10_probe,
	.remove         = ds250dfx10_remove,
};

module_i2c_driver(ds250dfx10_driver);

MODULE_AUTHOR("Josua Mayer <josua@solid-run.com>");
MODULE_DESCRIPTION("TI DS250DFX10 Retimer Driver");
MODULE_LICENSE("GPL");
