/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __NET_TC_NAT_H
#define __NET_TC_NAT_H

#include <linux/types.h>
#include <net/act_api.h>

struct tcf_nat_parms {
	int action;
	__be32 old_addr;
	__be32 new_addr;
	__be32 mask;
	u32 flags;
	struct rcu_head rcu;
};

struct tcf_nat {
	struct tc_action common;
	struct tcf_nat_parms __rcu *parms;
};

#define to_tcf_nat(a) ((struct tcf_nat *)a)

#endif /* __NET_TC_NAT_H */
