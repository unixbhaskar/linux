// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dwmac-sti.c - STMicroelectronics DWMAC Specific Glue layer
 *
 * Copyright (C) 2003-2014 STMicroelectronics (R&D) Limited
 * Author: Srinivas Kandagatla <srinivas.kandagatla@st.com>
 * Contributors: Giuseppe Cavallaro <peppe.cavallaro@st.com>
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/stmmac.h>
#include <linux/phy.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_net.h>

#include "stmmac_platform.h"

#define DWMAC_50MHZ	50000000

#define IS_PHY_IF_MODE_GBIT(iface)	(phy_interface_mode_is_rgmii(iface) || \
					 iface == PHY_INTERFACE_MODE_GMII)

/* STiH4xx register definitions (STiH407/STiH410 families)
 *
 * Below table summarizes the clock requirement and clock sources for
 * supported phy interface modes with link speeds.
 * ________________________________________________
 *|  PHY_MODE	| 1000 Mbit Link | 100 Mbit Link   |
 * ------------------------------------------------
 *|	MII	|	n/a	 |	25Mhz	   |
 *|		|		 |	txclk	   |
 * ------------------------------------------------
 *|	GMII	|     125Mhz	 |	25Mhz	   |
 *|		|  clk-125/txclk |	txclk	   |
 * ------------------------------------------------
 *|	RGMII	|     125Mhz	 |	25Mhz	   |
 *|		|  clk-125/txclk |	clkgen     |
 *|		|    clkgen	 |		   |
 * ------------------------------------------------
 *|	RMII	|	n/a	 |	25Mhz	   |
 *|		|		 |clkgen/phyclk-in |
 * ------------------------------------------------
 *
 *	  Register Configuration
 *-------------------------------
 * src	 |BIT(8)| BIT(7)| BIT(6)|
 *-------------------------------
 * txclk |   0	|  n/a	|   1	|
 *-------------------------------
 * ck_125|   0	|  n/a	|   0	|
 *-------------------------------
 * phyclk|   1	|   0	|  n/a	|
 *-------------------------------
 * clkgen|   1	|   1	|  n/a	|
 *-------------------------------
 */

#define STIH4XX_RETIME_SRC_MASK			GENMASK(8, 6)
#define STIH4XX_ETH_SEL_TX_RETIME_CLK		BIT(8)
#define STIH4XX_ETH_SEL_INTERNAL_NOTEXT_PHYCLK	BIT(7)
#define STIH4XX_ETH_SEL_TXCLK_NOT_CLK125	BIT(6)

#define ENMII_MASK	GENMASK(5, 5)
#define ENMII		BIT(5)
#define EN_MASK		GENMASK(1, 1)
#define EN		BIT(1)

/*
 * 3 bits [4:2]
 *	000-GMII/MII
 *	001-RGMII
 *	010-SGMII
 *	100-RMII
 */
#define MII_PHY_SEL_MASK	GENMASK(4, 2)
#define ETH_PHY_SEL_RMII	BIT(4)
#define ETH_PHY_SEL_SGMII	BIT(3)
#define ETH_PHY_SEL_RGMII	BIT(2)
#define ETH_PHY_SEL_GMII	0x0
#define ETH_PHY_SEL_MII		0x0

struct sti_dwmac {
	phy_interface_t interface;	/* MII interface */
	bool ext_phyclk;	/* Clock from external PHY */
	u32 tx_retime_src;	/* TXCLK Retiming*/
	struct clk *clk;	/* PHY clock */
	u32 ctrl_reg;		/* GMAC glue-logic control register */
	int clk_sel_reg;	/* GMAC ext clk selection register */
	struct regmap *regmap;
	bool gmac_en;
	int speed;
	void (*fix_retime_src)(void *priv, int speed, unsigned int mode);
};

struct sti_dwmac_of_data {
	void (*fix_retime_src)(void *priv, int speed, unsigned int mode);
};

