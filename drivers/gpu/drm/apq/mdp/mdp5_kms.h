/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Author: Jagath Jog <jagathjog1996@gmail.com> 
 */

#ifndef __MDP5_KMS_H__
#define __MDP5_KMS_H__

struct mdp5_kms {
	struct device *dev;
	struct clk *ahb_clk;
	struct clk *axi_clk;
	struct clk *core_clk;
	void __iomem *mmio;
};

#endif