static u32 phy_intf_sels[] = {
	[PHY_INTERFACE_MODE_MII] = ETH_PHY_SEL_MII,
	[PHY_INTERFACE_MODE_GMII] = ETH_PHY_SEL_GMII,
	[PHY_INTERFACE_MODE_RGMII] = ETH_PHY_SEL_RGMII,
	[PHY_INTERFACE_MODE_RGMII_ID] = ETH_PHY_SEL_RGMII,
	[PHY_INTERFACE_MODE_SGMII] = ETH_PHY_SEL_SGMII,
	[PHY_INTERFACE_MODE_RMII] = ETH_PHY_SEL_RMII,
};

enum {
	TX_RETIME_SRC_NA = 0,
	TX_RETIME_SRC_TXCLK = 1,
	TX_RETIME_SRC_CLK_125,
	TX_RETIME_SRC_PHYCLK,
	TX_RETIME_SRC_CLKGEN,
};

static u32 stih4xx_tx_retime_val[] = {
	[TX_RETIME_SRC_TXCLK] = STIH4XX_ETH_SEL_TXCLK_NOT_CLK125,
	[TX_RETIME_SRC_CLK_125] = 0x0,
	[TX_RETIME_SRC_PHYCLK] = STIH4XX_ETH_SEL_TX_RETIME_CLK,
	[TX_RETIME_SRC_CLKGEN] = STIH4XX_ETH_SEL_TX_RETIME_CLK
				 | STIH4XX_ETH_SEL_INTERNAL_NOTEXT_PHYCLK,
};

static void stih4xx_fix_retime_src(void *priv, int spd, unsigned int mode)
{
	struct sti_dwmac *dwmac = priv;
	u32 src = dwmac->tx_retime_src;
	u32 reg = dwmac->ctrl_reg;
	long freq = 0;

	if (dwmac->interface == PHY_INTERFACE_MODE_MII) {
		src = TX_RETIME_SRC_TXCLK;
	} else if (dwmac->interface == PHY_INTERFACE_MODE_RMII) {
		if (dwmac->ext_phyclk) {
			src = TX_RETIME_SRC_PHYCLK;
		} else {
			src = TX_RETIME_SRC_CLKGEN;
			freq = DWMAC_50MHZ;
		}
	} else if (phy_interface_mode_is_rgmii(dwmac->interface)) {
		/* On GiGa clk source can be either ext or from clkgen */
		freq = rgmii_clock(spd);

		if (spd != SPEED_1000 && freq > 0)
			/* Switch to clkgen for these speeds */
			src = TX_RETIME_SRC_CLKGEN;
	}

	if (src == TX_RETIME_SRC_CLKGEN && freq > 0)
		clk_set_rate(dwmac->clk, freq);

	regmap_update_bits(dwmac->regmap, reg, STIH4XX_RETIME_SRC_MASK,
			   stih4xx_tx_retime_val[src]);
}

static int sti_dwmac_set_mode(struct sti_dwmac *dwmac)
{
	struct regmap *regmap = dwmac->regmap;
	int iface = dwmac->interface;
	u32 reg = dwmac->ctrl_reg;
	u32 val;

	if (dwmac->gmac_en)
		regmap_update_bits(regmap, reg, EN_MASK, EN);

	regmap_update_bits(regmap, reg, MII_PHY_SEL_MASK, phy_intf_sels[iface]);

	val = (iface == PHY_INTERFACE_MODE_REVMII) ? 0 : ENMII;
	regmap_update_bits(regmap, reg, ENMII_MASK, val);

	dwmac->fix_retime_src(dwmac, dwmac->speed, 0);

	return 0;
}

static int sti_dwmac_parse_data(struct sti_dwmac *dwmac,
				struct platform_device *pdev,
				struct plat_stmmacenet_data *plat_dat)
{
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct regmap *regmap;
	int err;

	/* clk selection from extra syscfg register */
	dwmac->clk_sel_reg = -ENXIO;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sti-clkconf");
	if (res)
		dwmac->clk_sel_reg = res->start;

	regmap = syscon_regmap_lookup_by_phandle_args(np, "st,syscon",
						      1, &dwmac->ctrl_reg);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	dwmac->interface = plat_dat->phy_interface;
	dwmac->regmap = regmap;
	dwmac->gmac_en = of_property_read_bool(np, "st,gmac_en");
	dwmac->ext_phyclk = of_property_read_bool(np, "st,ext-phyclk");
	dwmac->tx_retime_src = TX_RETIME_SRC_NA;
	dwmac->speed = SPEED_100;

	if (IS_PHY_IF_MODE_GBIT(dwmac->interface)) {
		const char *rs;

		dwmac->tx_retime_src = TX_RETIME_SRC_CLKGEN;

		err = of_property_read_string(np, "st,tx-retime-src", &rs);
		if (err < 0) {
			dev_warn(dev, "Use internal clock source\n");
		} else {
			if (!strcasecmp(rs, "clk_125"))
				dwmac->tx_retime_src = TX_RETIME_SRC_CLK_125;
			else if (!strcasecmp(rs, "txclk"))
				dwmac->tx_retime_src = TX_RETIME_SRC_TXCLK;
		}
		dwmac->speed = SPEED_1000;
	}

	dwmac->clk = devm_clk_get(dev, "sti-ethclk");
	if (IS_ERR(dwmac->clk)) {
		dev_warn(dev, "No phy clock provided...\n");
		dwmac->clk = NULL;
	}

	return 0;
}

static int sti_dwmac_init(struct platform_device *pdev, void *bsp_priv)
{
	struct sti_dwmac *dwmac = bsp_priv;
	int ret;

	ret = clk_prepare_enable(dwmac->clk);
	if (ret)
		return ret;

	ret = sti_dwmac_set_mode(dwmac);
	if (ret)
		clk_disable_unprepare(dwmac->clk);

	return ret;
}

static void sti_dwmac_exit(struct platform_device *pdev, void *bsp_priv)
{
	struct sti_dwmac *dwmac = bsp_priv;

	clk_disable_unprepare(dwmac->clk);
}

static int sti_dwmac_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	const struct sti_dwmac_of_data *data;
	struct stmmac_resources stmmac_res;
	struct sti_dwmac *dwmac;
	int ret;

	data = of_device_get_match_data(&pdev->dev);
	if (!data) {
		dev_err(&pdev->dev, "No OF match data provided\n");
		return -EINVAL;
	}

	ret = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (ret)
		return ret;

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	dwmac = devm_kzalloc(&pdev->dev, sizeof(*dwmac), GFP_KERNEL);
	if (!dwmac)
		return -ENOMEM;

	ret = sti_dwmac_parse_data(dwmac, pdev, plat_dat);
	if (ret) {
		dev_err(&pdev->dev, "Unable to parse OF data\n");
		return ret;
	}

	dwmac->fix_retime_src = data->fix_retime_src;

	plat_dat->bsp_priv = dwmac;
	plat_dat->fix_mac_speed = data->fix_retime_src;
	plat_dat->init = sti_dwmac_init;
	plat_dat->exit = sti_dwmac_exit;

	return devm_stmmac_pltfr_probe(pdev, plat_dat, &stmmac_res);
}

static const struct sti_dwmac_of_data stih4xx_dwmac_data = {
	.fix_retime_src = stih4xx_fix_retime_src,
};

static const struct of_device_id sti_dwmac_match[] = {
	{ .compatible = "st,stih407-dwmac", .data = &stih4xx_dwmac_data},
	{ }
};
MODULE_DEVICE_TABLE(of, sti_dwmac_match);

static struct platform_driver sti_dwmac_driver = {
	.probe  = sti_dwmac_probe,
	.driver = {
		.name           = "sti-dwmac",
		.pm		= &stmmac_pltfr_pm_ops,
		.of_match_table = sti_dwmac_match,
	},
};
module_platform_driver(sti_dwmac_driver);

MODULE_AUTHOR("Srinivas Kandagatla <srinivas.kandagatla@st.com>");
MODULE_DESCRIPTION("STMicroelectronics DWMAC Specific Glue layer");
MODULE_LICENSE("GPL");
