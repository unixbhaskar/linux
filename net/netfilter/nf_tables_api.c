// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2007-2009 Patrick McHardy <kaber@trash.net>
 *
 * Development of this code funded by Astaro AG (http://www.astaro.com/)
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/skbuff.h>
#include <linux/netlink.h>
#include <linux/vmalloc.h>
#include <linux/rhashtable.h>
#include <linux/audit.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nf_tables.h>
#include <net/netfilter/nf_flow_table.h>
#include <net/netfilter/nf_tables_core.h>
#include <net/netfilter/nf_tables.h>
#include <net/netfilter/nf_tables_offload.h>
#include <net/net_namespace.h>
#include <net/sock.h>

#define NFT_MODULE_AUTOLOAD_LIMIT (MODULE_NAME_LEN - sizeof("nft-expr-255-"))
#define NFT_SET_MAX_ANONLEN 16

/* limit compaction to avoid huge kmalloc/krealloc sizes. */
#define NFT_MAX_SET_NELEMS ((2048 - sizeof(struct nft_trans_elem)) / sizeof(struct nft_trans_one_elem))

unsigned int nf_tables_net_id __read_mostly;

static LIST_HEAD(nf_tables_expressions);
static LIST_HEAD(nf_tables_objects);
static LIST_HEAD(nf_tables_flowtables);
static LIST_HEAD(nf_tables_gc_list);
static DEFINE_SPINLOCK(nf_tables_destroy_list_lock);
static DEFINE_SPINLOCK(nf_tables_gc_list_lock);

enum {
	NFT_VALIDATE_SKIP	= 0,
	NFT_VALIDATE_NEED,
	NFT_VALIDATE_DO,
};

static struct rhltable nft_objname_ht;

static u32 nft_chain_hash(const void *data, u32 len, u32 seed);
static u32 nft_chain_hash_obj(const void *data, u32 len, u32 seed);
static int nft_chain_hash_cmp(struct rhashtable_compare_arg *, const void *);

static u32 nft_objname_hash(const void *data, u32 len, u32 seed);
static u32 nft_objname_hash_obj(const void *data, u32 len, u32 seed);
static int nft_objname_hash_cmp(struct rhashtable_compare_arg *, const void *);

static const struct rhashtable_params nft_chain_ht_params = {
	.head_offset		= offsetof(struct nft_chain, rhlhead),
	.key_offset		= offsetof(struct nft_chain, name),
	.hashfn			= nft_chain_hash,
	.obj_hashfn		= nft_chain_hash_obj,
	.obj_cmpfn		= nft_chain_hash_cmp,
	.automatic_shrinking	= true,
};

static const struct rhashtable_params nft_objname_ht_params = {
	.head_offset		= offsetof(struct nft_object, rhlhead),
	.key_offset		= offsetof(struct nft_object, key),
	.hashfn			= nft_objname_hash,
	.obj_hashfn		= nft_objname_hash_obj,
	.obj_cmpfn		= nft_objname_hash_cmp,
	.automatic_shrinking	= true,
};

struct nft_audit_data {
	struct nft_table *table;
	int entries;
	int op;
	struct list_head list;
};

static const u8 nft2audit_op[NFT_MSG_MAX] = { // enum nf_tables_msg_types
	[NFT_MSG_NEWTABLE]	= AUDIT_NFT_OP_TABLE_REGISTER,
	[NFT_MSG_GETTABLE]	= AUDIT_NFT_OP_INVALID,
	[NFT_MSG_DELTABLE]	= AUDIT_NFT_OP_TABLE_UNREGISTER,
	[NFT_MSG_NEWCHAIN]	= AUDIT_NFT_OP_CHAIN_REGISTER,
	[NFT_MSG_GETCHAIN]	= AUDIT_NFT_OP_INVALID,
	[NFT_MSG_DELCHAIN]	= AUDIT_NFT_OP_CHAIN_UNREGISTER,
	[NFT_MSG_NEWRULE]	= AUDIT_NFT_OP_RULE_REGISTER,
	[NFT_MSG_GETRULE]	= AUDIT_NFT_OP_INVALID,
	[NFT_MSG_DELRULE]	= AUDIT_NFT_OP_RULE_UNREGISTER,
	[NFT_MSG_NEWSET]	= AUDIT_NFT_OP_SET_REGISTER,
	[NFT_MSG_GETSET]	= AUDIT_NFT_OP_INVALID,
	[NFT_MSG_DELSET]	= AUDIT_NFT_OP_SET_UNREGISTER,
	[NFT_MSG_NEWSETELEM]	= AUDIT_NFT_OP_SETELEM_REGISTER,
	[NFT_MSG_GETSETELEM]	= AUDIT_NFT_OP_INVALID,
	[NFT_MSG_DELSETELEM]	= AUDIT_NFT_OP_SETELEM_UNREGISTER,
	[NFT_MSG_NEWGEN]	= AUDIT_NFT_OP_GEN_REGISTER,
	[NFT_MSG_GETGEN]	= AUDIT_NFT_OP_INVALID,
	[NFT_MSG_TRACE]		= AUDIT_NFT_OP_INVALID,
	[NFT_MSG_NEWOBJ]	= AUDIT_NFT_OP_OBJ_REGISTER,
	[NFT_MSG_GETOBJ]	= AUDIT_NFT_OP_INVALID,
	[NFT_MSG_DELOBJ]	= AUDIT_NFT_OP_OBJ_UNREGISTER,
	[NFT_MSG_GETOBJ_RESET]	= AUDIT_NFT_OP_OBJ_RESET,
	[NFT_MSG_NEWFLOWTABLE]	= AUDIT_NFT_OP_FLOWTABLE_REGISTER,
	[NFT_MSG_GETFLOWTABLE]	= AUDIT_NFT_OP_INVALID,
	[NFT_MSG_DELFLOWTABLE]	= AUDIT_NFT_OP_FLOWTABLE_UNREGISTER,
	[NFT_MSG_GETSETELEM_RESET] = AUDIT_NFT_OP_SETELEM_RESET,
};

static void nft_validate_state_update(struct nft_table *table, u8 new_validate_state)
{
	switch (table->validate_state) {
	case NFT_VALIDATE_SKIP:
		WARN_ON_ONCE(new_validate_state == NFT_VALIDATE_DO);
		break;
	case NFT_VALIDATE_NEED:
		break;
	case NFT_VALIDATE_DO:
		if (new_validate_state == NFT_VALIDATE_NEED)
			return;
	}

	table->validate_state = new_validate_state;
}
static void nf_tables_trans_destroy_work(struct work_struct *w);

static void nft_trans_gc_work(struct work_struct *work);
static DECLARE_WORK(trans_gc_work, nft_trans_gc_work);

static void nft_ctx_init(struct nft_ctx *ctx,
			 struct net *net,
			 const struct sk_buff *skb,
			 const struct nlmsghdr *nlh,
			 u8 family,
			 struct nft_table *table,
			 struct nft_chain *chain,
			 const struct nlattr * const *nla)
{
	ctx->net	= net;
	ctx->family	= family;
	ctx->level	= 0;
	ctx->table	= table;
	ctx->chain	= chain;
	ctx->nla   	= nla;
	ctx->portid	= NETLINK_CB(skb).portid;
	ctx->report	= nlmsg_report(nlh);
	ctx->flags	= nlh->nlmsg_flags;
	ctx->seq	= nlh->nlmsg_seq;

	bitmap_zero(ctx->reg_inited, NFT_REG32_NUM);
}

static struct nft_trans *nft_trans_alloc_gfp(const struct nft_ctx *ctx,
					     int msg_type, u32 size, gfp_t gfp)
{
	struct nft_trans *trans;

	trans = kzalloc(size, gfp);
	if (trans == NULL)
		return NULL;

	INIT_LIST_HEAD(&trans->list);
	trans->msg_type = msg_type;

	trans->net = ctx->net;
	trans->table = ctx->table;
	trans->seq = ctx->seq;
	trans->flags = ctx->flags;
	trans->report = ctx->report;

	return trans;
}

static struct nft_trans *nft_trans_alloc(const struct nft_ctx *ctx,
					 int msg_type, u32 size)
{
	return nft_trans_alloc_gfp(ctx, msg_type, size, GFP_KERNEL);
}

static struct nft_trans_binding *nft_trans_get_binding(struct nft_trans *trans)
{
	switch (trans->msg_type) {
	case NFT_MSG_NEWCHAIN:
	case NFT_MSG_NEWSET:
		return container_of(trans, struct nft_trans_binding, nft_trans);
	}

	return NULL;
}

static void nft_trans_list_del(struct nft_trans *trans)
{
	struct nft_trans_binding *trans_binding;

	list_del(&trans->list);

	trans_binding = nft_trans_get_binding(trans);
	if (trans_binding)
		list_del(&trans_binding->binding_list);
}

static void nft_trans_destroy(struct nft_trans *trans)
{
	nft_trans_list_del(trans);
	kfree(trans);
}

static void __nft_set_trans_bind(const struct nft_ctx *ctx, struct nft_set *set,
				 bool bind)
{
	struct nftables_pernet *nft_net;
	struct net *net = ctx->net;
	struct nft_trans *trans;

	if (!nft_set_is_anonymous(set))
		return;

	nft_net = nft_pernet(net);
	list_for_each_entry_reverse(trans, &nft_net->commit_list, list) {
		switch (trans->msg_type) {
		case NFT_MSG_NEWSET:
			if (nft_trans_set(trans) == set)
				nft_trans_set_bound(trans) = bind;
			break;
		case NFT_MSG_NEWSETELEM:
			if (nft_trans_elem_set(trans) == set)
				nft_trans_elem_set_bound(trans) = bind;
			break;
		}
	}
}

static void nft_set_trans_bind(const struct nft_ctx *ctx, struct nft_set *set)
{
	return __nft_set_trans_bind(ctx, set, true);
}

static void nft_set_trans_unbind(const struct nft_ctx *ctx, struct nft_set *set)
{
	return __nft_set_trans_bind(ctx, set, false);
}

static void __nft_chain_trans_bind(const struct nft_ctx *ctx,
				   struct nft_chain *chain, bool bind)
{
	struct nftables_pernet *nft_net;
	struct net *net = ctx->net;
	struct nft_trans *trans;

	if (!nft_chain_binding(chain))
		return;

	nft_net = nft_pernet(net);
	list_for_each_entry_reverse(trans, &nft_net->commit_list, list) {
		switch (trans->msg_type) {
		case NFT_MSG_NEWCHAIN:
			if (nft_trans_chain(trans) == chain)
				nft_trans_chain_bound(trans) = bind;
			break;
		case NFT_MSG_NEWRULE:
			if (nft_trans_rule_chain(trans) == chain)
				nft_trans_rule_bound(trans) = bind;
			break;
		}
	}
}

static void nft_chain_trans_bind(const struct nft_ctx *ctx,
				 struct nft_chain *chain)
{
	__nft_chain_trans_bind(ctx, chain, true);
}

int nf_tables_bind_chain(const struct nft_ctx *ctx, struct nft_chain *chain)
{
	if (!nft_chain_binding(chain))
		return 0;

	if (nft_chain_binding(ctx->chain))
		return -EOPNOTSUPP;

	if (chain->bound)
		return -EBUSY;

	if (!nft_use_inc(&chain->use))
		return -EMFILE;

	chain->bound = true;
	nft_chain_trans_bind(ctx, chain);

	return 0;
}

void nf_tables_unbind_chain(const struct nft_ctx *ctx, struct nft_chain *chain)
{
	__nft_chain_trans_bind(ctx, chain, false);
}

static int nft_netdev_register_hooks(struct net *net,
				     struct list_head *hook_list)
{
	struct nf_hook_ops *ops;
	struct nft_hook *hook;
	int err, j;

	j = 0;
	list_for_each_entry(hook, hook_list, list) {
		list_for_each_entry(ops, &hook->ops_list, list) {
			err = nf_register_net_hook(net, ops);
			if (err < 0)
				goto err_register;

			j++;
		}
	}
	return 0;

err_register:
	list_for_each_entry(hook, hook_list, list) {
		list_for_each_entry(ops, &hook->ops_list, list) {
			if (j-- <= 0)
				break;

			nf_unregister_net_hook(net, ops);
		}
	}
	return err;
}

static void nft_netdev_hook_free_ops(struct nft_hook *hook)
{
	struct nf_hook_ops *ops, *next;

	list_for_each_entry_safe(ops, next, &hook->ops_list, list) {
		list_del(&ops->list);
		kfree(ops);
	}
}

static void nft_netdev_hook_free(struct nft_hook *hook)
{
	nft_netdev_hook_free_ops(hook);
	kfree(hook);
}

static void __nft_netdev_hook_free_rcu(struct rcu_head *rcu)
{
	struct nft_hook *hook = container_of(rcu, struct nft_hook, rcu);

	nft_netdev_hook_free(hook);
}

static void nft_netdev_hook_free_rcu(struct nft_hook *hook)
{
	call_rcu(&hook->rcu, __nft_netdev_hook_free_rcu);
}

static void nft_netdev_unregister_hooks(struct net *net,
					struct list_head *hook_list,
					bool release_netdev)
{
	struct nft_hook *hook, *next;
	struct nf_hook_ops *ops;

	list_for_each_entry_safe(hook, next, hook_list, list) {
		list_for_each_entry(ops, &hook->ops_list, list)
			nf_unregister_net_hook(net, ops);
		if (release_netdev) {
			list_del(&hook->list);
			nft_netdev_hook_free_rcu(hook);
		}
	}
}

static int nf_tables_register_hook(struct net *net,
				   const struct nft_table *table,
				   struct nft_chain *chain)
{
	struct nft_base_chain *basechain;
	const struct nf_hook_ops *ops;

	if (table->flags & NFT_TABLE_F_DORMANT ||
	    !nft_is_base_chain(chain))
		return 0;

	basechain = nft_base_chain(chain);
	ops = &basechain->ops;

	if (basechain->type->ops_register)
		return basechain->type->ops_register(net, ops);

	if (nft_base_chain_netdev(table->family, basechain->ops.hooknum))
		return nft_netdev_register_hooks(net, &basechain->hook_list);

	return nf_register_net_hook(net, &basechain->ops);
}

static void __nf_tables_unregister_hook(struct net *net,
					const struct nft_table *table,
					struct nft_chain *chain,
					bool release_netdev)
{
	struct nft_base_chain *basechain;
	const struct nf_hook_ops *ops;

	if (table->flags & NFT_TABLE_F_DORMANT ||
	    !nft_is_base_chain(chain))
		return;
	basechain = nft_base_chain(chain);
	ops = &basechain->ops;

	if (basechain->type->ops_unregister)
		return basechain->type->ops_unregister(net, ops);

	if (nft_base_chain_netdev(table->family, basechain->ops.hooknum))
		nft_netdev_unregister_hooks(net, &basechain->hook_list,
					    release_netdev);
	else
		nf_unregister_net_hook(net, &basechain->ops);
}

static void nf_tables_unregister_hook(struct net *net,
				      const struct nft_table *table,
				      struct nft_chain *chain)
{
	return __nf_tables_unregister_hook(net, table, chain, false);
}

static bool nft_trans_collapse_set_elem_allowed(const struct nft_trans_elem *a, const struct nft_trans_elem *b)
{
	/* NB: the ->bound equality check is defensive, at this time we only merge
	 * a new nft_trans_elem transaction request with the transaction tail
	 * element, but a->bound != b->bound would imply a NEWRULE transaction
	 * is queued in-between.
	 *
	 * The set check is mandatory, the NFT_MAX_SET_NELEMS check prevents
	 * huge krealloc() requests.
	 */
	return a->set == b->set && a->bound == b->bound && a->nelems < NFT_MAX_SET_NELEMS;
}

static bool nft_trans_collapse_set_elem(struct nftables_pernet *nft_net,
					struct nft_trans_elem *tail,
					struct nft_trans_elem *trans,
					gfp_t gfp)
{
	unsigned int nelems, old_nelems = tail->nelems;
	struct nft_trans_elem *new_trans;

	if (!nft_trans_collapse_set_elem_allowed(tail, trans))
		return false;

	/* "cannot happen", at this time userspace element add
	 * requests always allocate a new transaction element.
	 *
	 * This serves as a reminder to adjust the list_add_tail
	 * logic below in case this ever changes.
	 */
	if (WARN_ON_ONCE(trans->nelems != 1))
		return false;

	if (check_add_overflow(old_nelems, trans->nelems, &nelems))
		return false;

	/* krealloc might free tail which invalidates list pointers */
	list_del_init(&tail->nft_trans.list);

	new_trans = krealloc(tail, struct_size(tail, elems, nelems), gfp);
	if (!new_trans) {
		list_add_tail(&tail->nft_trans.list, &nft_net->commit_list);
		return false;
	}

	/*
	 * new_trans->nft_trans.list contains garbage, but
	 * list_add_tail() doesn't care.
	 */
	new_trans->nelems = nelems;
	new_trans->elems[old_nelems] = trans->elems[0];
	list_add_tail(&new_trans->nft_trans.list, &nft_net->commit_list);

	return true;
}

static bool nft_trans_try_collapse(struct nftables_pernet *nft_net,
				   struct nft_trans *trans, gfp_t gfp)
{
	struct nft_trans *tail;

	if (list_empty(&nft_net->commit_list))
		return false;

	tail = list_last_entry(&nft_net->commit_list, struct nft_trans, list);

	if (tail->msg_type != trans->msg_type)
		return false;

	switch (trans->msg_type) {
	case NFT_MSG_NEWSETELEM:
	case NFT_MSG_DELSETELEM:
		return nft_trans_collapse_set_elem(nft_net,
						   nft_trans_container_elem(tail),
						   nft_trans_container_elem(trans), gfp);
	}

	return false;
}

static void nft_trans_commit_list_add_tail(struct net *net, struct nft_trans *trans)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	struct nft_trans_binding *binding;
	struct nft_trans_set *trans_set;

	list_add_tail(&trans->list, &nft_net->commit_list);

	binding = nft_trans_get_binding(trans);
	if (!binding)
		return;

	switch (trans->msg_type) {
	case NFT_MSG_NEWSET:
		trans_set = nft_trans_container_set(trans);

		if (!nft_trans_set_update(trans) &&
		    nft_set_is_anonymous(nft_trans_set(trans)))
			list_add_tail(&binding->binding_list, &nft_net->binding_list);

		list_add_tail(&trans_set->list_trans_newset, &nft_net->commit_set_list);
		break;
	case NFT_MSG_NEWCHAIN:
		if (!nft_trans_chain_update(trans) &&
		    nft_chain_binding(nft_trans_chain(trans)))
			list_add_tail(&binding->binding_list, &nft_net->binding_list);
		break;
	}
}

static void nft_trans_commit_list_add_elem(struct net *net, struct nft_trans *trans,
					   gfp_t gfp)
{
	struct nftables_pernet *nft_net = nft_pernet(net);

	WARN_ON_ONCE(trans->msg_type != NFT_MSG_NEWSETELEM &&
		     trans->msg_type != NFT_MSG_DELSETELEM);

	might_alloc(gfp);

	if (nft_trans_try_collapse(nft_net, trans, gfp)) {
		kfree(trans);
		return;
	}

	nft_trans_commit_list_add_tail(net, trans);
}

static int nft_trans_table_add(struct nft_ctx *ctx, int msg_type)
{
	struct nft_trans *trans;

	trans = nft_trans_alloc(ctx, msg_type, sizeof(struct nft_trans_table));
	if (trans == NULL)
		return -ENOMEM;

	if (msg_type == NFT_MSG_NEWTABLE)
		nft_activate_next(ctx->net, ctx->table);

	nft_trans_commit_list_add_tail(ctx->net, trans);
	return 0;
}

static int nft_deltable(struct nft_ctx *ctx)
{
	int err;

	err = nft_trans_table_add(ctx, NFT_MSG_DELTABLE);
	if (err < 0)
		return err;

	nft_deactivate_next(ctx->net, ctx->table);
	return err;
}

static struct nft_trans *
nft_trans_alloc_chain(const struct nft_ctx *ctx, int msg_type)
{
	struct nft_trans_chain *trans_chain;
	struct nft_trans *trans;

	trans = nft_trans_alloc(ctx, msg_type, sizeof(struct nft_trans_chain));
	if (!trans)
		return NULL;

	trans_chain = nft_trans_container_chain(trans);
	INIT_LIST_HEAD(&trans_chain->nft_trans_binding.binding_list);
	trans_chain->chain = ctx->chain;

	return trans;
}

static struct nft_trans *nft_trans_chain_add(struct nft_ctx *ctx, int msg_type)
{
	struct nft_trans *trans;

	trans = nft_trans_alloc_chain(ctx, msg_type);
	if (trans == NULL)
		return ERR_PTR(-ENOMEM);

	if (msg_type == NFT_MSG_NEWCHAIN) {
		nft_activate_next(ctx->net, ctx->chain);

		if (ctx->nla[NFTA_CHAIN_ID]) {
			nft_trans_chain_id(trans) =
				ntohl(nla_get_be32(ctx->nla[NFTA_CHAIN_ID]));
		}
	}
	nft_trans_commit_list_add_tail(ctx->net, trans);

	return trans;
}

static int nft_delchain(struct nft_ctx *ctx)
{
	struct nft_trans *trans;

	trans = nft_trans_chain_add(ctx, NFT_MSG_DELCHAIN);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	nft_use_dec(&ctx->table->use);
	nft_deactivate_next(ctx->net, ctx->chain);

	return 0;
}

void nft_rule_expr_activate(const struct nft_ctx *ctx, struct nft_rule *rule)
{
	struct nft_expr *expr;

	expr = nft_expr_first(rule);
	while (nft_expr_more(rule, expr)) {
		if (expr->ops->activate)
			expr->ops->activate(ctx, expr);

		expr = nft_expr_next(expr);
	}
}

void nft_rule_expr_deactivate(const struct nft_ctx *ctx, struct nft_rule *rule,
			      enum nft_trans_phase phase)
{
	struct nft_expr *expr;

	expr = nft_expr_first(rule);
	while (nft_expr_more(rule, expr)) {
		if (expr->ops->deactivate)
			expr->ops->deactivate(ctx, expr, phase);

		expr = nft_expr_next(expr);
	}
}

static int
nf_tables_delrule_deactivate(struct nft_ctx *ctx, struct nft_rule *rule)
{
	/* You cannot delete the same rule twice */
	if (nft_is_active_next(ctx->net, rule)) {
		nft_deactivate_next(ctx->net, rule);
		nft_use_dec(&ctx->chain->use);
		return 0;
	}
	return -ENOENT;
}

static struct nft_trans *nft_trans_rule_add(struct nft_ctx *ctx, int msg_type,
					    struct nft_rule *rule)
{
	struct nft_trans *trans;

	trans = nft_trans_alloc(ctx, msg_type, sizeof(struct nft_trans_rule));
	if (trans == NULL)
		return NULL;

	if (msg_type == NFT_MSG_NEWRULE && ctx->nla[NFTA_RULE_ID] != NULL) {
		nft_trans_rule_id(trans) =
			ntohl(nla_get_be32(ctx->nla[NFTA_RULE_ID]));
	}
	nft_trans_rule(trans) = rule;
	nft_trans_rule_chain(trans) = ctx->chain;
	nft_trans_commit_list_add_tail(ctx->net, trans);

	return trans;
}

static int nft_delrule(struct nft_ctx *ctx, struct nft_rule *rule)
{
	struct nft_flow_rule *flow;
	struct nft_trans *trans;
	int err;

	trans = nft_trans_rule_add(ctx, NFT_MSG_DELRULE, rule);
	if (trans == NULL)
		return -ENOMEM;

	if (ctx->chain->flags & NFT_CHAIN_HW_OFFLOAD) {
		flow = nft_flow_rule_create(ctx->net, rule);
		if (IS_ERR(flow)) {
			nft_trans_destroy(trans);
			return PTR_ERR(flow);
		}

		nft_trans_flow_rule(trans) = flow;
	}

	err = nf_tables_delrule_deactivate(ctx, rule);
	if (err < 0) {
		nft_trans_destroy(trans);
		return err;
	}
	nft_rule_expr_deactivate(ctx, rule, NFT_TRANS_PREPARE);

	return 0;
}

static int nft_delrule_by_chain(struct nft_ctx *ctx)
{
	struct nft_rule *rule;
	int err;

	list_for_each_entry(rule, &ctx->chain->rules, list) {
		if (!nft_is_active_next(ctx->net, rule))
			continue;

		err = nft_delrule(ctx, rule);
		if (err < 0)
			return err;
	}
	return 0;
}

static int __nft_trans_set_add(const struct nft_ctx *ctx, int msg_type,
			       struct nft_set *set,
			       const struct nft_set_desc *desc)
{
	struct nft_trans_set *trans_set;
	struct nft_trans *trans;

	trans = nft_trans_alloc(ctx, msg_type, sizeof(struct nft_trans_set));
	if (trans == NULL)
		return -ENOMEM;

	trans_set = nft_trans_container_set(trans);
	INIT_LIST_HEAD(&trans_set->nft_trans_binding.binding_list);
	INIT_LIST_HEAD(&trans_set->list_trans_newset);

	if (msg_type == NFT_MSG_NEWSET && ctx->nla[NFTA_SET_ID] && !desc) {
		nft_trans_set_id(trans) =
			ntohl(nla_get_be32(ctx->nla[NFTA_SET_ID]));
		nft_activate_next(ctx->net, set);
	}
	nft_trans_set(trans) = set;
	if (desc) {
		nft_trans_set_update(trans) = true;
		nft_trans_set_gc_int(trans) = desc->gc_int;
		nft_trans_set_timeout(trans) = desc->timeout;
		nft_trans_set_size(trans) = desc->size;
	}
	nft_trans_commit_list_add_tail(ctx->net, trans);

	return 0;
}

static int nft_trans_set_add(const struct nft_ctx *ctx, int msg_type,
			     struct nft_set *set)
{
	return __nft_trans_set_add(ctx, msg_type, set, NULL);
}

static int nft_mapelem_deactivate(const struct nft_ctx *ctx,
				  struct nft_set *set,
				  const struct nft_set_iter *iter,
				  struct nft_elem_priv *elem_priv)
{
	struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);

	if (!nft_set_elem_active(ext, iter->genmask))
		return 0;

	nft_set_elem_change_active(ctx->net, set, ext);
	nft_setelem_data_deactivate(ctx->net, set, elem_priv);

	return 0;
}

struct nft_set_elem_catchall {
	struct list_head	list;
	struct rcu_head		rcu;
	struct nft_elem_priv	*elem;
};

static void nft_map_catchall_deactivate(const struct nft_ctx *ctx,
					struct nft_set *set)
{
	u8 genmask = nft_genmask_next(ctx->net);
	struct nft_set_elem_catchall *catchall;
	struct nft_set_ext *ext;

	list_for_each_entry(catchall, &set->catchall_list, list) {
		ext = nft_set_elem_ext(set, catchall->elem);
		if (!nft_set_elem_active(ext, genmask))
			continue;

		nft_set_elem_change_active(ctx->net, set, ext);
		nft_setelem_data_deactivate(ctx->net, set, catchall->elem);
		break;
	}
}

static void nft_map_deactivate(const struct nft_ctx *ctx, struct nft_set *set)
{
	struct nft_set_iter iter = {
		.genmask	= nft_genmask_next(ctx->net),
		.type		= NFT_ITER_UPDATE,
		.fn		= nft_mapelem_deactivate,
	};

	set->ops->walk(ctx, set, &iter);
	WARN_ON_ONCE(iter.err);

	nft_map_catchall_deactivate(ctx, set);
}

static int nft_delset(const struct nft_ctx *ctx, struct nft_set *set)
{
	int err;

	err = nft_trans_set_add(ctx, NFT_MSG_DELSET, set);
	if (err < 0)
		return err;

	if (set->flags & (NFT_SET_MAP | NFT_SET_OBJECT))
		nft_map_deactivate(ctx, set);

	nft_deactivate_next(ctx->net, set);
	nft_use_dec(&ctx->table->use);

	return err;
}

static int nft_trans_obj_add(struct nft_ctx *ctx, int msg_type,
			     struct nft_object *obj)
{
	struct nft_trans *trans;

	trans = nft_trans_alloc(ctx, msg_type, sizeof(struct nft_trans_obj));
	if (trans == NULL)
		return -ENOMEM;

	if (msg_type == NFT_MSG_NEWOBJ)
		nft_activate_next(ctx->net, obj);

	nft_trans_obj(trans) = obj;
	nft_trans_commit_list_add_tail(ctx->net, trans);

	return 0;
}

static int nft_delobj(struct nft_ctx *ctx, struct nft_object *obj)
{
	int err;

	err = nft_trans_obj_add(ctx, NFT_MSG_DELOBJ, obj);
	if (err < 0)
		return err;

	nft_deactivate_next(ctx->net, obj);
	nft_use_dec(&ctx->table->use);

	return err;
}

static struct nft_trans *
nft_trans_flowtable_add(struct nft_ctx *ctx, int msg_type,
		        struct nft_flowtable *flowtable)
{
	struct nft_trans *trans;

	trans = nft_trans_alloc(ctx, msg_type,
				sizeof(struct nft_trans_flowtable));
	if (trans == NULL)
		return ERR_PTR(-ENOMEM);

	if (msg_type == NFT_MSG_NEWFLOWTABLE)
		nft_activate_next(ctx->net, flowtable);

	INIT_LIST_HEAD(&nft_trans_flowtable_hooks(trans));
	nft_trans_flowtable(trans) = flowtable;
	nft_trans_commit_list_add_tail(ctx->net, trans);

	return trans;
}

static int nft_delflowtable(struct nft_ctx *ctx,
			    struct nft_flowtable *flowtable)
{
	struct nft_trans *trans;

	trans = nft_trans_flowtable_add(ctx, NFT_MSG_DELFLOWTABLE, flowtable);
	if (IS_ERR(trans))
		return PTR_ERR(trans);

	nft_deactivate_next(ctx->net, flowtable);
	nft_use_dec(&ctx->table->use);

	return 0;
}

static void __nft_reg_track_clobber(struct nft_regs_track *track, u8 dreg)
{
	int i;

	for (i = track->regs[dreg].num_reg; i > 0; i--)
		__nft_reg_track_cancel(track, dreg - i);
}

static void __nft_reg_track_update(struct nft_regs_track *track,
				   const struct nft_expr *expr,
				   u8 dreg, u8 num_reg)
{
	track->regs[dreg].selector = expr;
	track->regs[dreg].bitwise = NULL;
	track->regs[dreg].num_reg = num_reg;
}

void nft_reg_track_update(struct nft_regs_track *track,
			  const struct nft_expr *expr, u8 dreg, u8 len)
{
	unsigned int regcount;
	int i;

	__nft_reg_track_clobber(track, dreg);

	regcount = DIV_ROUND_UP(len, NFT_REG32_SIZE);
	for (i = 0; i < regcount; i++, dreg++)
		__nft_reg_track_update(track, expr, dreg, i);
}
EXPORT_SYMBOL_GPL(nft_reg_track_update);

void nft_reg_track_cancel(struct nft_regs_track *track, u8 dreg, u8 len)
{
	unsigned int regcount;
	int i;

	__nft_reg_track_clobber(track, dreg);

	regcount = DIV_ROUND_UP(len, NFT_REG32_SIZE);
	for (i = 0; i < regcount; i++, dreg++)
		__nft_reg_track_cancel(track, dreg);
}
EXPORT_SYMBOL_GPL(nft_reg_track_cancel);

void __nft_reg_track_cancel(struct nft_regs_track *track, u8 dreg)
{
	track->regs[dreg].selector = NULL;
	track->regs[dreg].bitwise = NULL;
	track->regs[dreg].num_reg = 0;
}
EXPORT_SYMBOL_GPL(__nft_reg_track_cancel);

/*
 * Tables
 */

static struct nft_table *nft_table_lookup(const struct net *net,
					  const struct nlattr *nla,
					  u8 family, u8 genmask, u32 nlpid)
{
	struct nftables_pernet *nft_net;
	struct nft_table *table;

	if (nla == NULL)
		return ERR_PTR(-EINVAL);

	nft_net = nft_pernet(net);
	list_for_each_entry_rcu(table, &nft_net->tables, list,
				lockdep_is_held(&nft_net->commit_mutex)) {
		if (!nla_strcmp(nla, table->name) &&
		    table->family == family &&
		    nft_active_genmask(table, genmask)) {
			if (nft_table_has_owner(table) &&
			    nlpid && table->nlpid != nlpid)
				return ERR_PTR(-EPERM);

			return table;
		}
	}

	return ERR_PTR(-ENOENT);
}

static struct nft_table *nft_table_lookup_byhandle(const struct net *net,
						   const struct nlattr *nla,
						   int family, u8 genmask, u32 nlpid)
{
	struct nftables_pernet *nft_net;
	struct nft_table *table;

	nft_net = nft_pernet(net);
	list_for_each_entry(table, &nft_net->tables, list) {
		if (be64_to_cpu(nla_get_be64(nla)) == table->handle &&
		    table->family == family &&
		    nft_active_genmask(table, genmask)) {
			if (nft_table_has_owner(table) &&
			    nlpid && table->nlpid != nlpid)
				return ERR_PTR(-EPERM);

			return table;
		}
	}

	return ERR_PTR(-ENOENT);
}

static inline u64 nf_tables_alloc_handle(struct nft_table *table)
{
	return ++table->hgenerator;
}

static const struct nft_chain_type *chain_type[NFPROTO_NUMPROTO][NFT_CHAIN_T_MAX];

static const struct nft_chain_type *
__nft_chain_type_get(u8 family, enum nft_chain_types type)
{
	if (family >= NFPROTO_NUMPROTO ||
	    type >= NFT_CHAIN_T_MAX)
		return NULL;

	return chain_type[family][type];
}

static const struct nft_chain_type *
__nf_tables_chain_type_lookup(const struct nlattr *nla, u8 family)
{
	const struct nft_chain_type *type;
	int i;

	for (i = 0; i < NFT_CHAIN_T_MAX; i++) {
		type = __nft_chain_type_get(family, i);
		if (!type)
			continue;
		if (!nla_strcmp(nla, type->name))
			return type;
	}
	return NULL;
}

struct nft_module_request {
	struct list_head	list;
	char			module[MODULE_NAME_LEN];
	bool			done;
};

#ifdef CONFIG_MODULES
__printf(2, 3) int nft_request_module(struct net *net, const char *fmt,
				      ...)
{
	char module_name[MODULE_NAME_LEN];
	struct nftables_pernet *nft_net;
	struct nft_module_request *req;
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = vsnprintf(module_name, MODULE_NAME_LEN, fmt, args);
	va_end(args);
	if (ret >= MODULE_NAME_LEN)
		return 0;

	nft_net = nft_pernet(net);
	list_for_each_entry(req, &nft_net->module_list, list) {
		if (!strcmp(req->module, module_name)) {
			if (req->done)
				return 0;

			/* A request to load this module already exists. */
			return -EAGAIN;
		}
	}

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req->done = false;
	strscpy(req->module, module_name, MODULE_NAME_LEN);
	list_add_tail(&req->list, &nft_net->module_list);

	return -EAGAIN;
}
EXPORT_SYMBOL_GPL(nft_request_module);
#endif

static void lockdep_nfnl_nft_mutex_not_held(void)
{
#ifdef CONFIG_PROVE_LOCKING
	if (debug_locks)
		WARN_ON_ONCE(lockdep_nfnl_is_held(NFNL_SUBSYS_NFTABLES));
#endif
}

static const struct nft_chain_type *
nf_tables_chain_type_lookup(struct net *net, const struct nlattr *nla,
			    u8 family, bool autoload)
{
	const struct nft_chain_type *type;

	type = __nf_tables_chain_type_lookup(nla, family);
	if (type != NULL)
		return type;

	lockdep_nfnl_nft_mutex_not_held();
#ifdef CONFIG_MODULES
	if (autoload) {
		if (nft_request_module(net, "nft-chain-%u-%.*s", family,
				       nla_len(nla),
				       (const char *)nla_data(nla)) == -EAGAIN)
			return ERR_PTR(-EAGAIN);
	}
#endif
	return ERR_PTR(-ENOENT);
}

static __be16 nft_base_seq(const struct net *net)
{
	struct nftables_pernet *nft_net = nft_pernet(net);

	return htons(nft_net->base_seq & 0xffff);
}

static const struct nla_policy nft_table_policy[NFTA_TABLE_MAX + 1] = {
	[NFTA_TABLE_NAME]	= { .type = NLA_STRING,
				    .len = NFT_TABLE_MAXNAMELEN - 1 },
	[NFTA_TABLE_FLAGS]	= { .type = NLA_U32 },
	[NFTA_TABLE_HANDLE]	= { .type = NLA_U64 },
	[NFTA_TABLE_USERDATA]	= { .type = NLA_BINARY,
				    .len = NFT_USERDATA_MAXLEN }
};

static int nf_tables_fill_table_info(struct sk_buff *skb, struct net *net,
				     u32 portid, u32 seq, int event, u32 flags,
				     int family, const struct nft_table *table)
{
	struct nlmsghdr *nlh;

	nlh = nfnl_msg_put(skb, portid, seq,
			   nfnl_msg_type(NFNL_SUBSYS_NFTABLES, event),
			   flags, family, NFNETLINK_V0, nft_base_seq(net));
	if (!nlh)
		goto nla_put_failure;

	if (nla_put_string(skb, NFTA_TABLE_NAME, table->name) ||
	    nla_put_be32(skb, NFTA_TABLE_USE, htonl(table->use)) ||
	    nla_put_be64(skb, NFTA_TABLE_HANDLE, cpu_to_be64(table->handle),
			 NFTA_TABLE_PAD))
		goto nla_put_failure;

	if (event == NFT_MSG_DELTABLE ||
	    event == NFT_MSG_DESTROYTABLE) {
		nlmsg_end(skb, nlh);
		return 0;
	}

	if (nla_put_be32(skb, NFTA_TABLE_FLAGS,
			 htonl(table->flags & NFT_TABLE_F_MASK)))
		goto nla_put_failure;

	if (nft_table_has_owner(table) &&
	    nla_put_be32(skb, NFTA_TABLE_OWNER, htonl(table->nlpid)))
		goto nla_put_failure;

	if (table->udata) {
		if (nla_put(skb, NFTA_TABLE_USERDATA, table->udlen, table->udata))
			goto nla_put_failure;
	}

	nlmsg_end(skb, nlh);
	return 0;

nla_put_failure:
	nlmsg_trim(skb, nlh);
	return -1;
}

struct nftnl_skb_parms {
	bool report;
};
#define NFT_CB(skb)	(*(struct nftnl_skb_parms*)&((skb)->cb))

static void nft_notify_enqueue(struct sk_buff *skb, bool report,
			       struct list_head *notify_list)
{
	NFT_CB(skb).report = report;
	list_add_tail(&skb->list, notify_list);
}

static void nf_tables_table_notify(const struct nft_ctx *ctx, int event)
{
	struct nftables_pernet *nft_net;
	struct sk_buff *skb;
	u16 flags = 0;
	int err;

	if (!ctx->report &&
	    !nfnetlink_has_listeners(ctx->net, NFNLGRP_NFTABLES))
		return;

	skb = nlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (skb == NULL)
		goto err;

	if (ctx->flags & (NLM_F_CREATE | NLM_F_EXCL))
		flags |= ctx->flags & (NLM_F_CREATE | NLM_F_EXCL);

	err = nf_tables_fill_table_info(skb, ctx->net, ctx->portid, ctx->seq,
					event, flags, ctx->family, ctx->table);
	if (err < 0) {
		kfree_skb(skb);
		goto err;
	}

	nft_net = nft_pernet(ctx->net);
	nft_notify_enqueue(skb, ctx->report, &nft_net->notify_list);
	return;
err:
	nfnetlink_set_err(ctx->net, ctx->portid, NFNLGRP_NFTABLES, -ENOBUFS);
}

static int nf_tables_dump_tables(struct sk_buff *skb,
				 struct netlink_callback *cb)
{
	const struct nfgenmsg *nfmsg = nlmsg_data(cb->nlh);
	struct nftables_pernet *nft_net;
	const struct nft_table *table;
	unsigned int idx = 0, s_idx = cb->args[0];
	struct net *net = sock_net(skb->sk);
	int family = nfmsg->nfgen_family;

	rcu_read_lock();
	nft_net = nft_pernet(net);
	cb->seq = READ_ONCE(nft_net->base_seq);

	list_for_each_entry_rcu(table, &nft_net->tables, list) {
		if (family != NFPROTO_UNSPEC && family != table->family)
			continue;

		if (idx < s_idx)
			goto cont;
		if (idx > s_idx)
			memset(&cb->args[1], 0,
			       sizeof(cb->args) - sizeof(cb->args[0]));
		if (!nft_is_active(net, table))
			continue;
		if (nf_tables_fill_table_info(skb, net,
					      NETLINK_CB(cb->skb).portid,
					      cb->nlh->nlmsg_seq,
					      NFT_MSG_NEWTABLE, NLM_F_MULTI,
					      table->family, table) < 0)
			goto done;

		nl_dump_check_consistent(cb, nlmsg_hdr(skb));
cont:
		idx++;
	}
done:
	rcu_read_unlock();
	cb->args[0] = idx;
	return skb->len;
}

static int nft_netlink_dump_start_rcu(struct sock *nlsk, struct sk_buff *skb,
				      const struct nlmsghdr *nlh,
				      struct netlink_dump_control *c)
{
	int err;

	if (!try_module_get(THIS_MODULE))
		return -EINVAL;

	rcu_read_unlock();
	err = netlink_dump_start(nlsk, skb, nlh, c);
	rcu_read_lock();
	module_put(THIS_MODULE);

	return err;
}

/* called with rcu_read_lock held */
static int nf_tables_gettable(struct sk_buff *skb, const struct nfnl_info *info,
			      const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_cur(info->net);
	u8 family = info->nfmsg->nfgen_family;
	const struct nft_table *table;
	struct net *net = info->net;
	struct sk_buff *skb2;
	int err;

	if (info->nlh->nlmsg_flags & NLM_F_DUMP) {
		struct netlink_dump_control c = {
			.dump = nf_tables_dump_tables,
			.module = THIS_MODULE,
		};

		return nft_netlink_dump_start_rcu(info->sk, skb, info->nlh, &c);
	}

	table = nft_table_lookup(net, nla[NFTA_TABLE_NAME], family, genmask, 0);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_TABLE_NAME]);
		return PTR_ERR(table);
	}

	skb2 = alloc_skb(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb2)
		return -ENOMEM;

	err = nf_tables_fill_table_info(skb2, net, NETLINK_CB(skb).portid,
					info->nlh->nlmsg_seq, NFT_MSG_NEWTABLE,
					0, family, table);
	if (err < 0)
		goto err_fill_table_info;

	return nfnetlink_unicast(skb2, net, NETLINK_CB(skb).portid);

err_fill_table_info:
	kfree_skb(skb2);
	return err;
}

static void nft_table_disable(struct net *net, struct nft_table *table, u32 cnt)
{
	struct nft_chain *chain;
	u32 i = 0;

	list_for_each_entry(chain, &table->chains, list) {
		if (!nft_is_active_next(net, chain))
			continue;
		if (!nft_is_base_chain(chain))
			continue;

		if (cnt && i++ == cnt)
			break;

		nf_tables_unregister_hook(net, table, chain);
	}
}

static int nf_tables_table_enable(struct net *net, struct nft_table *table)
{
	struct nft_chain *chain;
	int err, i = 0;

	list_for_each_entry(chain, &table->chains, list) {
		if (!nft_is_active_next(net, chain))
			continue;
		if (!nft_is_base_chain(chain))
			continue;

		err = nf_tables_register_hook(net, table, chain);
		if (err < 0)
			goto err_register_hooks;

		i++;
	}
	return 0;

err_register_hooks:
	if (i)
		nft_table_disable(net, table, i);
	return err;
}

static void nf_tables_table_disable(struct net *net, struct nft_table *table)
{
	table->flags &= ~NFT_TABLE_F_DORMANT;
	nft_table_disable(net, table, 0);
	table->flags |= NFT_TABLE_F_DORMANT;
}

#define __NFT_TABLE_F_INTERNAL		(NFT_TABLE_F_MASK + 1)
#define __NFT_TABLE_F_WAS_DORMANT	(__NFT_TABLE_F_INTERNAL << 0)
#define __NFT_TABLE_F_WAS_AWAKEN	(__NFT_TABLE_F_INTERNAL << 1)
#define __NFT_TABLE_F_WAS_ORPHAN	(__NFT_TABLE_F_INTERNAL << 2)
#define __NFT_TABLE_F_UPDATE		(__NFT_TABLE_F_WAS_DORMANT | \
					 __NFT_TABLE_F_WAS_AWAKEN | \
					 __NFT_TABLE_F_WAS_ORPHAN)

static bool nft_table_pending_update(const struct nft_ctx *ctx)
{
	struct nftables_pernet *nft_net = nft_pernet(ctx->net);
	struct nft_trans *trans;

	if (ctx->table->flags & __NFT_TABLE_F_UPDATE)
		return true;

	list_for_each_entry(trans, &nft_net->commit_list, list) {
		if (trans->table == ctx->table &&
		    ((trans->msg_type == NFT_MSG_NEWCHAIN &&
		      nft_trans_chain_update(trans)) ||
		     (trans->msg_type == NFT_MSG_DELCHAIN &&
		      nft_is_base_chain(nft_trans_chain(trans)))))
			return true;
	}

	return false;
}

static int nf_tables_updtable(struct nft_ctx *ctx)
{
	struct nft_trans *trans;
	u32 flags;
	int ret;

	if (!ctx->nla[NFTA_TABLE_FLAGS])
		return 0;

	flags = ntohl(nla_get_be32(ctx->nla[NFTA_TABLE_FLAGS]));
	if (flags & ~NFT_TABLE_F_MASK)
		return -EOPNOTSUPP;

	if (flags == (ctx->table->flags & NFT_TABLE_F_MASK))
		return 0;

	if ((nft_table_has_owner(ctx->table) &&
	     !(flags & NFT_TABLE_F_OWNER)) ||
	    (flags & NFT_TABLE_F_OWNER &&
	     !nft_table_is_orphan(ctx->table)))
		return -EOPNOTSUPP;

	if ((flags ^ ctx->table->flags) & NFT_TABLE_F_PERSIST)
		return -EOPNOTSUPP;

	/* No dormant off/on/off/on games in single transaction */
	if (nft_table_pending_update(ctx))
		return -EINVAL;

	trans = nft_trans_alloc(ctx, NFT_MSG_NEWTABLE,
				sizeof(struct nft_trans_table));
	if (trans == NULL)
		return -ENOMEM;

	if ((flags & NFT_TABLE_F_DORMANT) &&
	    !(ctx->table->flags & NFT_TABLE_F_DORMANT)) {
		ctx->table->flags |= NFT_TABLE_F_DORMANT;
		if (!(ctx->table->flags & __NFT_TABLE_F_UPDATE))
			ctx->table->flags |= __NFT_TABLE_F_WAS_AWAKEN;
	} else if (!(flags & NFT_TABLE_F_DORMANT) &&
		   ctx->table->flags & NFT_TABLE_F_DORMANT) {
		ctx->table->flags &= ~NFT_TABLE_F_DORMANT;
		if (!(ctx->table->flags & __NFT_TABLE_F_UPDATE)) {
			ret = nf_tables_table_enable(ctx->net, ctx->table);
			if (ret < 0)
				goto err_register_hooks;

			ctx->table->flags |= __NFT_TABLE_F_WAS_DORMANT;
		}
	}

	if ((flags & NFT_TABLE_F_OWNER) &&
	    !nft_table_has_owner(ctx->table)) {
		ctx->table->nlpid = ctx->portid;
		ctx->table->flags |= NFT_TABLE_F_OWNER |
				     __NFT_TABLE_F_WAS_ORPHAN;
	}

	nft_trans_table_update(trans) = true;
	nft_trans_commit_list_add_tail(ctx->net, trans);

	return 0;

err_register_hooks:
	ctx->table->flags |= NFT_TABLE_F_DORMANT;
	nft_trans_destroy(trans);
	return ret;
}

static u32 nft_chain_hash(const void *data, u32 len, u32 seed)
{
	const char *name = data;

	return jhash(name, strlen(name), seed);
}

static u32 nft_chain_hash_obj(const void *data, u32 len, u32 seed)
{
	const struct nft_chain *chain = data;

	return nft_chain_hash(chain->name, 0, seed);
}

static int nft_chain_hash_cmp(struct rhashtable_compare_arg *arg,
			      const void *ptr)
{
	const struct nft_chain *chain = ptr;
	const char *name = arg->key;

	return strcmp(chain->name, name);
}

static u32 nft_objname_hash(const void *data, u32 len, u32 seed)
{
	const struct nft_object_hash_key *k = data;

	seed ^= hash_ptr(k->table, 32);

	return jhash(k->name, strlen(k->name), seed);
}

static u32 nft_objname_hash_obj(const void *data, u32 len, u32 seed)
{
	const struct nft_object *obj = data;

	return nft_objname_hash(&obj->key, 0, seed);
}

static int nft_objname_hash_cmp(struct rhashtable_compare_arg *arg,
				const void *ptr)
{
	const struct nft_object_hash_key *k = arg->key;
	const struct nft_object *obj = ptr;

	if (obj->key.table != k->table)
		return -1;

	return strcmp(obj->key.name, k->name);
}

static bool nft_supported_family(u8 family)
{
	return false
#ifdef CONFIG_NF_TABLES_INET
		|| family == NFPROTO_INET
#endif
#ifdef CONFIG_NF_TABLES_IPV4
		|| family == NFPROTO_IPV4
#endif
#ifdef CONFIG_NF_TABLES_ARP
		|| family == NFPROTO_ARP
#endif
#ifdef CONFIG_NF_TABLES_NETDEV
		|| family == NFPROTO_NETDEV
#endif
#if IS_ENABLED(CONFIG_NF_TABLES_BRIDGE)
		|| family == NFPROTO_BRIDGE
#endif
#ifdef CONFIG_NF_TABLES_IPV6
		|| family == NFPROTO_IPV6
#endif
		;
}

static int nf_tables_newtable(struct sk_buff *skb, const struct nfnl_info *info,
			      const struct nlattr * const nla[])
{
	struct nftables_pernet *nft_net = nft_pernet(info->net);
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_next(info->net);
	u8 family = info->nfmsg->nfgen_family;
	struct net *net = info->net;
	const struct nlattr *attr;
	struct nft_table *table;
	struct nft_ctx ctx;
	u32 flags = 0;
	int err;

	if (!nft_supported_family(family))
		return -EOPNOTSUPP;

	lockdep_assert_held(&nft_net->commit_mutex);
	attr = nla[NFTA_TABLE_NAME];
	table = nft_table_lookup(net, attr, family, genmask,
				 NETLINK_CB(skb).portid);
	if (IS_ERR(table)) {
		if (PTR_ERR(table) != -ENOENT)
			return PTR_ERR(table);
	} else {
		if (info->nlh->nlmsg_flags & NLM_F_EXCL) {
			NL_SET_BAD_ATTR(extack, attr);
			return -EEXIST;
		}
		if (info->nlh->nlmsg_flags & NLM_F_REPLACE)
			return -EOPNOTSUPP;

		nft_ctx_init(&ctx, net, skb, info->nlh, family, table, NULL, nla);

		return nf_tables_updtable(&ctx);
	}

	if (nla[NFTA_TABLE_FLAGS]) {
		flags = ntohl(nla_get_be32(nla[NFTA_TABLE_FLAGS]));
		if (flags & ~NFT_TABLE_F_MASK)
			return -EOPNOTSUPP;
	}

	err = -ENOMEM;
	table = kzalloc(sizeof(*table), GFP_KERNEL_ACCOUNT);
	if (table == NULL)
		goto err_kzalloc;

	table->validate_state = nft_net->validate_state;
	table->name = nla_strdup(attr, GFP_KERNEL_ACCOUNT);
	if (table->name == NULL)
		goto err_strdup;

	if (nla[NFTA_TABLE_USERDATA]) {
		table->udata = nla_memdup(nla[NFTA_TABLE_USERDATA], GFP_KERNEL_ACCOUNT);
		if (table->udata == NULL)
			goto err_table_udata;

		table->udlen = nla_len(nla[NFTA_TABLE_USERDATA]);
	}

	err = rhltable_init(&table->chains_ht, &nft_chain_ht_params);
	if (err)
		goto err_chain_ht;

	INIT_LIST_HEAD(&table->chains);
	INIT_LIST_HEAD(&table->sets);
	INIT_LIST_HEAD(&table->objects);
	INIT_LIST_HEAD(&table->flowtables);
	table->family = family;
	table->flags = flags;
	table->handle = ++nft_net->table_handle;
	if (table->flags & NFT_TABLE_F_OWNER)
		table->nlpid = NETLINK_CB(skb).portid;

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, NULL, nla);
	err = nft_trans_table_add(&ctx, NFT_MSG_NEWTABLE);
	if (err < 0)
		goto err_trans;

	list_add_tail_rcu(&table->list, &nft_net->tables);
	return 0;
err_trans:
	rhltable_destroy(&table->chains_ht);
err_chain_ht:
	kfree(table->udata);
err_table_udata:
	kfree(table->name);
err_strdup:
	kfree(table);
err_kzalloc:
	return err;
}

static int nft_flush_table(struct nft_ctx *ctx)
{
	struct nft_flowtable *flowtable, *nft;
	struct nft_chain *chain, *nc;
	struct nft_object *obj, *ne;
	struct nft_set *set, *ns;
	int err;

	list_for_each_entry(chain, &ctx->table->chains, list) {
		if (!nft_is_active_next(ctx->net, chain))
			continue;

		if (nft_chain_binding(chain))
			continue;

		ctx->chain = chain;

		err = nft_delrule_by_chain(ctx);
		if (err < 0)
			goto out;
	}

	list_for_each_entry_safe(set, ns, &ctx->table->sets, list) {
		if (!nft_is_active_next(ctx->net, set))
			continue;

		if (nft_set_is_anonymous(set))
			continue;

		err = nft_delset(ctx, set);
		if (err < 0)
			goto out;
	}

	list_for_each_entry_safe(flowtable, nft, &ctx->table->flowtables, list) {
		if (!nft_is_active_next(ctx->net, flowtable))
			continue;

		err = nft_delflowtable(ctx, flowtable);
		if (err < 0)
			goto out;
	}

	list_for_each_entry_safe(obj, ne, &ctx->table->objects, list) {
		if (!nft_is_active_next(ctx->net, obj))
			continue;

		err = nft_delobj(ctx, obj);
		if (err < 0)
			goto out;
	}

	list_for_each_entry_safe(chain, nc, &ctx->table->chains, list) {
		if (!nft_is_active_next(ctx->net, chain))
			continue;

		if (nft_chain_binding(chain))
			continue;

		ctx->chain = chain;

		err = nft_delchain(ctx);
		if (err < 0)
			goto out;
	}

	err = nft_deltable(ctx);
out:
	return err;
}

static int nft_flush(struct nft_ctx *ctx, int family)
{
	struct nftables_pernet *nft_net = nft_pernet(ctx->net);
	const struct nlattr * const *nla = ctx->nla;
	struct nft_table *table, *nt;
	int err = 0;

	list_for_each_entry_safe(table, nt, &nft_net->tables, list) {
		if (family != AF_UNSPEC && table->family != family)
			continue;

		ctx->family = table->family;

		if (!nft_is_active_next(ctx->net, table))
			continue;

		if (nft_table_has_owner(table) && table->nlpid != ctx->portid)
			continue;

		if (nla[NFTA_TABLE_NAME] &&
		    nla_strcmp(nla[NFTA_TABLE_NAME], table->name) != 0)
			continue;

		ctx->table = table;

		err = nft_flush_table(ctx);
		if (err < 0)
			goto out;
	}
out:
	return err;
}

static int nf_tables_deltable(struct sk_buff *skb, const struct nfnl_info *info,
			      const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_next(info->net);
	u8 family = info->nfmsg->nfgen_family;
	struct net *net = info->net;
	const struct nlattr *attr;
	struct nft_table *table;
	struct nft_ctx ctx;

	nft_ctx_init(&ctx, net, skb, info->nlh, 0, NULL, NULL, nla);
	if (family == AF_UNSPEC ||
	    (!nla[NFTA_TABLE_NAME] && !nla[NFTA_TABLE_HANDLE]))
		return nft_flush(&ctx, family);

	if (nla[NFTA_TABLE_HANDLE]) {
		attr = nla[NFTA_TABLE_HANDLE];
		table = nft_table_lookup_byhandle(net, attr, family, genmask,
						  NETLINK_CB(skb).portid);
	} else {
		attr = nla[NFTA_TABLE_NAME];
		table = nft_table_lookup(net, attr, family, genmask,
					 NETLINK_CB(skb).portid);
	}

	if (IS_ERR(table)) {
		if (PTR_ERR(table) == -ENOENT &&
		    NFNL_MSG_TYPE(info->nlh->nlmsg_type) == NFT_MSG_DESTROYTABLE)
			return 0;

		NL_SET_BAD_ATTR(extack, attr);
		return PTR_ERR(table);
	}

	if (info->nlh->nlmsg_flags & NLM_F_NONREC &&
	    table->use > 0)
		return -EBUSY;

	ctx.family = family;
	ctx.table = table;

	return nft_flush_table(&ctx);
}

static void nf_tables_table_destroy(struct nft_table *table)
{
	if (WARN_ON(table->use > 0))
		return;

	rhltable_destroy(&table->chains_ht);
	kfree(table->name);
	kfree(table->udata);
	kfree(table);
}

void nft_register_chain_type(const struct nft_chain_type *ctype)
{
	nfnl_lock(NFNL_SUBSYS_NFTABLES);
	if (WARN_ON(__nft_chain_type_get(ctype->family, ctype->type))) {
		nfnl_unlock(NFNL_SUBSYS_NFTABLES);
		return;
	}
	chain_type[ctype->family][ctype->type] = ctype;
	nfnl_unlock(NFNL_SUBSYS_NFTABLES);
}
EXPORT_SYMBOL_GPL(nft_register_chain_type);

void nft_unregister_chain_type(const struct nft_chain_type *ctype)
{
	nfnl_lock(NFNL_SUBSYS_NFTABLES);
	chain_type[ctype->family][ctype->type] = NULL;
	nfnl_unlock(NFNL_SUBSYS_NFTABLES);
}
EXPORT_SYMBOL_GPL(nft_unregister_chain_type);

/*
 * Chains
 */

static struct nft_chain *
nft_chain_lookup_byhandle(const struct nft_table *table, u64 handle, u8 genmask)
{
	struct nft_chain *chain;

	list_for_each_entry(chain, &table->chains, list) {
		if (chain->handle == handle &&
		    nft_active_genmask(chain, genmask))
			return chain;
	}

	return ERR_PTR(-ENOENT);
}

static bool lockdep_commit_lock_is_held(const struct net *net)
{
#ifdef CONFIG_PROVE_LOCKING
	struct nftables_pernet *nft_net = nft_pernet(net);

	return lockdep_is_held(&nft_net->commit_mutex);
#else
	return true;
#endif
}

static struct nft_chain *nft_chain_lookup(struct net *net,
					  struct nft_table *table,
					  const struct nlattr *nla, u8 genmask)
{
	char search[NFT_CHAIN_MAXNAMELEN + 1];
	struct rhlist_head *tmp, *list;
	struct nft_chain *chain;

	if (nla == NULL)
		return ERR_PTR(-EINVAL);

	nla_strscpy(search, nla, sizeof(search));

	WARN_ON(!rcu_read_lock_held() &&
		!lockdep_commit_lock_is_held(net));

	chain = ERR_PTR(-ENOENT);
	rcu_read_lock();
	list = rhltable_lookup(&table->chains_ht, search, nft_chain_ht_params);
	if (!list)
		goto out_unlock;

	rhl_for_each_entry_rcu(chain, tmp, list, rhlhead) {
		if (nft_active_genmask(chain, genmask))
			goto out_unlock;
	}
	chain = ERR_PTR(-ENOENT);
out_unlock:
	rcu_read_unlock();
	return chain;
}

static const struct nla_policy nft_chain_policy[NFTA_CHAIN_MAX + 1] = {
	[NFTA_CHAIN_TABLE]	= { .type = NLA_STRING,
				    .len = NFT_TABLE_MAXNAMELEN - 1 },
	[NFTA_CHAIN_HANDLE]	= { .type = NLA_U64 },
	[NFTA_CHAIN_NAME]	= { .type = NLA_STRING,
				    .len = NFT_CHAIN_MAXNAMELEN - 1 },
	[NFTA_CHAIN_HOOK]	= { .type = NLA_NESTED },
	[NFTA_CHAIN_POLICY]	= { .type = NLA_U32 },
	[NFTA_CHAIN_TYPE]	= { .type = NLA_STRING,
				    .len = NFT_MODULE_AUTOLOAD_LIMIT },
	[NFTA_CHAIN_COUNTERS]	= { .type = NLA_NESTED },
	[NFTA_CHAIN_FLAGS]	= { .type = NLA_U32 },
	[NFTA_CHAIN_ID]		= { .type = NLA_U32 },
	[NFTA_CHAIN_USERDATA]	= { .type = NLA_BINARY,
				    .len = NFT_USERDATA_MAXLEN },
};

static const struct nla_policy nft_hook_policy[NFTA_HOOK_MAX + 1] = {
	[NFTA_HOOK_HOOKNUM]	= { .type = NLA_U32 },
	[NFTA_HOOK_PRIORITY]	= { .type = NLA_U32 },
	[NFTA_HOOK_DEV]		= { .type = NLA_STRING,
				    .len = IFNAMSIZ - 1 },
};

static int nft_dump_stats(struct sk_buff *skb, struct nft_stats __percpu *stats)
{
	struct nft_stats *cpu_stats, total;
	struct nlattr *nest;
	unsigned int seq;
	u64 pkts, bytes;
	int cpu;

	if (!stats)
		return 0;

	memset(&total, 0, sizeof(total));
	for_each_possible_cpu(cpu) {
		cpu_stats = per_cpu_ptr(stats, cpu);
		do {
			seq = u64_stats_fetch_begin(&cpu_stats->syncp);
			pkts = cpu_stats->pkts;
			bytes = cpu_stats->bytes;
		} while (u64_stats_fetch_retry(&cpu_stats->syncp, seq));
		total.pkts += pkts;
		total.bytes += bytes;
	}
	nest = nla_nest_start_noflag(skb, NFTA_CHAIN_COUNTERS);
	if (nest == NULL)
		goto nla_put_failure;

	if (nla_put_be64(skb, NFTA_COUNTER_PACKETS, cpu_to_be64(total.pkts),
			 NFTA_COUNTER_PAD) ||
	    nla_put_be64(skb, NFTA_COUNTER_BYTES, cpu_to_be64(total.bytes),
			 NFTA_COUNTER_PAD))
		goto nla_put_failure;

	nla_nest_end(skb, nest);
	return 0;

nla_put_failure:
	return -ENOSPC;
}

static int nft_dump_basechain_hook(struct sk_buff *skb,
				   const struct net *net, int family,
				   const struct nft_base_chain *basechain,
				   const struct list_head *hook_list)
{
	const struct nf_hook_ops *ops = &basechain->ops;
	struct nft_hook *hook, *first = NULL;
	struct nlattr *nest, *nest_devs;
	int n = 0;

	nest = nla_nest_start_noflag(skb, NFTA_CHAIN_HOOK);
	if (nest == NULL)
		goto nla_put_failure;
	if (nla_put_be32(skb, NFTA_HOOK_HOOKNUM, htonl(ops->hooknum)))
		goto nla_put_failure;
	if (nla_put_be32(skb, NFTA_HOOK_PRIORITY, htonl(ops->priority)))
		goto nla_put_failure;

	if (nft_base_chain_netdev(family, ops->hooknum)) {
		nest_devs = nla_nest_start_noflag(skb, NFTA_HOOK_DEVS);
		if (!nest_devs)
			goto nla_put_failure;

		if (!hook_list)
			hook_list = &basechain->hook_list;

		list_for_each_entry_rcu(hook, hook_list, list,
					lockdep_commit_lock_is_held(net)) {
			if (!first)
				first = hook;

			if (nla_put(skb, NFTA_DEVICE_NAME,
				    hook->ifnamelen, hook->ifname))
				goto nla_put_failure;
			n++;
		}
		nla_nest_end(skb, nest_devs);

		if (n == 1 &&
		    nla_put(skb, NFTA_HOOK_DEV,
			    first->ifnamelen, first->ifname))
			goto nla_put_failure;
	}
	nla_nest_end(skb, nest);

	return 0;
nla_put_failure:
	return -1;
}

static int nf_tables_fill_chain_info(struct sk_buff *skb, struct net *net,
				     u32 portid, u32 seq, int event, u32 flags,
				     int family, const struct nft_table *table,
				     const struct nft_chain *chain,
				     const struct list_head *hook_list)
{
	struct nlmsghdr *nlh;

	nlh = nfnl_msg_put(skb, portid, seq,
			   nfnl_msg_type(NFNL_SUBSYS_NFTABLES, event),
			   flags, family, NFNETLINK_V0, nft_base_seq(net));
	if (!nlh)
		goto nla_put_failure;

	if (nla_put_string(skb, NFTA_CHAIN_TABLE, table->name) ||
	    nla_put_string(skb, NFTA_CHAIN_NAME, chain->name) ||
	    nla_put_be64(skb, NFTA_CHAIN_HANDLE, cpu_to_be64(chain->handle),
			 NFTA_CHAIN_PAD))
		goto nla_put_failure;

	if (!hook_list &&
	    (event == NFT_MSG_DELCHAIN ||
	     event == NFT_MSG_DESTROYCHAIN)) {
		nlmsg_end(skb, nlh);
		return 0;
	}

	if (nft_is_base_chain(chain)) {
		const struct nft_base_chain *basechain = nft_base_chain(chain);
		struct nft_stats __percpu *stats;

		if (nft_dump_basechain_hook(skb, net, family, basechain, hook_list))
			goto nla_put_failure;

		if (nla_put_be32(skb, NFTA_CHAIN_POLICY,
				 htonl(basechain->policy)))
			goto nla_put_failure;

		if (nla_put_string(skb, NFTA_CHAIN_TYPE, basechain->type->name))
			goto nla_put_failure;

		stats = rcu_dereference_check(basechain->stats,
					      lockdep_commit_lock_is_held(net));
		if (nft_dump_stats(skb, stats))
			goto nla_put_failure;
	}

	if (chain->flags &&
	    nla_put_be32(skb, NFTA_CHAIN_FLAGS, htonl(chain->flags)))
		goto nla_put_failure;

	if (nla_put_be32(skb, NFTA_CHAIN_USE, htonl(chain->use)))
		goto nla_put_failure;

	if (chain->udata &&
	    nla_put(skb, NFTA_CHAIN_USERDATA, chain->udlen, chain->udata))
		goto nla_put_failure;

	nlmsg_end(skb, nlh);
	return 0;

nla_put_failure:
	nlmsg_trim(skb, nlh);
	return -1;
}

static void nf_tables_chain_notify(const struct nft_ctx *ctx, int event,
				   const struct list_head *hook_list)
{
	struct nftables_pernet *nft_net;
	struct sk_buff *skb;
	u16 flags = 0;
	int err;

	if (!ctx->report &&
	    !nfnetlink_has_listeners(ctx->net, NFNLGRP_NFTABLES))
		return;

	skb = nlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (skb == NULL)
		goto err;

	if (ctx->flags & (NLM_F_CREATE | NLM_F_EXCL))
		flags |= ctx->flags & (NLM_F_CREATE | NLM_F_EXCL);

	err = nf_tables_fill_chain_info(skb, ctx->net, ctx->portid, ctx->seq,
					event, flags, ctx->family, ctx->table,
					ctx->chain, hook_list);
	if (err < 0) {
		kfree_skb(skb);
		goto err;
	}

	nft_net = nft_pernet(ctx->net);
	nft_notify_enqueue(skb, ctx->report, &nft_net->notify_list);
	return;
err:
	nfnetlink_set_err(ctx->net, ctx->portid, NFNLGRP_NFTABLES, -ENOBUFS);
}

static int nf_tables_dump_chains(struct sk_buff *skb,
				 struct netlink_callback *cb)
{
	const struct nfgenmsg *nfmsg = nlmsg_data(cb->nlh);
	unsigned int idx = 0, s_idx = cb->args[0];
	struct net *net = sock_net(skb->sk);
	int family = nfmsg->nfgen_family;
	struct nftables_pernet *nft_net;
	const struct nft_table *table;
	const struct nft_chain *chain;

	rcu_read_lock();
	nft_net = nft_pernet(net);
	cb->seq = READ_ONCE(nft_net->base_seq);

	list_for_each_entry_rcu(table, &nft_net->tables, list) {
		if (family != NFPROTO_UNSPEC && family != table->family)
			continue;

		list_for_each_entry_rcu(chain, &table->chains, list) {
			if (idx < s_idx)
				goto cont;
			if (idx > s_idx)
				memset(&cb->args[1], 0,
				       sizeof(cb->args) - sizeof(cb->args[0]));
			if (!nft_is_active(net, chain))
				continue;
			if (nf_tables_fill_chain_info(skb, net,
						      NETLINK_CB(cb->skb).portid,
						      cb->nlh->nlmsg_seq,
						      NFT_MSG_NEWCHAIN,
						      NLM_F_MULTI,
						      table->family, table,
						      chain, NULL) < 0)
				goto done;

			nl_dump_check_consistent(cb, nlmsg_hdr(skb));
cont:
			idx++;
		}
	}
done:
	rcu_read_unlock();
	cb->args[0] = idx;
	return skb->len;
}

/* called with rcu_read_lock held */
static int nf_tables_getchain(struct sk_buff *skb, const struct nfnl_info *info,
			      const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_cur(info->net);
	u8 family = info->nfmsg->nfgen_family;
	const struct nft_chain *chain;
	struct net *net = info->net;
	struct nft_table *table;
	struct sk_buff *skb2;
	int err;

	if (info->nlh->nlmsg_flags & NLM_F_DUMP) {
		struct netlink_dump_control c = {
			.dump = nf_tables_dump_chains,
			.module = THIS_MODULE,
		};

		return nft_netlink_dump_start_rcu(info->sk, skb, info->nlh, &c);
	}

	table = nft_table_lookup(net, nla[NFTA_CHAIN_TABLE], family, genmask, 0);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_CHAIN_TABLE]);
		return PTR_ERR(table);
	}

	chain = nft_chain_lookup(net, table, nla[NFTA_CHAIN_NAME], genmask);
	if (IS_ERR(chain)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_CHAIN_NAME]);
		return PTR_ERR(chain);
	}

	skb2 = alloc_skb(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb2)
		return -ENOMEM;

	err = nf_tables_fill_chain_info(skb2, net, NETLINK_CB(skb).portid,
					info->nlh->nlmsg_seq, NFT_MSG_NEWCHAIN,
					0, family, table, chain, NULL);
	if (err < 0)
		goto err_fill_chain_info;

	return nfnetlink_unicast(skb2, net, NETLINK_CB(skb).portid);

err_fill_chain_info:
	kfree_skb(skb2);
	return err;
}

static const struct nla_policy nft_counter_policy[NFTA_COUNTER_MAX + 1] = {
	[NFTA_COUNTER_PACKETS]	= { .type = NLA_U64 },
	[NFTA_COUNTER_BYTES]	= { .type = NLA_U64 },
};

static struct nft_stats __percpu *nft_stats_alloc(const struct nlattr *attr)
{
	struct nlattr *tb[NFTA_COUNTER_MAX+1];
	struct nft_stats __percpu *newstats;
	struct nft_stats *stats;
	int err;

	err = nla_parse_nested_deprecated(tb, NFTA_COUNTER_MAX, attr,
					  nft_counter_policy, NULL);
	if (err < 0)
		return ERR_PTR_PCPU(err);

	if (!tb[NFTA_COUNTER_BYTES] || !tb[NFTA_COUNTER_PACKETS])
		return ERR_PTR_PCPU(-EINVAL);

	newstats = netdev_alloc_pcpu_stats(struct nft_stats);
	if (newstats == NULL)
		return ERR_PTR_PCPU(-ENOMEM);

	/* Restore old counters on this cpu, no problem. Per-cpu statistics
	 * are not exposed to userspace.
	 */
	preempt_disable();
	stats = this_cpu_ptr(newstats);
	stats->bytes = be64_to_cpu(nla_get_be64(tb[NFTA_COUNTER_BYTES]));
	stats->pkts = be64_to_cpu(nla_get_be64(tb[NFTA_COUNTER_PACKETS]));
	preempt_enable();

	return newstats;
}

static void nft_chain_stats_replace(struct nft_trans_chain *trans)
{
	const struct nft_trans *t = &trans->nft_trans_binding.nft_trans;
	struct nft_base_chain *chain = nft_base_chain(trans->chain);

	if (!trans->stats)
		return;

	trans->stats =
		rcu_replace_pointer(chain->stats, trans->stats,
				    lockdep_commit_lock_is_held(t->net));

	if (!trans->stats)
		static_branch_inc(&nft_counters_enabled);
}

static void nf_tables_chain_free_chain_rules(struct nft_chain *chain)
{
	struct nft_rule_blob *g0 = rcu_dereference_raw(chain->blob_gen_0);
	struct nft_rule_blob *g1 = rcu_dereference_raw(chain->blob_gen_1);

	if (g0 != g1)
		kvfree(g1);
	kvfree(g0);

	/* should be NULL either via abort or via successful commit */
	WARN_ON_ONCE(chain->blob_next);
	kvfree(chain->blob_next);
}

void nf_tables_chain_destroy(struct nft_chain *chain)
{
	const struct nft_table *table = chain->table;
	struct nft_hook *hook, *next;

	if (WARN_ON(chain->use > 0))
		return;

	/* no concurrent access possible anymore */
	nf_tables_chain_free_chain_rules(chain);

	if (nft_is_base_chain(chain)) {
		struct nft_base_chain *basechain = nft_base_chain(chain);

		if (nft_base_chain_netdev(table->family, basechain->ops.hooknum)) {
			list_for_each_entry_safe(hook, next,
						 &basechain->hook_list, list) {
				list_del_rcu(&hook->list);
				nft_netdev_hook_free_rcu(hook);
			}
		}
		module_put(basechain->type->owner);
		if (rcu_access_pointer(basechain->stats)) {
			static_branch_dec(&nft_counters_enabled);
			free_percpu(rcu_dereference_raw(basechain->stats));
		}
		kfree(chain->name);
		kfree(chain->udata);
		kfree(basechain);
	} else {
		kfree(chain->name);
		kfree(chain->udata);
		kfree(chain);
	}
}

static struct nft_hook *nft_netdev_hook_alloc(struct net *net,
					      const struct nlattr *attr)
{
	struct nf_hook_ops *ops;
	struct net_device *dev;
	struct nft_hook *hook;
	int err;

	hook = kzalloc(sizeof(struct nft_hook), GFP_KERNEL_ACCOUNT);
	if (!hook)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&hook->ops_list);

	err = nla_strscpy(hook->ifname, attr, IFNAMSIZ);
	if (err < 0)
		goto err_hook_free;

	hook->ifnamelen = nla_len(attr);

	/* nf_tables_netdev_event() is called under rtnl_mutex, this is
	 * indirectly serializing all the other holders of the commit_mutex with
	 * the rtnl_mutex.
	 */
	for_each_netdev(net, dev) {
		if (strncmp(dev->name, hook->ifname, hook->ifnamelen))
			continue;

		ops = kzalloc(sizeof(struct nf_hook_ops), GFP_KERNEL_ACCOUNT);
		if (!ops) {
			err = -ENOMEM;
			goto err_hook_free;
		}
		ops->dev = dev;
		list_add_tail(&ops->list, &hook->ops_list);
	}
	return hook;

err_hook_free:
	nft_netdev_hook_free(hook);
	return ERR_PTR(err);
}

static struct nft_hook *nft_hook_list_find(struct list_head *hook_list,
					   const struct nft_hook *this)
{
	struct nft_hook *hook;

	list_for_each_entry(hook, hook_list, list) {
		if (!strncmp(hook->ifname, this->ifname,
			     min(hook->ifnamelen, this->ifnamelen)))
			return hook;
	}

	return NULL;
}

static int nf_tables_parse_netdev_hooks(struct net *net,
					const struct nlattr *attr,
					struct list_head *hook_list,
					struct netlink_ext_ack *extack)
{
	struct nft_hook *hook, *next;
	const struct nlattr *tmp;
	int rem, n = 0, err;

	nla_for_each_nested(tmp, attr, rem) {
		if (nla_type(tmp) != NFTA_DEVICE_NAME) {
			err = -EINVAL;
			goto err_hook;
		}

		hook = nft_netdev_hook_alloc(net, tmp);
		if (IS_ERR(hook)) {
			NL_SET_BAD_ATTR(extack, tmp);
			err = PTR_ERR(hook);
			goto err_hook;
		}
		if (nft_hook_list_find(hook_list, hook)) {
			NL_SET_BAD_ATTR(extack, tmp);
			nft_netdev_hook_free(hook);
			err = -EEXIST;
			goto err_hook;
		}
		list_add_tail(&hook->list, hook_list);
		n++;

		if (n == NFT_NETDEVICE_MAX) {
			err = -EFBIG;
			goto err_hook;
		}
	}

	return 0;

err_hook:
	list_for_each_entry_safe(hook, next, hook_list, list) {
		list_del(&hook->list);
		nft_netdev_hook_free(hook);
	}
	return err;
}

struct nft_chain_hook {
	u32				num;
	s32				priority;
	const struct nft_chain_type	*type;
	struct list_head		list;
};

static int nft_chain_parse_netdev(struct net *net, struct nlattr *tb[],
				  struct list_head *hook_list,
				  struct netlink_ext_ack *extack, u32 flags)
{
	struct nft_hook *hook;
	int err;

	if (tb[NFTA_HOOK_DEV]) {
		hook = nft_netdev_hook_alloc(net, tb[NFTA_HOOK_DEV]);
		if (IS_ERR(hook)) {
			NL_SET_BAD_ATTR(extack, tb[NFTA_HOOK_DEV]);
			return PTR_ERR(hook);
		}

		list_add_tail(&hook->list, hook_list);
	} else if (tb[NFTA_HOOK_DEVS]) {
		err = nf_tables_parse_netdev_hooks(net, tb[NFTA_HOOK_DEVS],
						   hook_list, extack);
		if (err < 0)
			return err;

	}

	if (flags & NFT_CHAIN_HW_OFFLOAD &&
	    list_empty(hook_list))
		return -EINVAL;

	return 0;
}

static int nft_chain_parse_hook(struct net *net,
				struct nft_base_chain *basechain,
				const struct nlattr * const nla[],
				struct nft_chain_hook *hook, u8 family,
				u32 flags, struct netlink_ext_ack *extack)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	struct nlattr *ha[NFTA_HOOK_MAX + 1];
	const struct nft_chain_type *type;
	int err;

	lockdep_assert_held(&nft_net->commit_mutex);
	lockdep_nfnl_nft_mutex_not_held();

	err = nla_parse_nested_deprecated(ha, NFTA_HOOK_MAX,
					  nla[NFTA_CHAIN_HOOK],
					  nft_hook_policy, NULL);
	if (err < 0)
		return err;

	if (!basechain) {
		if (!ha[NFTA_HOOK_HOOKNUM] ||
		    !ha[NFTA_HOOK_PRIORITY]) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_CHAIN_NAME]);
			return -ENOENT;
		}

		hook->num = ntohl(nla_get_be32(ha[NFTA_HOOK_HOOKNUM]));
		hook->priority = ntohl(nla_get_be32(ha[NFTA_HOOK_PRIORITY]));

		type = __nft_chain_type_get(family, NFT_CHAIN_T_DEFAULT);
		if (!type)
			return -EOPNOTSUPP;

		if (nla[NFTA_CHAIN_TYPE]) {
			type = nf_tables_chain_type_lookup(net, nla[NFTA_CHAIN_TYPE],
							   family, true);
			if (IS_ERR(type)) {
				NL_SET_BAD_ATTR(extack, nla[NFTA_CHAIN_TYPE]);
				return PTR_ERR(type);
			}
		}
		if (hook->num >= NFT_MAX_HOOKS || !(type->hook_mask & (1 << hook->num)))
			return -EOPNOTSUPP;

		if (type->type == NFT_CHAIN_T_NAT &&
		    hook->priority <= NF_IP_PRI_CONNTRACK)
			return -EOPNOTSUPP;
	} else {
		if (ha[NFTA_HOOK_HOOKNUM]) {
			hook->num = ntohl(nla_get_be32(ha[NFTA_HOOK_HOOKNUM]));
			if (hook->num != basechain->ops.hooknum)
				return -EOPNOTSUPP;
		}
		if (ha[NFTA_HOOK_PRIORITY]) {
			hook->priority = ntohl(nla_get_be32(ha[NFTA_HOOK_PRIORITY]));
			if (hook->priority != basechain->ops.priority)
				return -EOPNOTSUPP;
		}

		if (nla[NFTA_CHAIN_TYPE]) {
			type = __nf_tables_chain_type_lookup(nla[NFTA_CHAIN_TYPE],
							     family);
			if (!type) {
				NL_SET_BAD_ATTR(extack, nla[NFTA_CHAIN_TYPE]);
				return -ENOENT;
			}
		} else {
			type = basechain->type;
		}
	}

	if (!try_module_get(type->owner)) {
		if (nla[NFTA_CHAIN_TYPE])
			NL_SET_BAD_ATTR(extack, nla[NFTA_CHAIN_TYPE]);
		return -ENOENT;
	}

	hook->type = type;

	INIT_LIST_HEAD(&hook->list);
	if (nft_base_chain_netdev(family, hook->num)) {
		err = nft_chain_parse_netdev(net, ha, &hook->list, extack, flags);
		if (err < 0) {
			module_put(type->owner);
			return err;
		}
	} else if (ha[NFTA_HOOK_DEV] || ha[NFTA_HOOK_DEVS]) {
		module_put(type->owner);
		return -EOPNOTSUPP;
	}

	return 0;
}

static void nft_chain_release_hook(struct nft_chain_hook *hook)
{
	struct nft_hook *h, *next;

	list_for_each_entry_safe(h, next, &hook->list, list) {
		list_del(&h->list);
		nft_netdev_hook_free(h);
	}
	module_put(hook->type->owner);
}

static void nft_last_rule(const struct nft_chain *chain, const void *ptr)
{
	struct nft_rule_dp_last *lrule;

	BUILD_BUG_ON(offsetof(struct nft_rule_dp_last, end) != 0);

	lrule = (struct nft_rule_dp_last *)ptr;
	lrule->end.is_last = 1;
	lrule->chain = chain;
	/* blob size does not include the trailer rule */
}

static struct nft_rule_blob *nf_tables_chain_alloc_rules(const struct nft_chain *chain,
							 unsigned int size)
{
	struct nft_rule_blob *blob;

	if (size > INT_MAX)
		return NULL;

	size += sizeof(struct nft_rule_blob) + sizeof(struct nft_rule_dp_last);

	blob = kvmalloc(size, GFP_KERNEL_ACCOUNT);
	if (!blob)
		return NULL;

	blob->size = 0;
	nft_last_rule(chain, blob->data);

	return blob;
}

static void nft_basechain_hook_init(struct nf_hook_ops *ops, u8 family,
				    const struct nft_chain_hook *hook,
				    struct nft_chain *chain)
{
	ops->pf			= family;
	ops->hooknum		= hook->num;
	ops->priority		= hook->priority;
	ops->priv		= chain;
	ops->hook		= hook->type->hooks[ops->hooknum];
	ops->hook_ops_type	= NF_HOOK_OP_NF_TABLES;
}

static int nft_basechain_init(struct nft_base_chain *basechain, u8 family,
			      struct nft_chain_hook *hook, u32 flags)
{
	struct nft_chain *chain;
	struct nf_hook_ops *ops;
	struct nft_hook *h;

	basechain->type = hook->type;
	INIT_LIST_HEAD(&basechain->hook_list);
	chain = &basechain->chain;

	if (nft_base_chain_netdev(family, hook->num)) {
		list_splice_init(&hook->list, &basechain->hook_list);
		list_for_each_entry(h, &basechain->hook_list, list) {
			list_for_each_entry(ops, &h->ops_list, list)
				nft_basechain_hook_init(ops, family, hook, chain);
		}
	}
	nft_basechain_hook_init(&basechain->ops, family, hook, chain);

	chain->flags |= NFT_CHAIN_BASE | flags;
	basechain->policy = NF_ACCEPT;
	if (chain->flags & NFT_CHAIN_HW_OFFLOAD &&
	    !nft_chain_offload_support(basechain)) {
		list_splice_init(&basechain->hook_list, &hook->list);
		return -EOPNOTSUPP;
	}

	flow_block_init(&basechain->flow_block);

	return 0;
}

int nft_chain_add(struct nft_table *table, struct nft_chain *chain)
{
	int err;

	err = rhltable_insert_key(&table->chains_ht, chain->name,
				  &chain->rhlhead, nft_chain_ht_params);
	if (err)
		return err;

	list_add_tail_rcu(&chain->list, &table->chains);

	return 0;
}

static u64 chain_id;

static int nf_tables_addchain(struct nft_ctx *ctx, u8 family, u8 policy,
			      u32 flags, struct netlink_ext_ack *extack)
{
	const struct nlattr * const *nla = ctx->nla;
	struct nft_table *table = ctx->table;
	struct nft_base_chain *basechain;
	struct net *net = ctx->net;
	char name[NFT_NAME_MAXLEN];
	struct nft_rule_blob *blob;
	struct nft_trans *trans;
	struct nft_chain *chain;
	int err;

	if (nla[NFTA_CHAIN_HOOK]) {
		struct nft_stats __percpu *stats = NULL;
		struct nft_chain_hook hook = {};

		if (table->flags & __NFT_TABLE_F_UPDATE)
			return -EINVAL;

		if (flags & NFT_CHAIN_BINDING)
			return -EOPNOTSUPP;

		err = nft_chain_parse_hook(net, NULL, nla, &hook, family, flags,
					   extack);
		if (err < 0)
			return err;

		basechain = kzalloc(sizeof(*basechain), GFP_KERNEL_ACCOUNT);
		if (basechain == NULL) {
			nft_chain_release_hook(&hook);
			return -ENOMEM;
		}
		chain = &basechain->chain;

		if (nla[NFTA_CHAIN_COUNTERS]) {
			stats = nft_stats_alloc(nla[NFTA_CHAIN_COUNTERS]);
			if (IS_ERR_PCPU(stats)) {
				nft_chain_release_hook(&hook);
				kfree(basechain);
				return PTR_ERR_PCPU(stats);
			}
			rcu_assign_pointer(basechain->stats, stats);
		}

		err = nft_basechain_init(basechain, family, &hook, flags);
		if (err < 0) {
			nft_chain_release_hook(&hook);
			kfree(basechain);
			free_percpu(stats);
			return err;
		}
		if (stats)
			static_branch_inc(&nft_counters_enabled);
	} else {
		if (flags & NFT_CHAIN_BASE)
			return -EINVAL;
		if (flags & NFT_CHAIN_HW_OFFLOAD)
			return -EOPNOTSUPP;

		chain = kzalloc(sizeof(*chain), GFP_KERNEL_ACCOUNT);
		if (chain == NULL)
			return -ENOMEM;

		chain->flags = flags;
	}
	ctx->chain = chain;

	INIT_LIST_HEAD(&chain->rules);
	chain->handle = nf_tables_alloc_handle(table);
	chain->table = table;

	if (nla[NFTA_CHAIN_NAME]) {
		chain->name = nla_strdup(nla[NFTA_CHAIN_NAME], GFP_KERNEL_ACCOUNT);
	} else {
		if (!(flags & NFT_CHAIN_BINDING)) {
			err = -EINVAL;
			goto err_destroy_chain;
		}

		snprintf(name, sizeof(name), "__chain%llu", ++chain_id);
		chain->name = kstrdup(name, GFP_KERNEL_ACCOUNT);
	}

	if (!chain->name) {
		err = -ENOMEM;
		goto err_destroy_chain;
	}

	if (nla[NFTA_CHAIN_USERDATA]) {
		chain->udata = nla_memdup(nla[NFTA_CHAIN_USERDATA], GFP_KERNEL_ACCOUNT);
		if (chain->udata == NULL) {
			err = -ENOMEM;
			goto err_destroy_chain;
		}
		chain->udlen = nla_len(nla[NFTA_CHAIN_USERDATA]);
	}

	blob = nf_tables_chain_alloc_rules(chain, 0);
	if (!blob) {
		err = -ENOMEM;
		goto err_destroy_chain;
	}

	RCU_INIT_POINTER(chain->blob_gen_0, blob);
	RCU_INIT_POINTER(chain->blob_gen_1, blob);

	if (!nft_use_inc(&table->use)) {
		err = -EMFILE;
		goto err_destroy_chain;
	}

	trans = nft_trans_chain_add(ctx, NFT_MSG_NEWCHAIN);
	if (IS_ERR(trans)) {
		err = PTR_ERR(trans);
		goto err_trans;
	}

	nft_trans_chain_policy(trans) = NFT_CHAIN_POLICY_UNSET;
	if (nft_is_base_chain(chain))
		nft_trans_chain_policy(trans) = policy;

	err = nft_chain_add(table, chain);
	if (err < 0)
		goto err_chain_add;

	/* This must be LAST to ensure no packets are walking over this chain. */
	err = nf_tables_register_hook(net, table, chain);
	if (err < 0)
		goto err_register_hook;

	return 0;

err_register_hook:
	nft_chain_del(chain);
err_chain_add:
	nft_trans_destroy(trans);
err_trans:
	nft_use_dec_restore(&table->use);
err_destroy_chain:
	nf_tables_chain_destroy(chain);

	return err;
}

static int nf_tables_updchain(struct nft_ctx *ctx, u8 genmask, u8 policy,
			      u32 flags, const struct nlattr *attr,
			      struct netlink_ext_ack *extack)
{
	const struct nlattr * const *nla = ctx->nla;
	struct nft_base_chain *basechain = NULL;
	struct nft_table *table = ctx->table;
	struct nft_chain *chain = ctx->chain;
	struct nft_chain_hook hook = {};
	struct nft_stats __percpu *stats = NULL;
	struct nft_hook *h, *next;
	struct nf_hook_ops *ops;
	struct nft_trans *trans;
	bool unregister = false;
	int err;

	if (chain->flags ^ flags)
		return -EOPNOTSUPP;

	INIT_LIST_HEAD(&hook.list);

	if (nla[NFTA_CHAIN_HOOK]) {
		if (!nft_is_base_chain(chain)) {
			NL_SET_BAD_ATTR(extack, attr);
			return -EEXIST;
		}

		basechain = nft_base_chain(chain);
		err = nft_chain_parse_hook(ctx->net, basechain, nla, &hook,
					   ctx->family, flags, extack);
		if (err < 0)
			return err;

		if (basechain->type != hook.type) {
			nft_chain_release_hook(&hook);
			NL_SET_BAD_ATTR(extack, attr);
			return -EEXIST;
		}

		if (nft_base_chain_netdev(ctx->family, basechain->ops.hooknum)) {
			list_for_each_entry_safe(h, next, &hook.list, list) {
				list_for_each_entry(ops, &h->ops_list, list) {
					ops->pf		= basechain->ops.pf;
					ops->hooknum	= basechain->ops.hooknum;
					ops->priority	= basechain->ops.priority;
					ops->priv	= basechain->ops.priv;
					ops->hook	= basechain->ops.hook;
				}

				if (nft_hook_list_find(&basechain->hook_list, h)) {
					list_del(&h->list);
					nft_netdev_hook_free(h);
				}
			}
		} else {
			ops = &basechain->ops;
			if (ops->hooknum != hook.num ||
			    ops->priority != hook.priority) {
				nft_chain_release_hook(&hook);
				NL_SET_BAD_ATTR(extack, attr);
				return -EEXIST;
			}
		}
	}

	if (nla[NFTA_CHAIN_HANDLE] &&
	    nla[NFTA_CHAIN_NAME]) {
		struct nft_chain *chain2;

		chain2 = nft_chain_lookup(ctx->net, table,
					  nla[NFTA_CHAIN_NAME], genmask);
		if (!IS_ERR(chain2)) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_CHAIN_NAME]);
			err = -EEXIST;
			goto err_hooks;
		}
	}

	if (table->flags & __NFT_TABLE_F_UPDATE &&
	    !list_empty(&hook.list)) {
		NL_SET_BAD_ATTR(extack, attr);
		err = -EOPNOTSUPP;
		goto err_hooks;
	}

	if (!(table->flags & NFT_TABLE_F_DORMANT) &&
	    nft_is_base_chain(chain) &&
	    !list_empty(&hook.list)) {
		basechain = nft_base_chain(chain);
		ops = &basechain->ops;

		if (nft_base_chain_netdev(table->family, basechain->ops.hooknum)) {
			err = nft_netdev_register_hooks(ctx->net, &hook.list);
			if (err < 0)
				goto err_hooks;

			unregister = true;
		}
	}

	if (nla[NFTA_CHAIN_COUNTERS]) {
		if (!nft_is_base_chain(chain)) {
			err = -EOPNOTSUPP;
			goto err_hooks;
		}

		stats = nft_stats_alloc(nla[NFTA_CHAIN_COUNTERS]);
		if (IS_ERR_PCPU(stats)) {
			err = PTR_ERR_PCPU(stats);
			goto err_hooks;
		}
	}

	err = -ENOMEM;
	trans = nft_trans_alloc_chain(ctx, NFT_MSG_NEWCHAIN);
	if (trans == NULL)
		goto err_trans;

	nft_trans_chain_stats(trans) = stats;
	nft_trans_chain_update(trans) = true;

	if (nla[NFTA_CHAIN_POLICY])
		nft_trans_chain_policy(trans) = policy;
	else
		nft_trans_chain_policy(trans) = -1;

	if (nla[NFTA_CHAIN_HANDLE] &&
	    nla[NFTA_CHAIN_NAME]) {
		struct nftables_pernet *nft_net = nft_pernet(ctx->net);
		struct nft_trans *tmp;
		char *name;

		err = -ENOMEM;
		name = nla_strdup(nla[NFTA_CHAIN_NAME], GFP_KERNEL_ACCOUNT);
		if (!name)
			goto err_trans;

		err = -EEXIST;
		list_for_each_entry(tmp, &nft_net->commit_list, list) {
			if (tmp->msg_type == NFT_MSG_NEWCHAIN &&
			    tmp->table == table &&
			    nft_trans_chain_update(tmp) &&
			    nft_trans_chain_name(tmp) &&
			    strcmp(name, nft_trans_chain_name(tmp)) == 0) {
				NL_SET_BAD_ATTR(extack, nla[NFTA_CHAIN_NAME]);
				kfree(name);
				goto err_trans;
			}
		}

		nft_trans_chain_name(trans) = name;
	}

	nft_trans_basechain(trans) = basechain;
	INIT_LIST_HEAD(&nft_trans_chain_hooks(trans));
	list_splice(&hook.list, &nft_trans_chain_hooks(trans));
	if (nla[NFTA_CHAIN_HOOK])
		module_put(hook.type->owner);

	nft_trans_commit_list_add_tail(ctx->net, trans);

	return 0;

err_trans:
	free_percpu(stats);
	kfree(trans);
err_hooks:
	if (nla[NFTA_CHAIN_HOOK]) {
		list_for_each_entry_safe(h, next, &hook.list, list) {
			if (unregister) {
				list_for_each_entry(ops, &h->ops_list, list)
					nf_unregister_net_hook(ctx->net, ops);
			}
			list_del(&h->list);
			nft_netdev_hook_free_rcu(h);
		}
		module_put(hook.type->owner);
	}

	return err;
}

static struct nft_chain *nft_chain_lookup_byid(const struct net *net,
					       const struct nft_table *table,
					       const struct nlattr *nla, u8 genmask)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	u32 id = ntohl(nla_get_be32(nla));
	struct nft_trans *trans;

	list_for_each_entry(trans, &nft_net->commit_list, list) {
		if (trans->msg_type == NFT_MSG_NEWCHAIN &&
		    nft_trans_chain(trans)->table == table &&
		    id == nft_trans_chain_id(trans) &&
		    nft_active_genmask(nft_trans_chain(trans), genmask))
			return nft_trans_chain(trans);
	}
	return ERR_PTR(-ENOENT);
}

static int nf_tables_newchain(struct sk_buff *skb, const struct nfnl_info *info,
			      const struct nlattr * const nla[])
{
	struct nftables_pernet *nft_net = nft_pernet(info->net);
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_next(info->net);
	u8 family = info->nfmsg->nfgen_family;
	struct nft_chain *chain = NULL;
	struct net *net = info->net;
	const struct nlattr *attr;
	struct nft_table *table;
	u8 policy = NF_ACCEPT;
	struct nft_ctx ctx;
	u64 handle = 0;
	u32 flags = 0;

	lockdep_assert_held(&nft_net->commit_mutex);

	table = nft_table_lookup(net, nla[NFTA_CHAIN_TABLE], family, genmask,
				 NETLINK_CB(skb).portid);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_CHAIN_TABLE]);
		return PTR_ERR(table);
	}

	chain = NULL;
	attr = nla[NFTA_CHAIN_NAME];

	if (nla[NFTA_CHAIN_HANDLE]) {
		handle = be64_to_cpu(nla_get_be64(nla[NFTA_CHAIN_HANDLE]));
		chain = nft_chain_lookup_byhandle(table, handle, genmask);
		if (IS_ERR(chain)) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_CHAIN_HANDLE]);
			return PTR_ERR(chain);
		}
		attr = nla[NFTA_CHAIN_HANDLE];
	} else if (nla[NFTA_CHAIN_NAME]) {
		chain = nft_chain_lookup(net, table, attr, genmask);
		if (IS_ERR(chain)) {
			if (PTR_ERR(chain) != -ENOENT) {
				NL_SET_BAD_ATTR(extack, attr);
				return PTR_ERR(chain);
			}
			chain = NULL;
		}
	} else if (!nla[NFTA_CHAIN_ID]) {
		return -EINVAL;
	}

	if (nla[NFTA_CHAIN_POLICY]) {
		if (chain != NULL &&
		    !nft_is_base_chain(chain)) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_CHAIN_POLICY]);
			return -EOPNOTSUPP;
		}

		if (chain == NULL &&
		    nla[NFTA_CHAIN_HOOK] == NULL) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_CHAIN_POLICY]);
			return -EOPNOTSUPP;
		}

		policy = ntohl(nla_get_be32(nla[NFTA_CHAIN_POLICY]));
		switch (policy) {
		case NF_DROP:
		case NF_ACCEPT:
			break;
		default:
			return -EINVAL;
		}
	}

	if (nla[NFTA_CHAIN_FLAGS])
		flags = ntohl(nla_get_be32(nla[NFTA_CHAIN_FLAGS]));
	else if (chain)
		flags = chain->flags;

	if (flags & ~NFT_CHAIN_FLAGS)
		return -EOPNOTSUPP;

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, chain, nla);

	if (chain != NULL) {
		if (chain->flags & NFT_CHAIN_BINDING)
			return -EINVAL;

		if (info->nlh->nlmsg_flags & NLM_F_EXCL) {
			NL_SET_BAD_ATTR(extack, attr);
			return -EEXIST;
		}
		if (info->nlh->nlmsg_flags & NLM_F_REPLACE)
			return -EOPNOTSUPP;

		flags |= chain->flags & NFT_CHAIN_BASE;
		return nf_tables_updchain(&ctx, genmask, policy, flags, attr,
					  extack);
	}

	return nf_tables_addchain(&ctx, family, policy, flags, extack);
}

static int nft_delchain_hook(struct nft_ctx *ctx,
			     struct nft_base_chain *basechain,
			     struct netlink_ext_ack *extack)
{
	const struct nft_chain *chain = &basechain->chain;
	const struct nlattr * const *nla = ctx->nla;
	struct nft_chain_hook chain_hook = {};
	struct nft_hook *this, *hook;
	LIST_HEAD(chain_del_list);
	struct nft_trans *trans;
	int err;

	if (ctx->table->flags & __NFT_TABLE_F_UPDATE)
		return -EOPNOTSUPP;

	err = nft_chain_parse_hook(ctx->net, basechain, nla, &chain_hook,
				   ctx->family, chain->flags, extack);
	if (err < 0)
		return err;

	list_for_each_entry(this, &chain_hook.list, list) {
		hook = nft_hook_list_find(&basechain->hook_list, this);
		if (!hook) {
			err = -ENOENT;
			goto err_chain_del_hook;
		}
		list_move(&hook->list, &chain_del_list);
	}

	trans = nft_trans_alloc_chain(ctx, NFT_MSG_DELCHAIN);
	if (!trans) {
		err = -ENOMEM;
		goto err_chain_del_hook;
	}

	nft_trans_basechain(trans) = basechain;
	nft_trans_chain_update(trans) = true;
	INIT_LIST_HEAD(&nft_trans_chain_hooks(trans));
	list_splice(&chain_del_list, &nft_trans_chain_hooks(trans));
	nft_chain_release_hook(&chain_hook);

	nft_trans_commit_list_add_tail(ctx->net, trans);

	return 0;

err_chain_del_hook:
	list_splice(&chain_del_list, &basechain->hook_list);
	nft_chain_release_hook(&chain_hook);

	return err;
}

static int nf_tables_delchain(struct sk_buff *skb, const struct nfnl_info *info,
			      const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_next(info->net);
	u8 family = info->nfmsg->nfgen_family;
	struct net *net = info->net;
	const struct nlattr *attr;
	struct nft_table *table;
	struct nft_chain *chain;
	struct nft_rule *rule;
	struct nft_ctx ctx;
	u64 handle;
	u32 use;
	int err;

	table = nft_table_lookup(net, nla[NFTA_CHAIN_TABLE], family, genmask,
				 NETLINK_CB(skb).portid);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_CHAIN_TABLE]);
		return PTR_ERR(table);
	}

	if (nla[NFTA_CHAIN_HANDLE]) {
		attr = nla[NFTA_CHAIN_HANDLE];
		handle = be64_to_cpu(nla_get_be64(attr));
		chain = nft_chain_lookup_byhandle(table, handle, genmask);
	} else {
		attr = nla[NFTA_CHAIN_NAME];
		chain = nft_chain_lookup(net, table, attr, genmask);
	}
	if (IS_ERR(chain)) {
		if (PTR_ERR(chain) == -ENOENT &&
		    NFNL_MSG_TYPE(info->nlh->nlmsg_type) == NFT_MSG_DESTROYCHAIN)
			return 0;

		NL_SET_BAD_ATTR(extack, attr);
		return PTR_ERR(chain);
	}

	if (nft_chain_binding(chain))
		return -EOPNOTSUPP;

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, chain, nla);

	if (nla[NFTA_CHAIN_HOOK]) {
		if (NFNL_MSG_TYPE(info->nlh->nlmsg_type) == NFT_MSG_DESTROYCHAIN ||
		    chain->flags & NFT_CHAIN_HW_OFFLOAD)
			return -EOPNOTSUPP;

		if (nft_is_base_chain(chain)) {
			struct nft_base_chain *basechain = nft_base_chain(chain);

			if (nft_base_chain_netdev(table->family, basechain->ops.hooknum))
				return nft_delchain_hook(&ctx, basechain, extack);
		}
	}

	if (info->nlh->nlmsg_flags & NLM_F_NONREC &&
	    chain->use > 0)
		return -EBUSY;

	use = chain->use;
	list_for_each_entry(rule, &chain->rules, list) {
		if (!nft_is_active_next(net, rule))
			continue;
		use--;

		err = nft_delrule(&ctx, rule);
		if (err < 0)
			return err;
	}

	/* There are rules and elements that are still holding references to us,
	 * we cannot do a recursive removal in this case.
	 */
	if (use > 0) {
		NL_SET_BAD_ATTR(extack, attr);
		return -EBUSY;
	}

	return nft_delchain(&ctx);
}

/*
 * Expressions
 */

/**
 *	nft_register_expr - register nf_tables expr type
 *	@type: expr type
 *
 *	Registers the expr type for use with nf_tables. Returns zero on
 *	success or a negative errno code otherwise.
 */
int nft_register_expr(struct nft_expr_type *type)
{
	if (WARN_ON_ONCE(type->maxattr > NFT_EXPR_MAXATTR))
		return -ENOMEM;

	nfnl_lock(NFNL_SUBSYS_NFTABLES);
	if (type->family == NFPROTO_UNSPEC)
		list_add_tail_rcu(&type->list, &nf_tables_expressions);
	else
		list_add_rcu(&type->list, &nf_tables_expressions);
	nfnl_unlock(NFNL_SUBSYS_NFTABLES);
	return 0;
}
EXPORT_SYMBOL_GPL(nft_register_expr);

/**
 *	nft_unregister_expr - unregister nf_tables expr type
 *	@type: expr type
 *
 * 	Unregisters the expr typefor use with nf_tables.
 */
void nft_unregister_expr(struct nft_expr_type *type)
{
	nfnl_lock(NFNL_SUBSYS_NFTABLES);
	list_del_rcu(&type->list);
	nfnl_unlock(NFNL_SUBSYS_NFTABLES);
}
EXPORT_SYMBOL_GPL(nft_unregister_expr);

static const struct nft_expr_type *__nft_expr_type_get(u8 family,
						       struct nlattr *nla)
{
	const struct nft_expr_type *type, *candidate = NULL;

	list_for_each_entry_rcu(type, &nf_tables_expressions, list) {
		if (!nla_strcmp(nla, type->name)) {
			if (!type->family && !candidate)
				candidate = type;
			else if (type->family == family)
				candidate = type;
		}
	}
	return candidate;
}

#ifdef CONFIG_MODULES
static int nft_expr_type_request_module(struct net *net, u8 family,
					struct nlattr *nla)
{
	if (nft_request_module(net, "nft-expr-%u-%.*s", family,
			       nla_len(nla), (char *)nla_data(nla)) == -EAGAIN)
		return -EAGAIN;

	return 0;
}
#endif

static const struct nft_expr_type *nft_expr_type_get(struct net *net,
						     u8 family,
						     struct nlattr *nla)
{
	const struct nft_expr_type *type;

	if (nla == NULL)
		return ERR_PTR(-EINVAL);

	rcu_read_lock();
	type = __nft_expr_type_get(family, nla);
	if (type != NULL && try_module_get(type->owner)) {
		rcu_read_unlock();
		return type;
	}
	rcu_read_unlock();

	lockdep_nfnl_nft_mutex_not_held();
#ifdef CONFIG_MODULES
	if (type == NULL) {
		if (nft_expr_type_request_module(net, family, nla) == -EAGAIN)
			return ERR_PTR(-EAGAIN);

		if (nft_request_module(net, "nft-expr-%.*s",
				       nla_len(nla),
				       (char *)nla_data(nla)) == -EAGAIN)
			return ERR_PTR(-EAGAIN);
	}
#endif
	return ERR_PTR(-ENOENT);
}

static const struct nla_policy nft_expr_policy[NFTA_EXPR_MAX + 1] = {
	[NFTA_EXPR_NAME]	= { .type = NLA_STRING,
				    .len = NFT_MODULE_AUTOLOAD_LIMIT },
	[NFTA_EXPR_DATA]	= { .type = NLA_NESTED },
};

static int nf_tables_fill_expr_info(struct sk_buff *skb,
				    const struct nft_expr *expr, bool reset)
{
	if (nla_put_string(skb, NFTA_EXPR_NAME, expr->ops->type->name))
		goto nla_put_failure;

	if (expr->ops->dump) {
		struct nlattr *data = nla_nest_start_noflag(skb,
							    NFTA_EXPR_DATA);
		if (data == NULL)
			goto nla_put_failure;
		if (expr->ops->dump(skb, expr, reset) < 0)
			goto nla_put_failure;
		nla_nest_end(skb, data);
	}

	return skb->len;

nla_put_failure:
	return -1;
};

int nft_expr_dump(struct sk_buff *skb, unsigned int attr,
		  const struct nft_expr *expr, bool reset)
{
	struct nlattr *nest;

	nest = nla_nest_start_noflag(skb, attr);
	if (!nest)
		goto nla_put_failure;
	if (nf_tables_fill_expr_info(skb, expr, reset) < 0)
		goto nla_put_failure;
	nla_nest_end(skb, nest);
	return 0;

nla_put_failure:
	return -1;
}

struct nft_expr_info {
	const struct nft_expr_ops	*ops;
	const struct nlattr		*attr;
	struct nlattr			*tb[NFT_EXPR_MAXATTR + 1];
};

static int nf_tables_expr_parse(const struct nft_ctx *ctx,
				const struct nlattr *nla,
				struct nft_expr_info *info)
{
	const struct nft_expr_type *type;
	const struct nft_expr_ops *ops;
	struct nlattr *tb[NFTA_EXPR_MAX + 1];
	int err;

	err = nla_parse_nested_deprecated(tb, NFTA_EXPR_MAX, nla,
					  nft_expr_policy, NULL);
	if (err < 0)
		return err;

	type = nft_expr_type_get(ctx->net, ctx->family, tb[NFTA_EXPR_NAME]);
	if (IS_ERR(type))
		return PTR_ERR(type);

	if (tb[NFTA_EXPR_DATA]) {
		err = nla_parse_nested_deprecated(info->tb, type->maxattr,
						  tb[NFTA_EXPR_DATA],
						  type->policy, NULL);
		if (err < 0)
			goto err1;
	} else
		memset(info->tb, 0, sizeof(info->tb[0]) * (type->maxattr + 1));

	if (type->select_ops != NULL) {
		ops = type->select_ops(ctx,
				       (const struct nlattr * const *)info->tb);
		if (IS_ERR(ops)) {
			err = PTR_ERR(ops);
#ifdef CONFIG_MODULES
			if (err == -EAGAIN)
				if (nft_expr_type_request_module(ctx->net,
								 ctx->family,
								 tb[NFTA_EXPR_NAME]) != -EAGAIN)
					err = -ENOENT;
#endif
			goto err1;
		}
	} else
		ops = type->ops;

	info->attr = nla;
	info->ops = ops;

	return 0;

err1:
	module_put(type->owner);
	return err;
}

int nft_expr_inner_parse(const struct nft_ctx *ctx, const struct nlattr *nla,
			 struct nft_expr_info *info)
{
	struct nlattr *tb[NFTA_EXPR_MAX + 1];
	const struct nft_expr_type *type;
	int err;

	err = nla_parse_nested_deprecated(tb, NFTA_EXPR_MAX, nla,
					  nft_expr_policy, NULL);
	if (err < 0)
		return err;

	if (!tb[NFTA_EXPR_DATA] || !tb[NFTA_EXPR_NAME])
		return -EINVAL;

	rcu_read_lock();

	type = __nft_expr_type_get(ctx->family, tb[NFTA_EXPR_NAME]);
	if (!type) {
		err = -ENOENT;
		goto out_unlock;
	}

	if (!type->inner_ops) {
		err = -EOPNOTSUPP;
		goto out_unlock;
	}

	err = nla_parse_nested_deprecated(info->tb, type->maxattr,
					  tb[NFTA_EXPR_DATA],
					  type->policy, NULL);
	if (err < 0)
		goto out_unlock;

	info->attr = nla;
	info->ops = type->inner_ops;

	/* No module reference will be taken on type->owner.
	 * Presence of type->inner_ops implies that the expression
	 * is builtin, so it cannot go away.
	 */
	rcu_read_unlock();
	return 0;

out_unlock:
	rcu_read_unlock();
	return err;
}

static int nf_tables_newexpr(const struct nft_ctx *ctx,
			     const struct nft_expr_info *expr_info,
			     struct nft_expr *expr)
{
	const struct nft_expr_ops *ops = expr_info->ops;
	int err;

	expr->ops = ops;
	if (ops->init) {
		err = ops->init(ctx, expr, (const struct nlattr **)expr_info->tb);
		if (err < 0)
			goto err1;
	}

	return 0;
err1:
	expr->ops = NULL;
	return err;
}

static void nf_tables_expr_destroy(const struct nft_ctx *ctx,
				   struct nft_expr *expr)
{
	const struct nft_expr_type *type = expr->ops->type;

	if (expr->ops->destroy)
		expr->ops->destroy(ctx, expr);
	module_put(type->owner);
}

static struct nft_expr *nft_expr_init(const struct nft_ctx *ctx,
				      const struct nlattr *nla)
{
	struct nft_expr_info expr_info;
	struct nft_expr *expr;
	struct module *owner;
	int err;

	err = nf_tables_expr_parse(ctx, nla, &expr_info);
	if (err < 0)
		goto err_expr_parse;

	err = -EOPNOTSUPP;
	if (!(expr_info.ops->type->flags & NFT_EXPR_STATEFUL))
		goto err_expr_stateful;

	err = -ENOMEM;
	expr = kzalloc(expr_info.ops->size, GFP_KERNEL_ACCOUNT);
	if (expr == NULL)
		goto err_expr_stateful;

	err = nf_tables_newexpr(ctx, &expr_info, expr);
	if (err < 0)
		goto err_expr_new;

	return expr;
err_expr_new:
	kfree(expr);
err_expr_stateful:
	owner = expr_info.ops->type->owner;
	if (expr_info.ops->type->release_ops)
		expr_info.ops->type->release_ops(expr_info.ops);

	module_put(owner);
err_expr_parse:
	return ERR_PTR(err);
}

int nft_expr_clone(struct nft_expr *dst, struct nft_expr *src, gfp_t gfp)
{
	int err;

	if (WARN_ON_ONCE(!src->ops->clone))
		return -EINVAL;

	dst->ops = src->ops;
	err = src->ops->clone(dst, src, gfp);
	if (err < 0)
		return err;

	__module_get(src->ops->type->owner);

	return 0;
}

void nft_expr_destroy(const struct nft_ctx *ctx, struct nft_expr *expr)
{
	nf_tables_expr_destroy(ctx, expr);
	kfree(expr);
}

/*
 * Rules
 */

static struct nft_rule *__nft_rule_lookup(const struct net *net,
					  const struct nft_chain *chain,
					  u64 handle)
{
	struct nft_rule *rule;

	// FIXME: this sucks
	list_for_each_entry_rcu(rule, &chain->rules, list,
				lockdep_commit_lock_is_held(net)) {
		if (handle == rule->handle)
			return rule;
	}

	return ERR_PTR(-ENOENT);
}

static struct nft_rule *nft_rule_lookup(const struct net *net,
					const struct nft_chain *chain,
					const struct nlattr *nla)
{
	if (nla == NULL)
		return ERR_PTR(-EINVAL);

	return __nft_rule_lookup(net, chain, be64_to_cpu(nla_get_be64(nla)));
}

static const struct nla_policy nft_rule_policy[NFTA_RULE_MAX + 1] = {
	[NFTA_RULE_TABLE]	= { .type = NLA_STRING,
				    .len = NFT_TABLE_MAXNAMELEN - 1 },
	[NFTA_RULE_CHAIN]	= { .type = NLA_STRING,
				    .len = NFT_CHAIN_MAXNAMELEN - 1 },
	[NFTA_RULE_HANDLE]	= { .type = NLA_U64 },
	[NFTA_RULE_EXPRESSIONS]	= NLA_POLICY_NESTED_ARRAY(nft_expr_policy),
	[NFTA_RULE_COMPAT]	= { .type = NLA_NESTED },
	[NFTA_RULE_POSITION]	= { .type = NLA_U64 },
	[NFTA_RULE_USERDATA]	= { .type = NLA_BINARY,
				    .len = NFT_USERDATA_MAXLEN },
	[NFTA_RULE_ID]		= { .type = NLA_U32 },
	[NFTA_RULE_POSITION_ID]	= { .type = NLA_U32 },
	[NFTA_RULE_CHAIN_ID]	= { .type = NLA_U32 },
};

static int nf_tables_fill_rule_info(struct sk_buff *skb, struct net *net,
				    u32 portid, u32 seq, int event,
				    u32 flags, int family,
				    const struct nft_table *table,
				    const struct nft_chain *chain,
				    const struct nft_rule *rule, u64 handle,
				    bool reset)
{
	struct nlmsghdr *nlh;
	const struct nft_expr *expr, *next;
	struct nlattr *list;
	u16 type = nfnl_msg_type(NFNL_SUBSYS_NFTABLES, event);

	nlh = nfnl_msg_put(skb, portid, seq, type, flags, family, NFNETLINK_V0,
			   nft_base_seq(net));
	if (!nlh)
		goto nla_put_failure;

	if (nla_put_string(skb, NFTA_RULE_TABLE, table->name))
		goto nla_put_failure;
	if (nla_put_string(skb, NFTA_RULE_CHAIN, chain->name))
		goto nla_put_failure;
	if (nla_put_be64(skb, NFTA_RULE_HANDLE, cpu_to_be64(rule->handle),
			 NFTA_RULE_PAD))
		goto nla_put_failure;

	if (event != NFT_MSG_DELRULE && handle) {
		if (nla_put_be64(skb, NFTA_RULE_POSITION, cpu_to_be64(handle),
				 NFTA_RULE_PAD))
			goto nla_put_failure;
	}

	if (chain->flags & NFT_CHAIN_HW_OFFLOAD)
		nft_flow_rule_stats(chain, rule);

	list = nla_nest_start_noflag(skb, NFTA_RULE_EXPRESSIONS);
	if (list == NULL)
		goto nla_put_failure;
	nft_rule_for_each_expr(expr, next, rule) {
		if (nft_expr_dump(skb, NFTA_LIST_ELEM, expr, reset) < 0)
			goto nla_put_failure;
	}
	nla_nest_end(skb, list);

	if (rule->udata) {
		struct nft_userdata *udata = nft_userdata(rule);
		if (nla_put(skb, NFTA_RULE_USERDATA, udata->len + 1,
			    udata->data) < 0)
			goto nla_put_failure;
	}

	nlmsg_end(skb, nlh);
	return 0;

nla_put_failure:
	nlmsg_trim(skb, nlh);
	return -1;
}

static void nf_tables_rule_notify(const struct nft_ctx *ctx,
				  const struct nft_rule *rule, int event)
{
	struct nftables_pernet *nft_net = nft_pernet(ctx->net);
	const struct nft_rule *prule;
	struct sk_buff *skb;
	u64 handle = 0;
	u16 flags = 0;
	int err;

	if (!ctx->report &&
	    !nfnetlink_has_listeners(ctx->net, NFNLGRP_NFTABLES))
		return;

	skb = nlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (skb == NULL)
		goto err;

	if (event == NFT_MSG_NEWRULE &&
	    !list_is_first(&rule->list, &ctx->chain->rules) &&
	    !list_is_last(&rule->list, &ctx->chain->rules)) {
		prule = list_prev_entry(rule, list);
		handle = prule->handle;
	}
	if (ctx->flags & (NLM_F_APPEND | NLM_F_REPLACE))
		flags |= NLM_F_APPEND;
	if (ctx->flags & (NLM_F_CREATE | NLM_F_EXCL))
		flags |= ctx->flags & (NLM_F_CREATE | NLM_F_EXCL);

	err = nf_tables_fill_rule_info(skb, ctx->net, ctx->portid, ctx->seq,
				       event, flags, ctx->family, ctx->table,
				       ctx->chain, rule, handle, false);
	if (err < 0) {
		kfree_skb(skb);
		goto err;
	}

	nft_notify_enqueue(skb, ctx->report, &nft_net->notify_list);
	return;
err:
	nfnetlink_set_err(ctx->net, ctx->portid, NFNLGRP_NFTABLES, -ENOBUFS);
}

static void audit_log_rule_reset(const struct nft_table *table,
				 unsigned int base_seq,
				 unsigned int nentries)
{
	char *buf = kasprintf(GFP_ATOMIC, "%s:%u",
			      table->name, base_seq);

	audit_log_nfcfg(buf, table->family, nentries,
			AUDIT_NFT_OP_RULE_RESET, GFP_ATOMIC);
	kfree(buf);
}

struct nft_rule_dump_ctx {
	unsigned int s_idx;
	char *table;
	char *chain;
	bool reset;
};

static int __nf_tables_dump_rules(struct sk_buff *skb,
				  unsigned int *idx,
				  struct netlink_callback *cb,
				  const struct nft_table *table,
				  const struct nft_chain *chain)
{
	struct nft_rule_dump_ctx *ctx = (void *)cb->ctx;
	struct net *net = sock_net(skb->sk);
	const struct nft_rule *rule, *prule;
	unsigned int entries = 0;
	int ret = 0;
	u64 handle;

	prule = NULL;
	list_for_each_entry_rcu(rule, &chain->rules, list) {
		if (!nft_is_active(net, rule))
			goto cont_skip;
		if (*idx < ctx->s_idx)
			goto cont;
		if (prule)
			handle = prule->handle;
		else
			handle = 0;

		if (nf_tables_fill_rule_info(skb, net, NETLINK_CB(cb->skb).portid,
					cb->nlh->nlmsg_seq,
					NFT_MSG_NEWRULE,
					NLM_F_MULTI | NLM_F_APPEND,
					table->family,
					table, chain, rule, handle, ctx->reset) < 0) {
			ret = 1;
			break;
		}
		entries++;
		nl_dump_check_consistent(cb, nlmsg_hdr(skb));
cont:
		prule = rule;
cont_skip:
		(*idx)++;
	}

	if (ctx->reset && entries)
		audit_log_rule_reset(table, cb->seq, entries);

	return ret;
}

static int nf_tables_dump_rules(struct sk_buff *skb,
				struct netlink_callback *cb)
{
	const struct nfgenmsg *nfmsg = nlmsg_data(cb->nlh);
	struct nft_rule_dump_ctx *ctx = (void *)cb->ctx;
	struct nft_table *table;
	const struct nft_chain *chain;
	unsigned int idx = 0;
	struct net *net = sock_net(skb->sk);
	int family = nfmsg->nfgen_family;
	struct nftables_pernet *nft_net;

	rcu_read_lock();
	nft_net = nft_pernet(net);
	cb->seq = READ_ONCE(nft_net->base_seq);

	list_for_each_entry_rcu(table, &nft_net->tables, list) {
		if (family != NFPROTO_UNSPEC && family != table->family)
			continue;

		if (ctx->table && strcmp(ctx->table, table->name) != 0)
			continue;

		if (ctx->table && ctx->chain) {
			struct rhlist_head *list, *tmp;

			list = rhltable_lookup(&table->chains_ht, ctx->chain,
					       nft_chain_ht_params);
			if (!list)
				goto done;

			rhl_for_each_entry_rcu(chain, tmp, list, rhlhead) {
				if (!nft_is_active(net, chain))
					continue;
				__nf_tables_dump_rules(skb, &idx,
						       cb, table, chain);
				break;
			}
			goto done;
		}

		list_for_each_entry_rcu(chain, &table->chains, list) {
			if (__nf_tables_dump_rules(skb, &idx,
						   cb, table, chain))
				goto done;
		}

		if (ctx->table)
			break;
	}
done:
	rcu_read_unlock();

	ctx->s_idx = idx;
	return skb->len;
}

static int nf_tables_dumpreset_rules(struct sk_buff *skb,
				     struct netlink_callback *cb)
{
	struct nftables_pernet *nft_net = nft_pernet(sock_net(skb->sk));
	int ret;

	/* Mutex is held is to prevent that two concurrent dump-and-reset calls
	 * do not underrun counters and quotas. The commit_mutex is used for
	 * the lack a better lock, this is not transaction path.
	 */
	mutex_lock(&nft_net->commit_mutex);
	ret = nf_tables_dump_rules(skb, cb);
	mutex_unlock(&nft_net->commit_mutex);

	return ret;
}

static int nf_tables_dump_rules_start(struct netlink_callback *cb)
{
	struct nft_rule_dump_ctx *ctx = (void *)cb->ctx;
	const struct nlattr * const *nla = cb->data;

	BUILD_BUG_ON(sizeof(*ctx) > sizeof(cb->ctx));

	if (nla[NFTA_RULE_TABLE]) {
		ctx->table = nla_strdup(nla[NFTA_RULE_TABLE], GFP_ATOMIC);
		if (!ctx->table)
			return -ENOMEM;
	}
	if (nla[NFTA_RULE_CHAIN]) {
		ctx->chain = nla_strdup(nla[NFTA_RULE_CHAIN], GFP_ATOMIC);
		if (!ctx->chain) {
			kfree(ctx->table);
			return -ENOMEM;
		}
	}
	return 0;
}

static int nf_tables_dumpreset_rules_start(struct netlink_callback *cb)
{
	struct nft_rule_dump_ctx *ctx = (void *)cb->ctx;

	ctx->reset = true;

	return nf_tables_dump_rules_start(cb);
}

static int nf_tables_dump_rules_done(struct netlink_callback *cb)
{
	struct nft_rule_dump_ctx *ctx = (void *)cb->ctx;

	kfree(ctx->table);
	kfree(ctx->chain);
	return 0;
}

/* Caller must hold rcu read lock or transaction mutex */
static struct sk_buff *
nf_tables_getrule_single(u32 portid, const struct nfnl_info *info,
			 const struct nlattr * const nla[], bool reset)
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_cur(info->net);
	u8 family = info->nfmsg->nfgen_family;
	const struct nft_chain *chain;
	const struct nft_rule *rule;
	struct net *net = info->net;
	struct nft_table *table;
	struct sk_buff *skb2;
	int err;

	table = nft_table_lookup(net, nla[NFTA_RULE_TABLE], family, genmask, 0);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_TABLE]);
		return ERR_CAST(table);
	}

	chain = nft_chain_lookup(net, table, nla[NFTA_RULE_CHAIN], genmask);
	if (IS_ERR(chain)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_CHAIN]);
		return ERR_CAST(chain);
	}

	rule = nft_rule_lookup(net, chain, nla[NFTA_RULE_HANDLE]);
	if (IS_ERR(rule)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_HANDLE]);
		return ERR_CAST(rule);
	}

	skb2 = alloc_skb(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb2)
		return ERR_PTR(-ENOMEM);

	err = nf_tables_fill_rule_info(skb2, net, portid,
				       info->nlh->nlmsg_seq, NFT_MSG_NEWRULE, 0,
				       family, table, chain, rule, 0, reset);
	if (err < 0) {
		kfree_skb(skb2);
		return ERR_PTR(err);
	}

	return skb2;
}

static int nf_tables_getrule(struct sk_buff *skb, const struct nfnl_info *info,
			     const struct nlattr * const nla[])
{
	u32 portid = NETLINK_CB(skb).portid;
	struct net *net = info->net;
	struct sk_buff *skb2;

	if (info->nlh->nlmsg_flags & NLM_F_DUMP) {
		struct netlink_dump_control c = {
			.start= nf_tables_dump_rules_start,
			.dump = nf_tables_dump_rules,
			.done = nf_tables_dump_rules_done,
			.module = THIS_MODULE,
			.data = (void *)nla,
		};

		return nft_netlink_dump_start_rcu(info->sk, skb, info->nlh, &c);
	}

	skb2 = nf_tables_getrule_single(portid, info, nla, false);
	if (IS_ERR(skb2))
		return PTR_ERR(skb2);

	return nfnetlink_unicast(skb2, net, portid);
}

static int nf_tables_getrule_reset(struct sk_buff *skb,
				   const struct nfnl_info *info,
				   const struct nlattr * const nla[])
{
	struct nftables_pernet *nft_net = nft_pernet(info->net);
	u32 portid = NETLINK_CB(skb).portid;
	struct net *net = info->net;
	struct sk_buff *skb2;
	char *buf;

	if (info->nlh->nlmsg_flags & NLM_F_DUMP) {
		struct netlink_dump_control c = {
			.start= nf_tables_dumpreset_rules_start,
			.dump = nf_tables_dumpreset_rules,
			.done = nf_tables_dump_rules_done,
			.module = THIS_MODULE,
			.data = (void *)nla,
		};

		return nft_netlink_dump_start_rcu(info->sk, skb, info->nlh, &c);
	}

	if (!try_module_get(THIS_MODULE))
		return -EINVAL;
	rcu_read_unlock();
	mutex_lock(&nft_net->commit_mutex);
	skb2 = nf_tables_getrule_single(portid, info, nla, true);
	mutex_unlock(&nft_net->commit_mutex);
	rcu_read_lock();
	module_put(THIS_MODULE);

	if (IS_ERR(skb2))
		return PTR_ERR(skb2);

	buf = kasprintf(GFP_ATOMIC, "%.*s:%u",
			nla_len(nla[NFTA_RULE_TABLE]),
			(char *)nla_data(nla[NFTA_RULE_TABLE]),
			nft_net->base_seq);
	audit_log_nfcfg(buf, info->nfmsg->nfgen_family, 1,
			AUDIT_NFT_OP_RULE_RESET, GFP_ATOMIC);
	kfree(buf);

	return nfnetlink_unicast(skb2, net, portid);
}

void nf_tables_rule_destroy(const struct nft_ctx *ctx, struct nft_rule *rule)
{
	struct nft_expr *expr, *next;

	/*
	 * Careful: some expressions might not be initialized in case this
	 * is called on error from nf_tables_newrule().
	 */
	expr = nft_expr_first(rule);
	while (nft_expr_more(rule, expr)) {
		next = nft_expr_next(expr);
		nf_tables_expr_destroy(ctx, expr);
		expr = next;
	}
	kfree(rule);
}

/* can only be used if rule is no longer visible to dumps */
static void nf_tables_rule_release(const struct nft_ctx *ctx, struct nft_rule *rule)
{
	WARN_ON_ONCE(!lockdep_commit_lock_is_held(ctx->net));

	nft_rule_expr_deactivate(ctx, rule, NFT_TRANS_RELEASE);
	nf_tables_rule_destroy(ctx, rule);
}

/** nft_chain_validate - loop detection and hook validation
 *
 * @ctx: context containing call depth and base chain
 * @chain: chain to validate
 *
 * Walk through the rules of the given chain and chase all jumps/gotos
 * and set lookups until either the jump limit is hit or all reachable
 * chains have been validated.
 */
int nft_chain_validate(const struct nft_ctx *ctx, const struct nft_chain *chain)
{
	struct nft_expr *expr, *last;
	struct nft_rule *rule;
	int err;

	if (ctx->level == NFT_JUMP_STACK_SIZE)
		return -EMLINK;

	list_for_each_entry(rule, &chain->rules, list) {
		if (fatal_signal_pending(current))
			return -EINTR;

		if (!nft_is_active_next(ctx->net, rule))
			continue;

		nft_rule_for_each_expr(expr, last, rule) {
			if (!expr->ops->validate)
				continue;

			/* This may call nft_chain_validate() recursively,
			 * callers that do so must increment ctx->level.
			 */
			err = expr->ops->validate(ctx, expr);
			if (err < 0)
				return err;
		}
	}

	return 0;
}
EXPORT_SYMBOL_GPL(nft_chain_validate);

static int nft_table_validate(struct net *net, const struct nft_table *table)
{
	struct nft_chain *chain;
	struct nft_ctx ctx = {
		.net	= net,
		.family	= table->family,
	};
	int err;

	list_for_each_entry(chain, &table->chains, list) {
		if (!nft_is_base_chain(chain))
			continue;

		ctx.chain = chain;
		err = nft_chain_validate(&ctx, chain);
		if (err < 0)
			return err;

		cond_resched();
	}

	return 0;
}

int nft_setelem_validate(const struct nft_ctx *ctx, struct nft_set *set,
			 const struct nft_set_iter *iter,
			 struct nft_elem_priv *elem_priv)
{
	const struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);
	struct nft_ctx *pctx = (struct nft_ctx *)ctx;
	const struct nft_data *data;
	int err;

	if (!nft_set_elem_active(ext, iter->genmask))
		return 0;

	if (nft_set_ext_exists(ext, NFT_SET_EXT_FLAGS) &&
	    *nft_set_ext_flags(ext) & NFT_SET_ELEM_INTERVAL_END)
		return 0;

	data = nft_set_ext_data(ext);
	switch (data->verdict.code) {
	case NFT_JUMP:
	case NFT_GOTO:
		pctx->level++;
		err = nft_chain_validate(ctx, data->verdict.chain);
		if (err < 0)
			return err;
		pctx->level--;
		break;
	default:
		break;
	}

	return 0;
}

int nft_set_catchall_validate(const struct nft_ctx *ctx, struct nft_set *set)
{
	struct nft_set_iter dummy_iter = {
		.genmask	= nft_genmask_next(ctx->net),
	};
	struct nft_set_elem_catchall *catchall;

	struct nft_set_ext *ext;
	int ret = 0;

	list_for_each_entry_rcu(catchall, &set->catchall_list, list,
				lockdep_commit_lock_is_held(ctx->net)) {
		ext = nft_set_elem_ext(set, catchall->elem);
		if (!nft_set_elem_active(ext, dummy_iter.genmask))
			continue;

		ret = nft_setelem_validate(ctx, set, &dummy_iter, catchall->elem);
		if (ret < 0)
			return ret;
	}

	return ret;
}

static struct nft_rule *nft_rule_lookup_byid(const struct net *net,
					     const struct nft_chain *chain,
					     const struct nlattr *nla);

#define NFT_RULE_MAXEXPRS	128

static int nf_tables_newrule(struct sk_buff *skb, const struct nfnl_info *info,
			     const struct nlattr * const nla[])
{
	struct nftables_pernet *nft_net = nft_pernet(info->net);
	struct netlink_ext_ack *extack = info->extack;
	unsigned int size, i, n, ulen = 0, usize = 0;
	u8 genmask = nft_genmask_next(info->net);
	struct nft_rule *rule, *old_rule = NULL;
	struct nft_expr_info *expr_info = NULL;
	u8 family = info->nfmsg->nfgen_family;
	struct nft_flow_rule *flow = NULL;
	struct net *net = info->net;
	struct nft_userdata *udata;
	struct nft_table *table;
	struct nft_chain *chain;
	struct nft_trans *trans;
	u64 handle, pos_handle;
	struct nft_expr *expr;
	struct nft_ctx ctx;
	struct nlattr *tmp;
	int err, rem;

	lockdep_assert_held(&nft_net->commit_mutex);

	table = nft_table_lookup(net, nla[NFTA_RULE_TABLE], family, genmask,
				 NETLINK_CB(skb).portid);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_TABLE]);
		return PTR_ERR(table);
	}

	if (nla[NFTA_RULE_CHAIN]) {
		chain = nft_chain_lookup(net, table, nla[NFTA_RULE_CHAIN],
					 genmask);
		if (IS_ERR(chain)) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_CHAIN]);
			return PTR_ERR(chain);
		}

	} else if (nla[NFTA_RULE_CHAIN_ID]) {
		chain = nft_chain_lookup_byid(net, table, nla[NFTA_RULE_CHAIN_ID],
					      genmask);
		if (IS_ERR(chain)) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_CHAIN_ID]);
			return PTR_ERR(chain);
		}
	} else {
		return -EINVAL;
	}

	if (nft_chain_is_bound(chain))
		return -EOPNOTSUPP;

	if (nla[NFTA_RULE_HANDLE]) {
		handle = be64_to_cpu(nla_get_be64(nla[NFTA_RULE_HANDLE]));
		rule = __nft_rule_lookup(net, chain, handle);
		if (IS_ERR(rule)) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_HANDLE]);
			return PTR_ERR(rule);
		}

		if (info->nlh->nlmsg_flags & NLM_F_EXCL) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_HANDLE]);
			return -EEXIST;
		}
		if (info->nlh->nlmsg_flags & NLM_F_REPLACE)
			old_rule = rule;
		else
			return -EOPNOTSUPP;
	} else {
		if (!(info->nlh->nlmsg_flags & NLM_F_CREATE) ||
		    info->nlh->nlmsg_flags & NLM_F_REPLACE)
			return -EINVAL;
		handle = nf_tables_alloc_handle(table);

		if (nla[NFTA_RULE_POSITION]) {
			pos_handle = be64_to_cpu(nla_get_be64(nla[NFTA_RULE_POSITION]));
			old_rule = __nft_rule_lookup(net, chain, pos_handle);
			if (IS_ERR(old_rule)) {
				NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_POSITION]);
				return PTR_ERR(old_rule);
			}
		} else if (nla[NFTA_RULE_POSITION_ID]) {
			old_rule = nft_rule_lookup_byid(net, chain, nla[NFTA_RULE_POSITION_ID]);
			if (IS_ERR(old_rule)) {
				NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_POSITION_ID]);
				return PTR_ERR(old_rule);
			}
		}
	}

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, chain, nla);

	n = 0;
	size = 0;
	if (nla[NFTA_RULE_EXPRESSIONS]) {
		expr_info = kvmalloc_array(NFT_RULE_MAXEXPRS,
					   sizeof(struct nft_expr_info),
					   GFP_KERNEL);
		if (!expr_info)
			return -ENOMEM;

		nla_for_each_nested(tmp, nla[NFTA_RULE_EXPRESSIONS], rem) {
			err = -EINVAL;
			if (nla_type(tmp) != NFTA_LIST_ELEM)
				goto err_release_expr;
			if (n == NFT_RULE_MAXEXPRS)
				goto err_release_expr;
			err = nf_tables_expr_parse(&ctx, tmp, &expr_info[n]);
			if (err < 0) {
				NL_SET_BAD_ATTR(extack, tmp);
				goto err_release_expr;
			}
			size += expr_info[n].ops->size;
			n++;
		}
	}
	/* Check for overflow of dlen field */
	err = -EFBIG;
	if (size >= 1 << 12)
		goto err_release_expr;

	if (nla[NFTA_RULE_USERDATA]) {
		ulen = nla_len(nla[NFTA_RULE_USERDATA]);
		if (ulen > 0)
			usize = sizeof(struct nft_userdata) + ulen;
	}

	err = -ENOMEM;
	rule = kzalloc(sizeof(*rule) + size + usize, GFP_KERNEL_ACCOUNT);
	if (rule == NULL)
		goto err_release_expr;

	nft_activate_next(net, rule);

	rule->handle = handle;
	rule->dlen   = size;
	rule->udata  = ulen ? 1 : 0;

	if (ulen) {
		udata = nft_userdata(rule);
		udata->len = ulen - 1;
		nla_memcpy(udata->data, nla[NFTA_RULE_USERDATA], ulen);
	}

	expr = nft_expr_first(rule);
	for (i = 0; i < n; i++) {
		err = nf_tables_newexpr(&ctx, &expr_info[i], expr);
		if (err < 0) {
			NL_SET_BAD_ATTR(extack, expr_info[i].attr);
			goto err_release_rule;
		}

		if (expr_info[i].ops->validate)
			nft_validate_state_update(table, NFT_VALIDATE_NEED);

		expr_info[i].ops = NULL;
		expr = nft_expr_next(expr);
	}

	if (chain->flags & NFT_CHAIN_HW_OFFLOAD) {
		flow = nft_flow_rule_create(net, rule);
		if (IS_ERR(flow)) {
			err = PTR_ERR(flow);
			goto err_release_rule;
		}
	}

	if (!nft_use_inc(&chain->use)) {
		err = -EMFILE;
		goto err_release_rule;
	}

	if (info->nlh->nlmsg_flags & NLM_F_REPLACE) {
		if (nft_chain_binding(chain)) {
			err = -EOPNOTSUPP;
			goto err_destroy_flow_rule;
		}

		err = nft_delrule(&ctx, old_rule);
		if (err < 0)
			goto err_destroy_flow_rule;

		trans = nft_trans_rule_add(&ctx, NFT_MSG_NEWRULE, rule);
		if (trans == NULL) {
			err = -ENOMEM;
			goto err_destroy_flow_rule;
		}
		list_add_tail_rcu(&rule->list, &old_rule->list);
	} else {
		trans = nft_trans_rule_add(&ctx, NFT_MSG_NEWRULE, rule);
		if (!trans) {
			err = -ENOMEM;
			goto err_destroy_flow_rule;
		}

		if (info->nlh->nlmsg_flags & NLM_F_APPEND) {
			if (old_rule)
				list_add_rcu(&rule->list, &old_rule->list);
			else
				list_add_tail_rcu(&rule->list, &chain->rules);
		 } else {
			if (old_rule)
				list_add_tail_rcu(&rule->list, &old_rule->list);
			else
				list_add_rcu(&rule->list, &chain->rules);
		}
	}
	kvfree(expr_info);

	if (flow)
		nft_trans_flow_rule(trans) = flow;

	if (table->validate_state == NFT_VALIDATE_DO)
		return nft_table_validate(net, table);

	return 0;

err_destroy_flow_rule:
	nft_use_dec_restore(&chain->use);
	if (flow)
		nft_flow_rule_destroy(flow);
err_release_rule:
	nft_rule_expr_deactivate(&ctx, rule, NFT_TRANS_PREPARE_ERROR);
	nf_tables_rule_destroy(&ctx, rule);
err_release_expr:
	for (i = 0; i < n; i++) {
		if (expr_info[i].ops) {
			module_put(expr_info[i].ops->type->owner);
			if (expr_info[i].ops->type->release_ops)
				expr_info[i].ops->type->release_ops(expr_info[i].ops);
		}
	}
	kvfree(expr_info);

	return err;
}

static struct nft_rule *nft_rule_lookup_byid(const struct net *net,
					     const struct nft_chain *chain,
					     const struct nlattr *nla)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	u32 id = ntohl(nla_get_be32(nla));
	struct nft_trans *trans;

	list_for_each_entry(trans, &nft_net->commit_list, list) {
		if (trans->msg_type == NFT_MSG_NEWRULE &&
		    nft_trans_rule_chain(trans) == chain &&
		    id == nft_trans_rule_id(trans))
			return nft_trans_rule(trans);
	}
	return ERR_PTR(-ENOENT);
}

static int nf_tables_delrule(struct sk_buff *skb, const struct nfnl_info *info,
			     const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_next(info->net);
	u8 family = info->nfmsg->nfgen_family;
	struct nft_chain *chain = NULL;
	struct net *net = info->net;
	struct nft_table *table;
	struct nft_rule *rule;
	struct nft_ctx ctx;
	int err = 0;

	table = nft_table_lookup(net, nla[NFTA_RULE_TABLE], family, genmask,
				 NETLINK_CB(skb).portid);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_TABLE]);
		return PTR_ERR(table);
	}

	if (nla[NFTA_RULE_CHAIN]) {
		chain = nft_chain_lookup(net, table, nla[NFTA_RULE_CHAIN],
					 genmask);
		if (IS_ERR(chain)) {
			if (PTR_ERR(chain) == -ENOENT &&
			    NFNL_MSG_TYPE(info->nlh->nlmsg_type) == NFT_MSG_DESTROYRULE)
				return 0;

			NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_CHAIN]);
			return PTR_ERR(chain);
		}
		if (nft_chain_binding(chain))
			return -EOPNOTSUPP;
	}

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, chain, nla);

	if (chain) {
		if (nla[NFTA_RULE_HANDLE]) {
			rule = nft_rule_lookup(info->net, chain, nla[NFTA_RULE_HANDLE]);
			if (IS_ERR(rule)) {
				if (PTR_ERR(rule) == -ENOENT &&
				    NFNL_MSG_TYPE(info->nlh->nlmsg_type) == NFT_MSG_DESTROYRULE)
					return 0;

				NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_HANDLE]);
				return PTR_ERR(rule);
			}

			err = nft_delrule(&ctx, rule);
		} else if (nla[NFTA_RULE_ID]) {
			rule = nft_rule_lookup_byid(net, chain, nla[NFTA_RULE_ID]);
			if (IS_ERR(rule)) {
				NL_SET_BAD_ATTR(extack, nla[NFTA_RULE_ID]);
				return PTR_ERR(rule);
			}

			err = nft_delrule(&ctx, rule);
		} else {
			err = nft_delrule_by_chain(&ctx);
		}
	} else {
		list_for_each_entry(chain, &table->chains, list) {
			if (!nft_is_active_next(net, chain))
				continue;
			if (nft_chain_binding(chain))
				continue;

			ctx.chain = chain;
			err = nft_delrule_by_chain(&ctx);
			if (err < 0)
				break;
		}
	}

	return err;
}

/*
 * Sets
 */
static const struct nft_set_type *nft_set_types[] = {
	&nft_set_hash_fast_type,
	&nft_set_hash_type,
	&nft_set_rhash_type,
	&nft_set_bitmap_type,
	&nft_set_rbtree_type,
#if defined(CONFIG_X86_64) && !defined(CONFIG_UML)
	&nft_set_pipapo_avx2_type,
#endif
	&nft_set_pipapo_type,
};

#define NFT_SET_FEATURES	(NFT_SET_INTERVAL | NFT_SET_MAP | \
				 NFT_SET_TIMEOUT | NFT_SET_OBJECT | \
				 NFT_SET_EVAL)

static bool nft_set_ops_candidate(const struct nft_set_type *type, u32 flags)
{
	return (flags & type->features) == (flags & NFT_SET_FEATURES);
}

/*
 * Select a set implementation based on the data characteristics and the
 * given policy. The total memory use might not be known if no size is
 * given, in that case the amount of memory per element is used.
 */
static const struct nft_set_ops *
nft_select_set_ops(const struct nft_ctx *ctx, u32 flags,
		   const struct nft_set_desc *desc)
{
	struct nftables_pernet *nft_net = nft_pernet(ctx->net);
	const struct nft_set_ops *ops, *bops;
	struct nft_set_estimate est, best;
	const struct nft_set_type *type;
	int i;

	lockdep_assert_held(&nft_net->commit_mutex);
	lockdep_nfnl_nft_mutex_not_held();

	bops	    = NULL;
	best.size   = ~0;
	best.lookup = ~0;
	best.space  = ~0;

	for (i = 0; i < ARRAY_SIZE(nft_set_types); i++) {
		type = nft_set_types[i];
		ops = &type->ops;

		if (!nft_set_ops_candidate(type, flags))
			continue;
		if (!ops->estimate(desc, flags, &est))
			continue;

		switch (desc->policy) {
		case NFT_SET_POL_PERFORMANCE:
			if (est.lookup < best.lookup)
				break;
			if (est.lookup == best.lookup &&
			    est.space < best.space)
				break;
			continue;
		case NFT_SET_POL_MEMORY:
			if (!desc->size) {
				if (est.space < best.space)
					break;
				if (est.space == best.space &&
				    est.lookup < best.lookup)
					break;
			} else if (est.size < best.size || !bops) {
				break;
			}
			continue;
		default:
			break;
		}

		bops = ops;
		best = est;
	}

	if (bops != NULL)
		return bops;

	return ERR_PTR(-EOPNOTSUPP);
}

static const struct nla_policy nft_set_policy[NFTA_SET_MAX + 1] = {
	[NFTA_SET_TABLE]		= { .type = NLA_STRING,
					    .len = NFT_TABLE_MAXNAMELEN - 1 },
	[NFTA_SET_NAME]			= { .type = NLA_STRING,
					    .len = NFT_SET_MAXNAMELEN - 1 },
	[NFTA_SET_FLAGS]		= { .type = NLA_U32 },
	[NFTA_SET_KEY_TYPE]		= { .type = NLA_U32 },
	[NFTA_SET_KEY_LEN]		= { .type = NLA_U32 },
	[NFTA_SET_DATA_TYPE]		= { .type = NLA_U32 },
	[NFTA_SET_DATA_LEN]		= { .type = NLA_U32 },
	[NFTA_SET_POLICY]		= { .type = NLA_U32 },
	[NFTA_SET_DESC]			= { .type = NLA_NESTED },
	[NFTA_SET_ID]			= { .type = NLA_U32 },
	[NFTA_SET_TIMEOUT]		= { .type = NLA_U64 },
	[NFTA_SET_GC_INTERVAL]		= { .type = NLA_U32 },
	[NFTA_SET_USERDATA]		= { .type = NLA_BINARY,
					    .len  = NFT_USERDATA_MAXLEN },
	[NFTA_SET_OBJ_TYPE]		= { .type = NLA_U32 },
	[NFTA_SET_HANDLE]		= { .type = NLA_U64 },
	[NFTA_SET_EXPR]			= { .type = NLA_NESTED },
	[NFTA_SET_EXPRESSIONS]		= NLA_POLICY_NESTED_ARRAY(nft_expr_policy),
	[NFTA_SET_TYPE]			= { .type = NLA_REJECT },
	[NFTA_SET_COUNT]		= { .type = NLA_REJECT },
};

static const struct nla_policy nft_concat_policy[NFTA_SET_FIELD_MAX + 1] = {
	[NFTA_SET_FIELD_LEN]	= { .type = NLA_U32 },
};

static const struct nla_policy nft_set_desc_policy[NFTA_SET_DESC_MAX + 1] = {
	[NFTA_SET_DESC_SIZE]		= { .type = NLA_U32 },
	[NFTA_SET_DESC_CONCAT]		= NLA_POLICY_NESTED_ARRAY(nft_concat_policy),
};

static struct nft_set *nft_set_lookup(const struct net *net,
				      const struct nft_table *table,
				      const struct nlattr *nla, u8 genmask)
{
	struct nft_set *set;

	if (nla == NULL)
		return ERR_PTR(-EINVAL);

	list_for_each_entry_rcu(set, &table->sets, list,
				lockdep_commit_lock_is_held(net)) {
		if (!nla_strcmp(nla, set->name) &&
		    nft_active_genmask(set, genmask))
			return set;
	}
	return ERR_PTR(-ENOENT);
}

static struct nft_set *nft_set_lookup_byhandle(const struct nft_table *table,
					       const struct nlattr *nla,
					       u8 genmask)
{
	struct nft_set *set;

	list_for_each_entry(set, &table->sets, list) {
		if (be64_to_cpu(nla_get_be64(nla)) == set->handle &&
		    nft_active_genmask(set, genmask))
			return set;
	}
	return ERR_PTR(-ENOENT);
}

static struct nft_set *nft_set_lookup_byid(const struct net *net,
					   const struct nft_table *table,
					   const struct nlattr *nla, u8 genmask)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	u32 id = ntohl(nla_get_be32(nla));
	struct nft_trans_set *trans;

	/* its likely the id we need is at the tail, not at start */
	list_for_each_entry_reverse(trans, &nft_net->commit_set_list, list_trans_newset) {
		struct nft_set *set = trans->set;

		if (id == trans->set_id &&
		    set->table == table &&
		    nft_active_genmask(set, genmask))
			return set;
	}
	return ERR_PTR(-ENOENT);
}

struct nft_set *nft_set_lookup_global(const struct net *net,
				      const struct nft_table *table,
				      const struct nlattr *nla_set_name,
				      const struct nlattr *nla_set_id,
				      u8 genmask)
{
	struct nft_set *set;

	set = nft_set_lookup(net, table, nla_set_name, genmask);
	if (IS_ERR(set)) {
		if (!nla_set_id)
			return set;

		set = nft_set_lookup_byid(net, table, nla_set_id, genmask);
	}
	return set;
}
EXPORT_SYMBOL_GPL(nft_set_lookup_global);

static int nf_tables_set_alloc_name(struct nft_ctx *ctx, struct nft_set *set,
				    const char *name)
{
	const struct nft_set *i;
	const char *p;
	unsigned long *inuse;
	unsigned int n = 0, min = 0;

	p = strchr(name, '%');
	if (p != NULL) {
		if (p[1] != 'd' || strchr(p + 2, '%'))
			return -EINVAL;

		if (strnlen(name, NFT_SET_MAX_ANONLEN) >= NFT_SET_MAX_ANONLEN)
			return -EINVAL;

		inuse = (unsigned long *)get_zeroed_page(GFP_KERNEL);
		if (inuse == NULL)
			return -ENOMEM;
cont:
		list_for_each_entry(i, &ctx->table->sets, list) {
			int tmp;

			if (!nft_is_active_next(ctx->net, i))
				continue;
			if (!sscanf(i->name, name, &tmp))
				continue;
			if (tmp < min || tmp >= min + BITS_PER_BYTE * PAGE_SIZE)
				continue;

			set_bit(tmp - min, inuse);
		}

		n = find_first_zero_bit(inuse, BITS_PER_BYTE * PAGE_SIZE);
		if (n >= BITS_PER_BYTE * PAGE_SIZE) {
			min += BITS_PER_BYTE * PAGE_SIZE;
			memset(inuse, 0, PAGE_SIZE);
			goto cont;
		}
		free_page((unsigned long)inuse);
	}

	set->name = kasprintf(GFP_KERNEL_ACCOUNT, name, min + n);
	if (!set->name)
		return -ENOMEM;

	list_for_each_entry(i, &ctx->table->sets, list) {
		if (!nft_is_active_next(ctx->net, i))
			continue;
		if (!strcmp(set->name, i->name)) {
			kfree(set->name);
			set->name = NULL;
			return -ENFILE;
		}
	}
	return 0;
}

int nf_msecs_to_jiffies64(const struct nlattr *nla, u64 *result)
{
	u64 ms = be64_to_cpu(nla_get_be64(nla));
	u64 max = (u64)(~((u64)0));

	max = div_u64(max, NSEC_PER_MSEC);
	if (ms >= max)
		return -ERANGE;

	ms *= NSEC_PER_MSEC;
	*result = nsecs_to_jiffies64(ms) ? : !!ms;
	return 0;
}

__be64 nf_jiffies64_to_msecs(u64 input)
{
	return cpu_to_be64(jiffies64_to_msecs(input));
}

static int nf_tables_fill_set_concat(struct sk_buff *skb,
				     const struct nft_set *set)
{
	struct nlattr *concat, *field;
	int i;

	concat = nla_nest_start_noflag(skb, NFTA_SET_DESC_CONCAT);
	if (!concat)
		return -ENOMEM;

	for (i = 0; i < set->field_count; i++) {
		field = nla_nest_start_noflag(skb, NFTA_LIST_ELEM);
		if (!field)
			return -ENOMEM;

		if (nla_put_be32(skb, NFTA_SET_FIELD_LEN,
				 htonl(set->field_len[i])))
			return -ENOMEM;

		nla_nest_end(skb, field);
	}

	nla_nest_end(skb, concat);

	return 0;
}

static u32 nft_set_userspace_size(const struct nft_set_ops *ops, u32 size)
{
	if (ops->usize)
		return ops->usize(size);

	return size;
}

static noinline_for_stack int
nf_tables_fill_set_info(struct sk_buff *skb, const struct nft_set *set)
{
	unsigned int nelems;
	char str[40];
	int ret;

	ret = snprintf(str, sizeof(str), "%ps", set->ops);

	/* Not expected to happen and harmless: NFTA_SET_TYPE is dumped
	 * to userspace purely for informational/debug purposes.
	 */
	DEBUG_NET_WARN_ON_ONCE(ret >= sizeof(str));

	if (nla_put_string(skb, NFTA_SET_TYPE, str))
		return -EMSGSIZE;

	nelems = nft_set_userspace_size(set->ops, atomic_read(&set->nelems));
	return nla_put_be32(skb, NFTA_SET_COUNT, htonl(nelems));
}

static int nf_tables_fill_set(struct sk_buff *skb, const struct nft_ctx *ctx,
			      const struct nft_set *set, u16 event, u16 flags)
{
	u64 timeout = READ_ONCE(set->timeout);
	u32 gc_int = READ_ONCE(set->gc_int);
	u32 portid = ctx->portid;
	struct nlmsghdr *nlh;
	struct nlattr *nest;
	u32 seq = ctx->seq;
	int i;

	nlh = nfnl_msg_put(skb, portid, seq,
			   nfnl_msg_type(NFNL_SUBSYS_NFTABLES, event),
			   flags, ctx->family, NFNETLINK_V0,
			   nft_base_seq(ctx->net));
	if (!nlh)
		goto nla_put_failure;

	if (nla_put_string(skb, NFTA_SET_TABLE, ctx->table->name))
		goto nla_put_failure;
	if (nla_put_string(skb, NFTA_SET_NAME, set->name))
		goto nla_put_failure;
	if (nla_put_be64(skb, NFTA_SET_HANDLE, cpu_to_be64(set->handle),
			 NFTA_SET_PAD))
		goto nla_put_failure;

	if (event == NFT_MSG_DELSET ||
	    event == NFT_MSG_DESTROYSET) {
		nlmsg_end(skb, nlh);
		return 0;
	}

	if (set->flags != 0)
		if (nla_put_be32(skb, NFTA_SET_FLAGS, htonl(set->flags)))
			goto nla_put_failure;

	if (nla_put_be32(skb, NFTA_SET_KEY_TYPE, htonl(set->ktype)))
		goto nla_put_failure;
	if (nla_put_be32(skb, NFTA_SET_KEY_LEN, htonl(set->klen)))
		goto nla_put_failure;
	if (set->flags & NFT_SET_MAP) {
		if (nla_put_be32(skb, NFTA_SET_DATA_TYPE, htonl(set->dtype)))
			goto nla_put_failure;
		if (nla_put_be32(skb, NFTA_SET_DATA_LEN, htonl(set->dlen)))
			goto nla_put_failure;
	}
	if (set->flags & NFT_SET_OBJECT &&
	    nla_put_be32(skb, NFTA_SET_OBJ_TYPE, htonl(set->objtype)))
		goto nla_put_failure;

	if (timeout &&
	    nla_put_be64(skb, NFTA_SET_TIMEOUT,
			 nf_jiffies64_to_msecs(timeout),
			 NFTA_SET_PAD))
		goto nla_put_failure;
	if (gc_int &&
	    nla_put_be32(skb, NFTA_SET_GC_INTERVAL, htonl(gc_int)))
		goto nla_put_failure;

	if (set->policy != NFT_SET_POL_PERFORMANCE) {
		if (nla_put_be32(skb, NFTA_SET_POLICY, htonl(set->policy)))
			goto nla_put_failure;
	}

	if (set->udata &&
	    nla_put(skb, NFTA_SET_USERDATA, set->udlen, set->udata))
		goto nla_put_failure;

	nest = nla_nest_start_noflag(skb, NFTA_SET_DESC);
	if (!nest)
		goto nla_put_failure;
	if (set->size &&
	    nla_put_be32(skb, NFTA_SET_DESC_SIZE,
			 htonl(nft_set_userspace_size(set->ops, set->size))))
		goto nla_put_failure;

	if (set->field_count > 1 &&
	    nf_tables_fill_set_concat(skb, set))
		goto nla_put_failure;

	nla_nest_end(skb, nest);

	if (nf_tables_fill_set_info(skb, set))
		goto nla_put_failure;

	if (set->num_exprs == 1) {
		nest = nla_nest_start_noflag(skb, NFTA_SET_EXPR);
		if (nf_tables_fill_expr_info(skb, set->exprs[0], false) < 0)
			goto nla_put_failure;

		nla_nest_end(skb, nest);
	} else if (set->num_exprs > 1) {
		nest = nla_nest_start_noflag(skb, NFTA_SET_EXPRESSIONS);
		if (nest == NULL)
			goto nla_put_failure;

		for (i = 0; i < set->num_exprs; i++) {
			if (nft_expr_dump(skb, NFTA_LIST_ELEM,
					  set->exprs[i], false) < 0)
				goto nla_put_failure;
		}
		nla_nest_end(skb, nest);
	}

	nlmsg_end(skb, nlh);
	return 0;

nla_put_failure:
	nlmsg_trim(skb, nlh);
	return -1;
}

static void nf_tables_set_notify(const struct nft_ctx *ctx,
				 const struct nft_set *set, int event,
			         gfp_t gfp_flags)
{
	struct nftables_pernet *nft_net = nft_pernet(ctx->net);
	u32 portid = ctx->portid;
	struct sk_buff *skb;
	u16 flags = 0;
	int err;

	if (!ctx->report &&
	    !nfnetlink_has_listeners(ctx->net, NFNLGRP_NFTABLES))
		return;

	skb = nlmsg_new(NLMSG_GOODSIZE, gfp_flags);
	if (skb == NULL)
		goto err;

	if (ctx->flags & (NLM_F_CREATE | NLM_F_EXCL))
		flags |= ctx->flags & (NLM_F_CREATE | NLM_F_EXCL);

	err = nf_tables_fill_set(skb, ctx, set, event, flags);
	if (err < 0) {
		kfree_skb(skb);
		goto err;
	}

	nft_notify_enqueue(skb, ctx->report, &nft_net->notify_list);
	return;
err:
	nfnetlink_set_err(ctx->net, portid, NFNLGRP_NFTABLES, -ENOBUFS);
}

static int nf_tables_dump_sets(struct sk_buff *skb, struct netlink_callback *cb)
{
	const struct nft_set *set;
	unsigned int idx, s_idx = cb->args[0];
	struct nft_table *table, *cur_table = (struct nft_table *)cb->args[2];
	struct net *net = sock_net(skb->sk);
	struct nft_ctx *ctx = cb->data, ctx_set;
	struct nftables_pernet *nft_net;

	if (cb->args[1])
		return skb->len;

	rcu_read_lock();
	nft_net = nft_pernet(net);
	cb->seq = READ_ONCE(nft_net->base_seq);

	list_for_each_entry_rcu(table, &nft_net->tables, list) {
		if (ctx->family != NFPROTO_UNSPEC &&
		    ctx->family != table->family)
			continue;

		if (ctx->table && ctx->table != table)
			continue;

		if (cur_table) {
			if (cur_table != table)
				continue;

			cur_table = NULL;
		}
		idx = 0;
		list_for_each_entry_rcu(set, &table->sets, list) {
			if (idx < s_idx)
				goto cont;
			if (!nft_is_active(net, set))
				goto cont;

			ctx_set = *ctx;
			ctx_set.table = table;
			ctx_set.family = table->family;

			if (nf_tables_fill_set(skb, &ctx_set, set,
					       NFT_MSG_NEWSET,
					       NLM_F_MULTI) < 0) {
				cb->args[0] = idx;
				cb->args[2] = (unsigned long) table;
				goto done;
			}
			nl_dump_check_consistent(cb, nlmsg_hdr(skb));
cont:
			idx++;
		}
		if (s_idx)
			s_idx = 0;
	}
	cb->args[1] = 1;
done:
	rcu_read_unlock();
	return skb->len;
}

static int nf_tables_dump_sets_start(struct netlink_callback *cb)
{
	struct nft_ctx *ctx_dump = NULL;

	ctx_dump = kmemdup(cb->data, sizeof(*ctx_dump), GFP_ATOMIC);
	if (ctx_dump == NULL)
		return -ENOMEM;

	cb->data = ctx_dump;
	return 0;
}

static int nf_tables_dump_sets_done(struct netlink_callback *cb)
{
	kfree(cb->data);
	return 0;
}

/* called with rcu_read_lock held */
static int nf_tables_getset(struct sk_buff *skb, const struct nfnl_info *info,
			    const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_cur(info->net);
	u8 family = info->nfmsg->nfgen_family;
	struct nft_table *table = NULL;
	struct net *net = info->net;
	const struct nft_set *set;
	struct sk_buff *skb2;
	struct nft_ctx ctx;
	int err;

	if (nla[NFTA_SET_TABLE]) {
		table = nft_table_lookup(net, nla[NFTA_SET_TABLE], family,
					 genmask, 0);
		if (IS_ERR(table)) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_SET_TABLE]);
			return PTR_ERR(table);
		}
	}

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, NULL, nla);

	if (info->nlh->nlmsg_flags & NLM_F_DUMP) {
		struct netlink_dump_control c = {
			.start = nf_tables_dump_sets_start,
			.dump = nf_tables_dump_sets,
			.done = nf_tables_dump_sets_done,
			.data = &ctx,
			.module = THIS_MODULE,
		};

		return nft_netlink_dump_start_rcu(info->sk, skb, info->nlh, &c);
	}

	/* Only accept unspec with dump */
	if (info->nfmsg->nfgen_family == NFPROTO_UNSPEC)
		return -EAFNOSUPPORT;
	if (!nla[NFTA_SET_TABLE])
		return -EINVAL;

	set = nft_set_lookup(net, table, nla[NFTA_SET_NAME], genmask);
	if (IS_ERR(set)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_SET_NAME]);
		return PTR_ERR(set);
	}

	skb2 = alloc_skb(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (skb2 == NULL)
		return -ENOMEM;

	err = nf_tables_fill_set(skb2, &ctx, set, NFT_MSG_NEWSET, 0);
	if (err < 0)
		goto err_fill_set_info;

	return nfnetlink_unicast(skb2, net, NETLINK_CB(skb).portid);

err_fill_set_info:
	kfree_skb(skb2);
	return err;
}

static int nft_set_desc_concat_parse(const struct nlattr *attr,
				     struct nft_set_desc *desc)
{
	struct nlattr *tb[NFTA_SET_FIELD_MAX + 1];
	u32 len;
	int err;

	if (desc->field_count >= ARRAY_SIZE(desc->field_len))
		return -E2BIG;

	err = nla_parse_nested_deprecated(tb, NFTA_SET_FIELD_MAX, attr,
					  nft_concat_policy, NULL);
	if (err < 0)
		return err;

	if (!tb[NFTA_SET_FIELD_LEN])
		return -EINVAL;

	len = ntohl(nla_get_be32(tb[NFTA_SET_FIELD_LEN]));
	if (!len || len > U8_MAX)
		return -EINVAL;

	desc->field_len[desc->field_count++] = len;

	return 0;
}

static int nft_set_desc_concat(struct nft_set_desc *desc,
			       const struct nlattr *nla)
{
	u32 len = 0, num_regs;
	struct nlattr *attr;
	int rem, err, i;

	nla_for_each_nested(attr, nla, rem) {
		if (nla_type(attr) != NFTA_LIST_ELEM)
			return -EINVAL;

		err = nft_set_desc_concat_parse(attr, desc);
		if (err < 0)
			return err;
	}

	for (i = 0; i < desc->field_count; i++)
		len += round_up(desc->field_len[i], sizeof(u32));

	if (len != desc->klen)
		return -EINVAL;

	num_regs = DIV_ROUND_UP(desc->klen, sizeof(u32));
	if (num_regs > NFT_REG32_COUNT)
		return -E2BIG;

	return 0;
}

static int nf_tables_set_desc_parse(struct nft_set_desc *desc,
				    const struct nlattr *nla)
{
	struct nlattr *da[NFTA_SET_DESC_MAX + 1];
	int err;

	err = nla_parse_nested_deprecated(da, NFTA_SET_DESC_MAX, nla,
					  nft_set_desc_policy, NULL);
	if (err < 0)
		return err;

	if (da[NFTA_SET_DESC_SIZE] != NULL)
		desc->size = ntohl(nla_get_be32(da[NFTA_SET_DESC_SIZE]));
	if (da[NFTA_SET_DESC_CONCAT])
		err = nft_set_desc_concat(desc, da[NFTA_SET_DESC_CONCAT]);

	return err;
}

static int nft_set_expr_alloc(struct nft_ctx *ctx, struct nft_set *set,
			      const struct nlattr * const *nla,
			      struct nft_expr **exprs, int *num_exprs,
			      u32 flags)
{
	struct nft_expr *expr;
	int err, i;

	if (nla[NFTA_SET_EXPR]) {
		expr = nft_set_elem_expr_alloc(ctx, set, nla[NFTA_SET_EXPR]);
		if (IS_ERR(expr)) {
			err = PTR_ERR(expr);
			goto err_set_expr_alloc;
		}
		exprs[0] = expr;
		(*num_exprs)++;
	} else if (nla[NFTA_SET_EXPRESSIONS]) {
		struct nlattr *tmp;
		int left;

		if (!(flags & NFT_SET_EXPR)) {
			err = -EINVAL;
			goto err_set_expr_alloc;
		}
		i = 0;
		nla_for_each_nested(tmp, nla[NFTA_SET_EXPRESSIONS], left) {
			if (i == NFT_SET_EXPR_MAX) {
				err = -E2BIG;
				goto err_set_expr_alloc;
			}
			if (nla_type(tmp) != NFTA_LIST_ELEM) {
				err = -EINVAL;
				goto err_set_expr_alloc;
			}
			expr = nft_set_elem_expr_alloc(ctx, set, tmp);
			if (IS_ERR(expr)) {
				err = PTR_ERR(expr);
				goto err_set_expr_alloc;
			}
			exprs[i++] = expr;
			(*num_exprs)++;
		}
	}

	return 0;

err_set_expr_alloc:
	for (i = 0; i < *num_exprs; i++)
		nft_expr_destroy(ctx, exprs[i]);

	return err;
}

static bool nft_set_is_same(const struct nft_set *set,
			    const struct nft_set_desc *desc,
			    struct nft_expr *exprs[], u32 num_exprs, u32 flags)
{
	int i;

	if (set->ktype != desc->ktype ||
	    set->dtype != desc->dtype ||
	    set->flags != flags ||
	    set->klen != desc->klen ||
	    set->dlen != desc->dlen ||
	    set->field_count != desc->field_count ||
	    set->num_exprs != num_exprs)
		return false;

	for (i = 0; i < desc->field_count; i++) {
		if (set->field_len[i] != desc->field_len[i])
			return false;
	}

	for (i = 0; i < num_exprs; i++) {
		if (set->exprs[i]->ops != exprs[i]->ops)
			return false;
	}

	return true;
}

static u32 nft_set_kernel_size(const struct nft_set_ops *ops,
			       const struct nft_set_desc *desc)
{
	if (ops->ksize)
		return ops->ksize(desc->size);

	return desc->size;
}

static int nf_tables_newset(struct sk_buff *skb, const struct nfnl_info *info,
			    const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_next(info->net);
	u8 family = info->nfmsg->nfgen_family;
	const struct nft_set_ops *ops;
	struct net *net = info->net;
	struct nft_set_desc desc;
	struct nft_table *table;
	unsigned char *udata;
	struct nft_set *set;
	struct nft_ctx ctx;
	size_t alloc_size;
	int num_exprs = 0;
	char *name;
	int err, i;
	u16 udlen;
	u32 flags;
	u64 size;

	if (nla[NFTA_SET_TABLE] == NULL ||
	    nla[NFTA_SET_NAME] == NULL ||
	    nla[NFTA_SET_KEY_LEN] == NULL ||
	    nla[NFTA_SET_ID] == NULL)
		return -EINVAL;

	memset(&desc, 0, sizeof(desc));

	desc.ktype = NFT_DATA_VALUE;
	if (nla[NFTA_SET_KEY_TYPE] != NULL) {
		desc.ktype = ntohl(nla_get_be32(nla[NFTA_SET_KEY_TYPE]));
		if ((desc.ktype & NFT_DATA_RESERVED_MASK) == NFT_DATA_RESERVED_MASK)
			return -EINVAL;
	}

	desc.klen = ntohl(nla_get_be32(nla[NFTA_SET_KEY_LEN]));
	if (desc.klen == 0 || desc.klen > NFT_DATA_VALUE_MAXLEN)
		return -EINVAL;

	flags = 0;
	if (nla[NFTA_SET_FLAGS] != NULL) {
		flags = ntohl(nla_get_be32(nla[NFTA_SET_FLAGS]));
		if (flags & ~(NFT_SET_ANONYMOUS | NFT_SET_CONSTANT |
			      NFT_SET_INTERVAL | NFT_SET_TIMEOUT |
			      NFT_SET_MAP | NFT_SET_EVAL |
			      NFT_SET_OBJECT | NFT_SET_CONCAT | NFT_SET_EXPR))
			return -EOPNOTSUPP;
		/* Only one of these operations is supported */
		if ((flags & (NFT_SET_MAP | NFT_SET_OBJECT)) ==
			     (NFT_SET_MAP | NFT_SET_OBJECT))
			return -EOPNOTSUPP;
		if ((flags & (NFT_SET_EVAL | NFT_SET_OBJECT)) ==
			     (NFT_SET_EVAL | NFT_SET_OBJECT))
			return -EOPNOTSUPP;
		if ((flags & (NFT_SET_ANONYMOUS | NFT_SET_TIMEOUT | NFT_SET_EVAL)) ==
			     (NFT_SET_ANONYMOUS | NFT_SET_TIMEOUT))
			return -EOPNOTSUPP;
		if ((flags & (NFT_SET_CONSTANT | NFT_SET_TIMEOUT)) ==
			     (NFT_SET_CONSTANT | NFT_SET_TIMEOUT))
			return -EOPNOTSUPP;
	}

	desc.dtype = 0;
	if (nla[NFTA_SET_DATA_TYPE] != NULL) {
		if (!(flags & NFT_SET_MAP))
			return -EINVAL;

		desc.dtype = ntohl(nla_get_be32(nla[NFTA_SET_DATA_TYPE]));
		if ((desc.dtype & NFT_DATA_RESERVED_MASK) == NFT_DATA_RESERVED_MASK &&
		    desc.dtype != NFT_DATA_VERDICT)
			return -EINVAL;

		if (desc.dtype != NFT_DATA_VERDICT) {
			if (nla[NFTA_SET_DATA_LEN] == NULL)
				return -EINVAL;
			desc.dlen = ntohl(nla_get_be32(nla[NFTA_SET_DATA_LEN]));
			if (desc.dlen == 0 || desc.dlen > NFT_DATA_VALUE_MAXLEN)
				return -EINVAL;
		} else
			desc.dlen = sizeof(struct nft_verdict);
	} else if (flags & NFT_SET_MAP)
		return -EINVAL;

	if (nla[NFTA_SET_OBJ_TYPE] != NULL) {
		if (!(flags & NFT_SET_OBJECT))
			return -EINVAL;

		desc.objtype = ntohl(nla_get_be32(nla[NFTA_SET_OBJ_TYPE]));
		if (desc.objtype == NFT_OBJECT_UNSPEC ||
		    desc.objtype > NFT_OBJECT_MAX)
			return -EOPNOTSUPP;
	} else if (flags & NFT_SET_OBJECT)
		return -EINVAL;
	else
		desc.objtype = NFT_OBJECT_UNSPEC;

	desc.timeout = 0;
	if (nla[NFTA_SET_TIMEOUT] != NULL) {
		if (!(flags & NFT_SET_TIMEOUT))
			return -EINVAL;

		if (flags & NFT_SET_ANONYMOUS)
			return -EOPNOTSUPP;

		err = nf_msecs_to_jiffies64(nla[NFTA_SET_TIMEOUT], &desc.timeout);
		if (err)
			return err;
	}
	desc.gc_int = 0;
	if (nla[NFTA_SET_GC_INTERVAL] != NULL) {
		if (!(flags & NFT_SET_TIMEOUT))
			return -EINVAL;

		if (flags & NFT_SET_ANONYMOUS)
			return -EOPNOTSUPP;

		desc.gc_int = ntohl(nla_get_be32(nla[NFTA_SET_GC_INTERVAL]));
	}

	desc.policy = NFT_SET_POL_PERFORMANCE;
	if (nla[NFTA_SET_POLICY] != NULL) {
		desc.policy = ntohl(nla_get_be32(nla[NFTA_SET_POLICY]));
		switch (desc.policy) {
		case NFT_SET_POL_PERFORMANCE:
		case NFT_SET_POL_MEMORY:
			break;
		default:
			return -EOPNOTSUPP;
		}
	}

	if (nla[NFTA_SET_DESC] != NULL) {
		err = nf_tables_set_desc_parse(&desc, nla[NFTA_SET_DESC]);
		if (err < 0)
			return err;

		if (desc.field_count > 1) {
			if (!(flags & NFT_SET_CONCAT))
				return -EINVAL;
		} else if (flags & NFT_SET_CONCAT) {
			return -EINVAL;
		}
	} else if (flags & NFT_SET_CONCAT) {
		return -EINVAL;
	}

	if (nla[NFTA_SET_EXPR] || nla[NFTA_SET_EXPRESSIONS])
		desc.expr = true;

	table = nft_table_lookup(net, nla[NFTA_SET_TABLE], family, genmask,
				 NETLINK_CB(skb).portid);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_SET_TABLE]);
		return PTR_ERR(table);
	}

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, NULL, nla);

	set = nft_set_lookup(net, table, nla[NFTA_SET_NAME], genmask);
	if (IS_ERR(set)) {
		if (PTR_ERR(set) != -ENOENT) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_SET_NAME]);
			return PTR_ERR(set);
		}
	} else {
		struct nft_expr *exprs[NFT_SET_EXPR_MAX] = {};

		if (info->nlh->nlmsg_flags & NLM_F_EXCL) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_SET_NAME]);
			return -EEXIST;
		}
		if (info->nlh->nlmsg_flags & NLM_F_REPLACE)
			return -EOPNOTSUPP;

		if (nft_set_is_anonymous(set))
			return -EOPNOTSUPP;

		err = nft_set_expr_alloc(&ctx, set, nla, exprs, &num_exprs, flags);
		if (err < 0)
			return err;

		if (desc.size)
			desc.size = nft_set_kernel_size(set->ops, &desc);

		err = 0;
		if (!nft_set_is_same(set, &desc, exprs, num_exprs, flags)) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_SET_NAME]);
			err = -EEXIST;
		}

		for (i = 0; i < num_exprs; i++)
			nft_expr_destroy(&ctx, exprs[i]);

		if (err < 0)
			return err;

		return __nft_trans_set_add(&ctx, NFT_MSG_NEWSET, set, &desc);
	}

	if (!(info->nlh->nlmsg_flags & NLM_F_CREATE))
		return -ENOENT;

	ops = nft_select_set_ops(&ctx, flags, &desc);
	if (IS_ERR(ops))
		return PTR_ERR(ops);

	if (desc.size)
		desc.size = nft_set_kernel_size(ops, &desc);

	udlen = 0;
	if (nla[NFTA_SET_USERDATA])
		udlen = nla_len(nla[NFTA_SET_USERDATA]);

	size = 0;
	if (ops->privsize != NULL)
		size = ops->privsize(nla, &desc);
	alloc_size = sizeof(*set) + size + udlen;
	if (alloc_size < size || alloc_size > INT_MAX)
		return -ENOMEM;

	if (!nft_use_inc(&table->use))
		return -EMFILE;

	set = kvzalloc(alloc_size, GFP_KERNEL_ACCOUNT);
	if (!set) {
		err = -ENOMEM;
		goto err_alloc;
	}

	name = nla_strdup(nla[NFTA_SET_NAME], GFP_KERNEL_ACCOUNT);
	if (!name) {
		err = -ENOMEM;
		goto err_set_name;
	}

	err = nf_tables_set_alloc_name(&ctx, set, name);
	kfree(name);
	if (err < 0)
		goto err_set_name;

	udata = NULL;
	if (udlen) {
		udata = set->data + size;
		nla_memcpy(udata, nla[NFTA_SET_USERDATA], udlen);
	}

	INIT_LIST_HEAD(&set->bindings);
	INIT_LIST_HEAD(&set->catchall_list);
	refcount_set(&set->refs, 1);
	set->table = table;
	write_pnet(&set->net, net);
	set->ops = ops;
	set->ktype = desc.ktype;
	set->klen = desc.klen;
	set->dtype = desc.dtype;
	set->objtype = desc.objtype;
	set->dlen = desc.dlen;
	set->flags = flags;
	set->size = desc.size;
	set->policy = desc.policy;
	set->udlen = udlen;
	set->udata = udata;
	set->timeout = desc.timeout;
	set->gc_int = desc.gc_int;

	set->field_count = desc.field_count;
	for (i = 0; i < desc.field_count; i++)
		set->field_len[i] = desc.field_len[i];

	err = ops->init(set, &desc, nla);
	if (err < 0)
		goto err_set_init;

	err = nft_set_expr_alloc(&ctx, set, nla, set->exprs, &num_exprs, flags);
	if (err < 0)
		goto err_set_destroy;

	set->num_exprs = num_exprs;
	set->handle = nf_tables_alloc_handle(table);
	INIT_LIST_HEAD(&set->pending_update);

	err = nft_trans_set_add(&ctx, NFT_MSG_NEWSET, set);
	if (err < 0)
		goto err_set_expr_alloc;

	list_add_tail_rcu(&set->list, &table->sets);

	return 0;

err_set_expr_alloc:
	for (i = 0; i < set->num_exprs; i++)
		nft_expr_destroy(&ctx, set->exprs[i]);
err_set_destroy:
	ops->destroy(&ctx, set);
err_set_init:
	kfree(set->name);
err_set_name:
	kvfree(set);
err_alloc:
	nft_use_dec_restore(&table->use);

	return err;
}

static void nft_set_catchall_destroy(const struct nft_ctx *ctx,
				     struct nft_set *set)
{
	struct nft_set_elem_catchall *next, *catchall;

	list_for_each_entry_safe(catchall, next, &set->catchall_list, list) {
		list_del_rcu(&catchall->list);
		nf_tables_set_elem_destroy(ctx, set, catchall->elem);
		kfree_rcu(catchall, rcu);
	}
}

static void nft_set_put(struct nft_set *set)
{
	if (refcount_dec_and_test(&set->refs)) {
		kfree(set->name);
		kvfree(set);
	}
}

static void nft_set_destroy(const struct nft_ctx *ctx, struct nft_set *set)
{
	int i;

	if (WARN_ON(set->use > 0))
		return;

	for (i = 0; i < set->num_exprs; i++)
		nft_expr_destroy(ctx, set->exprs[i]);

	set->ops->destroy(ctx, set);
	nft_set_catchall_destroy(ctx, set);
	nft_set_put(set);
}

static int nf_tables_delset(struct sk_buff *skb, const struct nfnl_info *info,
			    const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_next(info->net);
	u8 family = info->nfmsg->nfgen_family;
	struct net *net = info->net;
	const struct nlattr *attr;
	struct nft_table *table;
	struct nft_set *set;
	struct nft_ctx ctx;

	if (info->nfmsg->nfgen_family == NFPROTO_UNSPEC)
		return -EAFNOSUPPORT;

	table = nft_table_lookup(net, nla[NFTA_SET_TABLE], family,
				 genmask, NETLINK_CB(skb).portid);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_SET_TABLE]);
		return PTR_ERR(table);
	}

	if (nla[NFTA_SET_HANDLE]) {
		attr = nla[NFTA_SET_HANDLE];
		set = nft_set_lookup_byhandle(table, attr, genmask);
	} else {
		attr = nla[NFTA_SET_NAME];
		set = nft_set_lookup(net, table, attr, genmask);
	}

	if (IS_ERR(set)) {
		if (PTR_ERR(set) == -ENOENT &&
		    NFNL_MSG_TYPE(info->nlh->nlmsg_type) == NFT_MSG_DESTROYSET)
			return 0;

		NL_SET_BAD_ATTR(extack, attr);
		return PTR_ERR(set);
	}
	if (set->use ||
	    (info->nlh->nlmsg_flags & NLM_F_NONREC &&
	     atomic_read(&set->nelems) > 0)) {
		NL_SET_BAD_ATTR(extack, attr);
		return -EBUSY;
	}

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, NULL, nla);

	return nft_delset(&ctx, set);
}

static int nft_validate_register_store(const struct nft_ctx *ctx,
				       enum nft_registers reg,
				       const struct nft_data *data,
				       enum nft_data_types type,
				       unsigned int len);

static int nft_setelem_data_validate(const struct nft_ctx *ctx,
				     struct nft_set *set,
				     struct nft_elem_priv *elem_priv)
{
	const struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);
	enum nft_registers dreg;

	dreg = nft_type_to_reg(set->dtype);
	return nft_validate_register_store(ctx, dreg, nft_set_ext_data(ext),
					   set->dtype == NFT_DATA_VERDICT ?
					   NFT_DATA_VERDICT : NFT_DATA_VALUE,
					   set->dlen);
}

static int nf_tables_bind_check_setelem(const struct nft_ctx *ctx,
					struct nft_set *set,
					const struct nft_set_iter *iter,
					struct nft_elem_priv *elem_priv)
{
	const struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);

	if (!nft_set_elem_active(ext, iter->genmask))
		return 0;

	return nft_setelem_data_validate(ctx, set, elem_priv);
}

static int nft_set_catchall_bind_check(const struct nft_ctx *ctx,
				       struct nft_set *set)
{
	u8 genmask = nft_genmask_next(ctx->net);
	struct nft_set_elem_catchall *catchall;
	struct nft_set_ext *ext;
	int ret = 0;

	list_for_each_entry_rcu(catchall, &set->catchall_list, list,
				lockdep_commit_lock_is_held(ctx->net)) {
		ext = nft_set_elem_ext(set, catchall->elem);
		if (!nft_set_elem_active(ext, genmask))
			continue;

		ret = nft_setelem_data_validate(ctx, set, catchall->elem);
		if (ret < 0)
			break;
	}

	return ret;
}

int nf_tables_bind_set(const struct nft_ctx *ctx, struct nft_set *set,
		       struct nft_set_binding *binding)
{
	struct nft_set_binding *i;
	struct nft_set_iter iter;

	if (!list_empty(&set->bindings) && nft_set_is_anonymous(set))
		return -EBUSY;

	if (binding->flags & NFT_SET_MAP) {
		/* If the set is already bound to the same chain all
		 * jumps are already validated for that chain.
		 */
		list_for_each_entry(i, &set->bindings, list) {
			if (i->flags & NFT_SET_MAP &&
			    i->chain == binding->chain)
				goto bind;
		}

		iter.genmask	= nft_genmask_next(ctx->net);
		iter.type	= NFT_ITER_UPDATE;
		iter.skip 	= 0;
		iter.count	= 0;
		iter.err	= 0;
		iter.fn		= nf_tables_bind_check_setelem;

		set->ops->walk(ctx, set, &iter);
		if (!iter.err)
			iter.err = nft_set_catchall_bind_check(ctx, set);

		if (iter.err < 0)
			return iter.err;
	}
bind:
	if (!nft_use_inc(&set->use))
		return -EMFILE;

	binding->chain = ctx->chain;
	list_add_tail_rcu(&binding->list, &set->bindings);
	nft_set_trans_bind(ctx, set);

	return 0;
}
EXPORT_SYMBOL_GPL(nf_tables_bind_set);

static void nf_tables_unbind_set(const struct nft_ctx *ctx, struct nft_set *set,
				 struct nft_set_binding *binding, bool event)
{
	list_del_rcu(&binding->list);

	if (list_empty(&set->bindings) && nft_set_is_anonymous(set)) {
		list_del_rcu(&set->list);
		set->dead = 1;
		if (event)
			nf_tables_set_notify(ctx, set, NFT_MSG_DELSET,
					     GFP_KERNEL);
	}
}

static void nft_setelem_data_activate(const struct net *net,
				      const struct nft_set *set,
				      struct nft_elem_priv *elem_priv);

static int nft_mapelem_activate(const struct nft_ctx *ctx,
				struct nft_set *set,
				const struct nft_set_iter *iter,
				struct nft_elem_priv *elem_priv)
{
	struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);

	/* called from abort path, reverse check to undo changes. */
	if (nft_set_elem_active(ext, iter->genmask))
		return 0;

	nft_clear(ctx->net, ext);
	nft_setelem_data_activate(ctx->net, set, elem_priv);

	return 0;
}

static void nft_map_catchall_activate(const struct nft_ctx *ctx,
				      struct nft_set *set)
{
	u8 genmask = nft_genmask_next(ctx->net);
	struct nft_set_elem_catchall *catchall;
	struct nft_set_ext *ext;

	list_for_each_entry(catchall, &set->catchall_list, list) {
		ext = nft_set_elem_ext(set, catchall->elem);
		if (!nft_set_elem_active(ext, genmask))
			continue;

		nft_clear(ctx->net, ext);
		nft_setelem_data_activate(ctx->net, set, catchall->elem);
		break;
	}
}

static void nft_map_activate(const struct nft_ctx *ctx, struct nft_set *set)
{
	struct nft_set_iter iter = {
		.genmask	= nft_genmask_next(ctx->net),
		.type		= NFT_ITER_UPDATE,
		.fn		= nft_mapelem_activate,
	};

	set->ops->walk(ctx, set, &iter);
	WARN_ON_ONCE(iter.err);

	nft_map_catchall_activate(ctx, set);
}

void nf_tables_activate_set(const struct nft_ctx *ctx, struct nft_set *set)
{
	if (nft_set_is_anonymous(set)) {
		if (set->flags & (NFT_SET_MAP | NFT_SET_OBJECT))
			nft_map_activate(ctx, set);

		nft_clear(ctx->net, set);
	}

	nft_use_inc_restore(&set->use);
}
EXPORT_SYMBOL_GPL(nf_tables_activate_set);

void nf_tables_deactivate_set(const struct nft_ctx *ctx, struct nft_set *set,
			      struct nft_set_binding *binding,
			      enum nft_trans_phase phase)
{
	WARN_ON_ONCE(!lockdep_commit_lock_is_held(ctx->net));

	switch (phase) {
	case NFT_TRANS_PREPARE_ERROR:
		nft_set_trans_unbind(ctx, set);
		if (nft_set_is_anonymous(set))
			nft_deactivate_next(ctx->net, set);
		else
			list_del_rcu(&binding->list);

		nft_use_dec(&set->use);
		break;
	case NFT_TRANS_PREPARE:
		if (nft_set_is_anonymous(set)) {
			if (set->flags & (NFT_SET_MAP | NFT_SET_OBJECT))
				nft_map_deactivate(ctx, set);

			nft_deactivate_next(ctx->net, set);
		}
		nft_use_dec(&set->use);
		return;
	case NFT_TRANS_ABORT:
	case NFT_TRANS_RELEASE:
		if (nft_set_is_anonymous(set) &&
		    set->flags & (NFT_SET_MAP | NFT_SET_OBJECT))
			nft_map_deactivate(ctx, set);

		nft_use_dec(&set->use);
		fallthrough;
	default:
		nf_tables_unbind_set(ctx, set, binding,
				     phase == NFT_TRANS_COMMIT);
	}
}
EXPORT_SYMBOL_GPL(nf_tables_deactivate_set);

void nf_tables_destroy_set(const struct nft_ctx *ctx, struct nft_set *set)
{
	if (list_empty(&set->bindings) && nft_set_is_anonymous(set))
		nft_set_destroy(ctx, set);
}
EXPORT_SYMBOL_GPL(nf_tables_destroy_set);

const struct nft_set_ext_type nft_set_ext_types[] = {
	[NFT_SET_EXT_KEY]		= {
		.align	= __alignof__(u32),
	},
	[NFT_SET_EXT_DATA]		= {
		.align	= __alignof__(u32),
	},
	[NFT_SET_EXT_EXPRESSIONS]	= {
		.align	= __alignof__(struct nft_set_elem_expr),
	},
	[NFT_SET_EXT_OBJREF]		= {
		.len	= sizeof(struct nft_object *),
		.align	= __alignof__(struct nft_object *),
	},
	[NFT_SET_EXT_FLAGS]		= {
		.len	= sizeof(u8),
		.align	= __alignof__(u8),
	},
	[NFT_SET_EXT_TIMEOUT]		= {
		.len	= sizeof(struct nft_timeout),
		.align	= __alignof__(struct nft_timeout),
	},
	[NFT_SET_EXT_USERDATA]		= {
		.len	= sizeof(struct nft_userdata),
		.align	= __alignof__(struct nft_userdata),
	},
	[NFT_SET_EXT_KEY_END]		= {
		.align	= __alignof__(u32),
	},
};

/*
 * Set elements
 */

static const struct nla_policy nft_set_elem_policy[NFTA_SET_ELEM_MAX + 1] = {
	[NFTA_SET_ELEM_KEY]		= { .type = NLA_NESTED },
	[NFTA_SET_ELEM_DATA]		= { .type = NLA_NESTED },
	[NFTA_SET_ELEM_FLAGS]		= { .type = NLA_U32 },
	[NFTA_SET_ELEM_TIMEOUT]		= { .type = NLA_U64 },
	[NFTA_SET_ELEM_EXPIRATION]	= { .type = NLA_U64 },
	[NFTA_SET_ELEM_USERDATA]	= { .type = NLA_BINARY,
					    .len = NFT_USERDATA_MAXLEN },
	[NFTA_SET_ELEM_EXPR]		= { .type = NLA_NESTED },
	[NFTA_SET_ELEM_OBJREF]		= { .type = NLA_STRING,
					    .len = NFT_OBJ_MAXNAMELEN - 1 },
	[NFTA_SET_ELEM_KEY_END]		= { .type = NLA_NESTED },
	[NFTA_SET_ELEM_EXPRESSIONS]	= NLA_POLICY_NESTED_ARRAY(nft_expr_policy),
};

static const struct nla_policy nft_set_elem_list_policy[NFTA_SET_ELEM_LIST_MAX + 1] = {
	[NFTA_SET_ELEM_LIST_TABLE]	= { .type = NLA_STRING,
					    .len = NFT_TABLE_MAXNAMELEN - 1 },
	[NFTA_SET_ELEM_LIST_SET]	= { .type = NLA_STRING,
					    .len = NFT_SET_MAXNAMELEN - 1 },
	[NFTA_SET_ELEM_LIST_ELEMENTS]	= NLA_POLICY_NESTED_ARRAY(nft_set_elem_policy),
	[NFTA_SET_ELEM_LIST_SET_ID]	= { .type = NLA_U32 },
};

static int nft_set_elem_expr_dump(struct sk_buff *skb,
				  const struct nft_set *set,
				  const struct nft_set_ext *ext,
				  bool reset)
{
	struct nft_set_elem_expr *elem_expr;
	u32 size, num_exprs = 0;
	struct nft_expr *expr;
	struct nlattr *nest;

	elem_expr = nft_set_ext_expr(ext);
	nft_setelem_expr_foreach(expr, elem_expr, size)
		num_exprs++;

	if (num_exprs == 1) {
		expr = nft_setelem_expr_at(elem_expr, 0);
		if (nft_expr_dump(skb, NFTA_SET_ELEM_EXPR, expr, reset) < 0)
			return -1;

		return 0;
	} else if (num_exprs > 1) {
		nest = nla_nest_start_noflag(skb, NFTA_SET_ELEM_EXPRESSIONS);
		if (nest == NULL)
			goto nla_put_failure;

		nft_setelem_expr_foreach(expr, elem_expr, size) {
			expr = nft_setelem_expr_at(elem_expr, size);
			if (nft_expr_dump(skb, NFTA_LIST_ELEM, expr, reset) < 0)
				goto nla_put_failure;
		}
		nla_nest_end(skb, nest);
	}
	return 0;

nla_put_failure:
	return -1;
}

static int nf_tables_fill_setelem(struct sk_buff *skb,
				  const struct nft_set *set,
				  const struct nft_elem_priv *elem_priv,
				  bool reset)
{
	const struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);
	unsigned char *b = skb_tail_pointer(skb);
	struct nlattr *nest;

	nest = nla_nest_start_noflag(skb, NFTA_LIST_ELEM);
	if (nest == NULL)
		goto nla_put_failure;

	if (nft_set_ext_exists(ext, NFT_SET_EXT_KEY) &&
	    nft_data_dump(skb, NFTA_SET_ELEM_KEY, nft_set_ext_key(ext),
			  NFT_DATA_VALUE, set->klen) < 0)
		goto nla_put_failure;

	if (nft_set_ext_exists(ext, NFT_SET_EXT_KEY_END) &&
	    nft_data_dump(skb, NFTA_SET_ELEM_KEY_END, nft_set_ext_key_end(ext),
			  NFT_DATA_VALUE, set->klen) < 0)
		goto nla_put_failure;

	if (nft_set_ext_exists(ext, NFT_SET_EXT_DATA) &&
	    nft_data_dump(skb, NFTA_SET_ELEM_DATA, nft_set_ext_data(ext),
			  nft_set_datatype(set), set->dlen) < 0)
		goto nla_put_failure;

	if (nft_set_ext_exists(ext, NFT_SET_EXT_EXPRESSIONS) &&
	    nft_set_elem_expr_dump(skb, set, ext, reset))
		goto nla_put_failure;

	if (nft_set_ext_exists(ext, NFT_SET_EXT_OBJREF) &&
	    nla_put_string(skb, NFTA_SET_ELEM_OBJREF,
			   (*nft_set_ext_obj(ext))->key.name) < 0)
		goto nla_put_failure;

	if (nft_set_ext_exists(ext, NFT_SET_EXT_FLAGS) &&
	    nla_put_be32(skb, NFTA_SET_ELEM_FLAGS,
		         htonl(*nft_set_ext_flags(ext))))
		goto nla_put_failure;

	if (nft_set_ext_exists(ext, NFT_SET_EXT_TIMEOUT)) {
		u64 timeout = READ_ONCE(nft_set_ext_timeout(ext)->timeout);
		u64 set_timeout = READ_ONCE(set->timeout);
		__be64 msecs = 0;

		if (set_timeout != timeout) {
			msecs = nf_jiffies64_to_msecs(timeout);
			if (nla_put_be64(skb, NFTA_SET_ELEM_TIMEOUT, msecs,
					 NFTA_SET_ELEM_PAD))
				goto nla_put_failure;
		}

		if (timeout > 0) {
			u64 expires, now = get_jiffies_64();

			expires = READ_ONCE(nft_set_ext_timeout(ext)->expiration);
			if (time_before64(now, expires))
				expires -= now;
			else
				expires = 0;

			if (nla_put_be64(skb, NFTA_SET_ELEM_EXPIRATION,
					 nf_jiffies64_to_msecs(expires),
					 NFTA_SET_ELEM_PAD))
				goto nla_put_failure;
		}
	}

	if (nft_set_ext_exists(ext, NFT_SET_EXT_USERDATA)) {
		struct nft_userdata *udata;

		udata = nft_set_ext_userdata(ext);
		if (nla_put(skb, NFTA_SET_ELEM_USERDATA,
			    udata->len + 1, udata->data))
			goto nla_put_failure;
	}

	nla_nest_end(skb, nest);
	return 0;

nla_put_failure:
	nlmsg_trim(skb, b);
	return -EMSGSIZE;
}

struct nft_set_dump_args {
	const struct netlink_callback	*cb;
	struct nft_set_iter		iter;
	struct sk_buff			*skb;
	bool				reset;
};

static int nf_tables_dump_setelem(const struct nft_ctx *ctx,
				  struct nft_set *set,
				  const struct nft_set_iter *iter,
				  struct nft_elem_priv *elem_priv)
{
	const struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);
	struct nft_set_dump_args *args;

	if (!nft_set_elem_active(ext, iter->genmask))
		return 0;

	if (nft_set_elem_expired(ext) || nft_set_elem_is_dead(ext))
		return 0;

	args = container_of(iter, struct nft_set_dump_args, iter);
	return nf_tables_fill_setelem(args->skb, set, elem_priv, args->reset);
}

static void audit_log_nft_set_reset(const struct nft_table *table,
				    unsigned int base_seq,
				    unsigned int nentries)
{
	char *buf = kasprintf(GFP_ATOMIC, "%s:%u", table->name, base_seq);

	audit_log_nfcfg(buf, table->family, nentries,
			AUDIT_NFT_OP_SETELEM_RESET, GFP_ATOMIC);
	kfree(buf);
}

struct nft_set_dump_ctx {
	const struct nft_set	*set;
	struct nft_ctx		ctx;
	bool			reset;
};

static int nft_set_catchall_dump(struct net *net, struct sk_buff *skb,
				 const struct nft_set *set, bool reset,
				 unsigned int base_seq)
{
	struct nft_set_elem_catchall *catchall;
	u8 genmask = nft_genmask_cur(net);
	struct nft_set_ext *ext;
	int ret = 0;

	list_for_each_entry_rcu(catchall, &set->catchall_list, list) {
		ext = nft_set_elem_ext(set, catchall->elem);
		if (!nft_set_elem_active(ext, genmask) ||
		    nft_set_elem_expired(ext))
			continue;

		ret = nf_tables_fill_setelem(skb, set, catchall->elem, reset);
		if (reset && !ret)
			audit_log_nft_set_reset(set->table, base_seq, 1);
		break;
	}

	return ret;
}

static int nf_tables_dump_set(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct nft_set_dump_ctx *dump_ctx = cb->data;
	struct net *net = sock_net(skb->sk);
	struct nftables_pernet *nft_net;
	struct nft_table *table;
	struct nft_set *set;
	struct nft_set_dump_args args;
	bool set_found = false;
	struct nlmsghdr *nlh;
	struct nlattr *nest;
	u32 portid, seq;
	int event;

	rcu_read_lock();
	nft_net = nft_pernet(net);
	cb->seq = READ_ONCE(nft_net->base_seq);

	list_for_each_entry_rcu(table, &nft_net->tables, list) {
		if (dump_ctx->ctx.family != NFPROTO_UNSPEC &&
		    dump_ctx->ctx.family != table->family)
			continue;

		if (table != dump_ctx->ctx.table)
			continue;

		list_for_each_entry_rcu(set, &table->sets, list) {
			if (set == dump_ctx->set) {
				set_found = true;
				break;
			}
		}
		break;
	}

	if (!set_found) {
		rcu_read_unlock();
		return -ENOENT;
	}

	event  = nfnl_msg_type(NFNL_SUBSYS_NFTABLES, NFT_MSG_NEWSETELEM);
	portid = NETLINK_CB(cb->skb).portid;
	seq    = cb->nlh->nlmsg_seq;

	nlh = nfnl_msg_put(skb, portid, seq, event, NLM_F_MULTI,
			   table->family, NFNETLINK_V0, nft_base_seq(net));
	if (!nlh)
		goto nla_put_failure;

	if (nla_put_string(skb, NFTA_SET_ELEM_LIST_TABLE, table->name))
		goto nla_put_failure;
	if (nla_put_string(skb, NFTA_SET_ELEM_LIST_SET, set->name))
		goto nla_put_failure;

	nest = nla_nest_start_noflag(skb, NFTA_SET_ELEM_LIST_ELEMENTS);
	if (nest == NULL)
		goto nla_put_failure;

	args.cb			= cb;
	args.skb		= skb;
	args.reset		= dump_ctx->reset;
	args.iter.genmask	= nft_genmask_cur(net);
	args.iter.type		= NFT_ITER_READ;
	args.iter.skip		= cb->args[0];
	args.iter.count		= 0;
	args.iter.err		= 0;
	args.iter.fn		= nf_tables_dump_setelem;
	set->ops->walk(&dump_ctx->ctx, set, &args.iter);

	if (!args.iter.err && args.iter.count == cb->args[0])
		args.iter.err = nft_set_catchall_dump(net, skb, set,
						      dump_ctx->reset, cb->seq);
	nla_nest_end(skb, nest);
	nlmsg_end(skb, nlh);

	rcu_read_unlock();

	if (args.iter.err && args.iter.err != -EMSGSIZE)
		return args.iter.err;
	if (args.iter.count == cb->args[0])
		return 0;

	cb->args[0] = args.iter.count;
	return skb->len;

nla_put_failure:
	rcu_read_unlock();
	return -ENOSPC;
}

static int nf_tables_dumpreset_set(struct sk_buff *skb,
				   struct netlink_callback *cb)
{
	struct nftables_pernet *nft_net = nft_pernet(sock_net(skb->sk));
	struct nft_set_dump_ctx *dump_ctx = cb->data;
	int ret, skip = cb->args[0];

	mutex_lock(&nft_net->commit_mutex);

	ret = nf_tables_dump_set(skb, cb);

	if (cb->args[0] > skip)
		audit_log_nft_set_reset(dump_ctx->ctx.table, cb->seq,
					cb->args[0] - skip);

	mutex_unlock(&nft_net->commit_mutex);

	return ret;
}

static int nf_tables_dump_set_start(struct netlink_callback *cb)
{
	struct nft_set_dump_ctx *dump_ctx = cb->data;

	cb->data = kmemdup(dump_ctx, sizeof(*dump_ctx), GFP_ATOMIC);

	return cb->data ? 0 : -ENOMEM;
}

static int nf_tables_dump_set_done(struct netlink_callback *cb)
{
	kfree(cb->data);
	return 0;
}

static int nf_tables_fill_setelem_info(struct sk_buff *skb,
				       const struct nft_ctx *ctx, u32 seq,
				       u32 portid, int event, u16 flags,
				       const struct nft_set *set,
				       const struct nft_elem_priv *elem_priv,
				       bool reset)
{
	struct nlmsghdr *nlh;
	struct nlattr *nest;
	int err;

	event = nfnl_msg_type(NFNL_SUBSYS_NFTABLES, event);
	nlh = nfnl_msg_put(skb, portid, seq, event, flags, ctx->family,
			   NFNETLINK_V0, nft_base_seq(ctx->net));
	if (!nlh)
		goto nla_put_failure;

	if (nla_put_string(skb, NFTA_SET_TABLE, ctx->table->name))
		goto nla_put_failure;
	if (nla_put_string(skb, NFTA_SET_NAME, set->name))
		goto nla_put_failure;

	nest = nla_nest_start_noflag(skb, NFTA_SET_ELEM_LIST_ELEMENTS);
	if (nest == NULL)
		goto nla_put_failure;

	err = nf_tables_fill_setelem(skb, set, elem_priv, reset);
	if (err < 0)
		goto nla_put_failure;

	nla_nest_end(skb, nest);

	nlmsg_end(skb, nlh);
	return 0;

nla_put_failure:
	nlmsg_trim(skb, nlh);
	return -1;
}

static int nft_setelem_parse_flags(const struct nft_set *set,
				   const struct nlattr *attr, u32 *flags)
{
	if (attr == NULL)
		return 0;

	*flags = ntohl(nla_get_be32(attr));
	if (*flags & ~(NFT_SET_ELEM_INTERVAL_END | NFT_SET_ELEM_CATCHALL))
		return -EOPNOTSUPP;
	if (!(set->flags & NFT_SET_INTERVAL) &&
	    *flags & NFT_SET_ELEM_INTERVAL_END)
		return -EINVAL;
	if ((*flags & (NFT_SET_ELEM_INTERVAL_END | NFT_SET_ELEM_CATCHALL)) ==
	    (NFT_SET_ELEM_INTERVAL_END | NFT_SET_ELEM_CATCHALL))
		return -EINVAL;

	return 0;
}

static int nft_setelem_parse_key(struct nft_ctx *ctx, const struct nft_set *set,
				 struct nft_data *key, struct nlattr *attr)
{
	struct nft_data_desc desc = {
		.type	= NFT_DATA_VALUE,
		.size	= NFT_DATA_VALUE_MAXLEN,
		.len	= set->klen,
	};

	return nft_data_init(ctx, key, &desc, attr);
}

static int nft_setelem_parse_data(struct nft_ctx *ctx, struct nft_set *set,
				  struct nft_data_desc *desc,
				  struct nft_data *data,
				  struct nlattr *attr)
{
	u32 dtype;

	if (set->dtype == NFT_DATA_VERDICT)
		dtype = NFT_DATA_VERDICT;
	else
		dtype = NFT_DATA_VALUE;

	desc->type = dtype;
	desc->size = NFT_DATA_VALUE_MAXLEN;
	desc->len = set->dlen;
	desc->flags = NFT_DATA_DESC_SETELEM;

	return nft_data_init(ctx, data, desc, attr);
}

static void *nft_setelem_catchall_get(const struct net *net,
				      const struct nft_set *set)
{
	struct nft_set_elem_catchall *catchall;
	u8 genmask = nft_genmask_cur(net);
	struct nft_set_ext *ext;
	void *priv = NULL;

	list_for_each_entry_rcu(catchall, &set->catchall_list, list) {
		ext = nft_set_elem_ext(set, catchall->elem);
		if (!nft_set_elem_active(ext, genmask) ||
		    nft_set_elem_expired(ext))
			continue;

		priv = catchall->elem;
		break;
	}

	return priv;
}

static int nft_setelem_get(struct nft_ctx *ctx, const struct nft_set *set,
			   struct nft_set_elem *elem, u32 flags)
{
	void *priv;

	if (!(flags & NFT_SET_ELEM_CATCHALL)) {
		priv = set->ops->get(ctx->net, set, elem, flags);
		if (IS_ERR(priv))
			return PTR_ERR(priv);
	} else {
		priv = nft_setelem_catchall_get(ctx->net, set);
		if (!priv)
			return -ENOENT;
	}
	elem->priv = priv;

	return 0;
}

static int nft_get_set_elem(struct nft_ctx *ctx, const struct nft_set *set,
			    const struct nlattr *attr, bool reset)
{
	struct nlattr *nla[NFTA_SET_ELEM_MAX + 1];
	struct nft_set_elem elem;
	struct sk_buff *skb;
	uint32_t flags = 0;
	int err;

	err = nla_parse_nested_deprecated(nla, NFTA_SET_ELEM_MAX, attr,
					  nft_set_elem_policy, NULL);
	if (err < 0)
		return err;

	err = nft_setelem_parse_flags(set, nla[NFTA_SET_ELEM_FLAGS], &flags);
	if (err < 0)
		return err;

	if (!nla[NFTA_SET_ELEM_KEY] && !(flags & NFT_SET_ELEM_CATCHALL))
		return -EINVAL;

	if (nla[NFTA_SET_ELEM_KEY]) {
		err = nft_setelem_parse_key(ctx, set, &elem.key.val,
					    nla[NFTA_SET_ELEM_KEY]);
		if (err < 0)
			return err;
	}

	if (nla[NFTA_SET_ELEM_KEY_END]) {
		err = nft_setelem_parse_key(ctx, set, &elem.key_end.val,
					    nla[NFTA_SET_ELEM_KEY_END]);
		if (err < 0)
			return err;
	}

	err = nft_setelem_get(ctx, set, &elem, flags);
	if (err < 0)
		return err;

	err = -ENOMEM;
	skb = nlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (skb == NULL)
		return err;

	err = nf_tables_fill_setelem_info(skb, ctx, ctx->seq, ctx->portid,
					  NFT_MSG_NEWSETELEM, 0, set, elem.priv,
					  reset);
	if (err < 0)
		goto err_fill_setelem;

	return nfnetlink_unicast(skb, ctx->net, ctx->portid);

err_fill_setelem:
	kfree_skb(skb);
	return err;
}

static int nft_set_dump_ctx_init(struct nft_set_dump_ctx *dump_ctx,
				 const struct sk_buff *skb,
				 const struct nfnl_info *info,
				 const struct nlattr * const nla[],
				 bool reset)
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_cur(info->net);
	u8 family = info->nfmsg->nfgen_family;
	struct net *net = info->net;
	struct nft_table *table;
	struct nft_set *set;

	table = nft_table_lookup(net, nla[NFTA_SET_ELEM_LIST_TABLE], family,
				 genmask, 0);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_SET_ELEM_LIST_TABLE]);
		return PTR_ERR(table);
	}

	set = nft_set_lookup(net, table, nla[NFTA_SET_ELEM_LIST_SET], genmask);
	if (IS_ERR(set)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_SET_ELEM_LIST_SET]);
		return PTR_ERR(set);
	}

	nft_ctx_init(&dump_ctx->ctx, net, skb,
		     info->nlh, family, table, NULL, nla);
	dump_ctx->set = set;
	dump_ctx->reset = reset;
	return 0;
}

/* called with rcu_read_lock held */
static int nf_tables_getsetelem(struct sk_buff *skb,
				const struct nfnl_info *info,
				const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	struct nft_set_dump_ctx dump_ctx;
	struct nlattr *attr;
	int rem, err = 0;

	if (info->nlh->nlmsg_flags & NLM_F_DUMP) {
		struct netlink_dump_control c = {
			.start = nf_tables_dump_set_start,
			.dump = nf_tables_dump_set,
			.done = nf_tables_dump_set_done,
			.module = THIS_MODULE,
		};

		err = nft_set_dump_ctx_init(&dump_ctx, skb, info, nla, false);
		if (err)
			return err;

		c.data = &dump_ctx;
		return nft_netlink_dump_start_rcu(info->sk, skb, info->nlh, &c);
	}

	if (!nla[NFTA_SET_ELEM_LIST_ELEMENTS])
		return -EINVAL;

	err = nft_set_dump_ctx_init(&dump_ctx, skb, info, nla, false);
	if (err)
		return err;

	nla_for_each_nested(attr, nla[NFTA_SET_ELEM_LIST_ELEMENTS], rem) {
		err = nft_get_set_elem(&dump_ctx.ctx, dump_ctx.set, attr, false);
		if (err < 0) {
			NL_SET_BAD_ATTR(extack, attr);
			break;
		}
	}

	return err;
}

static int nf_tables_getsetelem_reset(struct sk_buff *skb,
				      const struct nfnl_info *info,
				      const struct nlattr * const nla[])
{
	struct nftables_pernet *nft_net = nft_pernet(info->net);
	struct netlink_ext_ack *extack = info->extack;
	struct nft_set_dump_ctx dump_ctx;
	int rem, err = 0, nelems = 0;
	struct nlattr *attr;

	if (info->nlh->nlmsg_flags & NLM_F_DUMP) {
		struct netlink_dump_control c = {
			.start = nf_tables_dump_set_start,
			.dump = nf_tables_dumpreset_set,
			.done = nf_tables_dump_set_done,
			.module = THIS_MODULE,
		};

		err = nft_set_dump_ctx_init(&dump_ctx, skb, info, nla, true);
		if (err)
			return err;

		c.data = &dump_ctx;
		return nft_netlink_dump_start_rcu(info->sk, skb, info->nlh, &c);
	}

	if (!nla[NFTA_SET_ELEM_LIST_ELEMENTS])
		return -EINVAL;

	if (!try_module_get(THIS_MODULE))
		return -EINVAL;
	rcu_read_unlock();
	mutex_lock(&nft_net->commit_mutex);
	rcu_read_lock();

	err = nft_set_dump_ctx_init(&dump_ctx, skb, info, nla, true);
	if (err)
		goto out_unlock;

	nla_for_each_nested(attr, nla[NFTA_SET_ELEM_LIST_ELEMENTS], rem) {
		err = nft_get_set_elem(&dump_ctx.ctx, dump_ctx.set, attr, true);
		if (err < 0) {
			NL_SET_BAD_ATTR(extack, attr);
			break;
		}
		nelems++;
	}
	audit_log_nft_set_reset(dump_ctx.ctx.table, nft_net->base_seq, nelems);

out_unlock:
	rcu_read_unlock();
	mutex_unlock(&nft_net->commit_mutex);
	rcu_read_lock();
	module_put(THIS_MODULE);

	return err;
}

static void nf_tables_setelem_notify(const struct nft_ctx *ctx,
				     const struct nft_set *set,
				     const struct nft_elem_priv *elem_priv,
				     int event)
{
	struct nftables_pernet *nft_net;
	struct net *net = ctx->net;
	u32 portid = ctx->portid;
	struct sk_buff *skb;
	u16 flags = 0;
	int err;

	if (!ctx->report && !nfnetlink_has_listeners(net, NFNLGRP_NFTABLES))
		return;

	skb = nlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (skb == NULL)
		goto err;

	if (ctx->flags & (NLM_F_CREATE | NLM_F_EXCL))
		flags |= ctx->flags & (NLM_F_CREATE | NLM_F_EXCL);

	err = nf_tables_fill_setelem_info(skb, ctx, 0, portid, event, flags,
					  set, elem_priv, false);
	if (err < 0) {
		kfree_skb(skb);
		goto err;
	}

	nft_net = nft_pernet(net);
	nft_notify_enqueue(skb, ctx->report, &nft_net->notify_list);
	return;
err:
	nfnetlink_set_err(net, portid, NFNLGRP_NFTABLES, -ENOBUFS);
}

static struct nft_trans *nft_trans_elem_alloc(const struct nft_ctx *ctx,
					      int msg_type,
					      struct nft_set *set)
{
	struct nft_trans_elem *te;
	struct nft_trans *trans;

	trans = nft_trans_alloc(ctx, msg_type, struct_size(te, elems, 1));
	if (trans == NULL)
		return NULL;

	te = nft_trans_container_elem(trans);
	te->nelems = 1;
	te->set = set;

	return trans;
}

struct nft_expr *nft_set_elem_expr_alloc(const struct nft_ctx *ctx,
					 const struct nft_set *set,
					 const struct nlattr *attr)
{
	struct nft_expr *expr;
	int err;

	expr = nft_expr_init(ctx, attr);
	if (IS_ERR(expr))
		return expr;

	err = -EOPNOTSUPP;
	if (expr->ops->type->flags & NFT_EXPR_GC) {
		if (set->flags & NFT_SET_TIMEOUT)
			goto err_set_elem_expr;
		if (!set->ops->gc_init)
			goto err_set_elem_expr;
		set->ops->gc_init(set);
	}

	return expr;

err_set_elem_expr:
	nft_expr_destroy(ctx, expr);
	return ERR_PTR(err);
}

static int nft_set_ext_check(const struct nft_set_ext_tmpl *tmpl, u8 id, u32 len)
{
	len += nft_set_ext_types[id].len;
	if (len > tmpl->ext_len[id] ||
	    len > U8_MAX)
		return -1;

	return 0;
}

static int nft_set_ext_memcpy(const struct nft_set_ext_tmpl *tmpl, u8 id,
			      void *to, const void *from, u32 len)
{
	if (nft_set_ext_check(tmpl, id, len) < 0)
		return -1;

	memcpy(to, from, len);

	return 0;
}

struct nft_elem_priv *nft_set_elem_init(const struct nft_set *set,
					const struct nft_set_ext_tmpl *tmpl,
					const u32 *key, const u32 *key_end,
					const u32 *data,
					u64 timeout, u64 expiration, gfp_t gfp)
{
	struct nft_set_ext *ext;
	void *elem;

	elem = kzalloc(set->ops->elemsize + tmpl->len, gfp);
	if (elem == NULL)
		return ERR_PTR(-ENOMEM);

	ext = nft_set_elem_ext(set, elem);
	nft_set_ext_init(ext, tmpl);

	if (nft_set_ext_exists(ext, NFT_SET_EXT_KEY) &&
	    nft_set_ext_memcpy(tmpl, NFT_SET_EXT_KEY,
			       nft_set_ext_key(ext), key, set->klen) < 0)
		goto err_ext_check;

	if (nft_set_ext_exists(ext, NFT_SET_EXT_KEY_END) &&
	    nft_set_ext_memcpy(tmpl, NFT_SET_EXT_KEY_END,
			       nft_set_ext_key_end(ext), key_end, set->klen) < 0)
		goto err_ext_check;

	if (nft_set_ext_exists(ext, NFT_SET_EXT_DATA) &&
	    nft_set_ext_memcpy(tmpl, NFT_SET_EXT_DATA,
			       nft_set_ext_data(ext), data, set->dlen) < 0)
		goto err_ext_check;

	if (nft_set_ext_exists(ext, NFT_SET_EXT_TIMEOUT)) {
		nft_set_ext_timeout(ext)->timeout = timeout;

		if (expiration == 0)
			expiration = timeout;

		nft_set_ext_timeout(ext)->expiration = get_jiffies_64() + expiration;
	}

	return elem;

err_ext_check:
	kfree(elem);

	return ERR_PTR(-EINVAL);
}

static void __nft_set_elem_expr_destroy(const struct nft_ctx *ctx,
					struct nft_expr *expr)
{
	if (expr->ops->destroy_clone) {
		expr->ops->destroy_clone(ctx, expr);
		module_put(expr->ops->type->owner);
	} else {
		nf_tables_expr_destroy(ctx, expr);
	}
}

static void nft_set_elem_expr_destroy(const struct nft_ctx *ctx,
				      struct nft_set_elem_expr *elem_expr)
{
	struct nft_expr *expr;
	u32 size;

	nft_setelem_expr_foreach(expr, elem_expr, size)
		__nft_set_elem_expr_destroy(ctx, expr);
}

/* Drop references and destroy. Called from gc, dynset and abort path. */
static void __nft_set_elem_destroy(const struct nft_ctx *ctx,
				   const struct nft_set *set,
				   const struct nft_elem_priv *elem_priv,
				   bool destroy_expr)
{
	struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);

	nft_data_release(nft_set_ext_key(ext), NFT_DATA_VALUE);
	if (nft_set_ext_exists(ext, NFT_SET_EXT_DATA))
		nft_data_release(nft_set_ext_data(ext), set->dtype);
	if (destroy_expr && nft_set_ext_exists(ext, NFT_SET_EXT_EXPRESSIONS))
		nft_set_elem_expr_destroy(ctx, nft_set_ext_expr(ext));
	if (nft_set_ext_exists(ext, NFT_SET_EXT_OBJREF))
		nft_use_dec(&(*nft_set_ext_obj(ext))->use);

	kfree(elem_priv);
}

/* Drop references and destroy. Called from gc and dynset. */
void nft_set_elem_destroy(const struct nft_set *set,
			  const struct nft_elem_priv *elem_priv,
			  bool destroy_expr)
{
	struct nft_ctx ctx = {
		.net	= read_pnet(&set->net),
		.family	= set->table->family,
	};

	__nft_set_elem_destroy(&ctx, set, elem_priv, destroy_expr);
}
EXPORT_SYMBOL_GPL(nft_set_elem_destroy);

/* Drop references and destroy. Called from abort path. */
static void nft_trans_set_elem_destroy(const struct nft_ctx *ctx, struct nft_trans_elem *te)
{
	int i;

	for (i = 0; i < te->nelems; i++) {
		/* skip update request, see nft_trans_elems_new_abort() */
		if (!te->elems[i].priv)
			continue;

		__nft_set_elem_destroy(ctx, te->set, te->elems[i].priv, true);
	}
}

/* Destroy element. References have been already dropped in the preparation
 * path via nft_setelem_data_deactivate().
 */
void nf_tables_set_elem_destroy(const struct nft_ctx *ctx,
				const struct nft_set *set,
				const struct nft_elem_priv *elem_priv)
{
	struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);

	if (nft_set_ext_exists(ext, NFT_SET_EXT_EXPRESSIONS))
		nft_set_elem_expr_destroy(ctx, nft_set_ext_expr(ext));

	kfree(elem_priv);
}

static void nft_trans_elems_destroy(const struct nft_ctx *ctx,
				    const struct nft_trans_elem *te)
{
	int i;

	for (i = 0; i < te->nelems; i++)
		nf_tables_set_elem_destroy(ctx, te->set, te->elems[i].priv);
}

int nft_set_elem_expr_clone(const struct nft_ctx *ctx, struct nft_set *set,
			    struct nft_expr *expr_array[])
{
	struct nft_expr *expr;
	int err, i, k;

	for (i = 0; i < set->num_exprs; i++) {
		expr = kzalloc(set->exprs[i]->ops->size, GFP_KERNEL_ACCOUNT);
		if (!expr)
			goto err_expr;

		err = nft_expr_clone(expr, set->exprs[i], GFP_KERNEL_ACCOUNT);
		if (err < 0) {
			kfree(expr);
			goto err_expr;
		}
		expr_array[i] = expr;
	}

	return 0;

err_expr:
	for (k = i - 1; k >= 0; k--)
		nft_expr_destroy(ctx, expr_array[k]);

	return -ENOMEM;
}

static int nft_set_elem_expr_setup(struct nft_ctx *ctx,
				   const struct nft_set_ext_tmpl *tmpl,
				   const struct nft_set_ext *ext,
				   struct nft_expr *expr_array[],
				   u32 num_exprs)
{
	struct nft_set_elem_expr *elem_expr = nft_set_ext_expr(ext);
	u32 len = sizeof(struct nft_set_elem_expr);
	struct nft_expr *expr;
	int i, err;

	if (num_exprs == 0)
		return 0;

	for (i = 0; i < num_exprs; i++)
		len += expr_array[i]->ops->size;

	if (nft_set_ext_check(tmpl, NFT_SET_EXT_EXPRESSIONS, len) < 0)
		return -EINVAL;

	for (i = 0; i < num_exprs; i++) {
		expr = nft_setelem_expr_at(elem_expr, elem_expr->size);
		err = nft_expr_clone(expr, expr_array[i], GFP_KERNEL_ACCOUNT);
		if (err < 0)
			goto err_elem_expr_setup;

		elem_expr->size += expr_array[i]->ops->size;
		nft_expr_destroy(ctx, expr_array[i]);
		expr_array[i] = NULL;
	}

	return 0;

err_elem_expr_setup:
	for (; i < num_exprs; i++) {
		nft_expr_destroy(ctx, expr_array[i]);
		expr_array[i] = NULL;
	}

	return -ENOMEM;
}

struct nft_set_ext *nft_set_catchall_lookup(const struct net *net,
					    const struct nft_set *set)
{
	struct nft_set_elem_catchall *catchall;
	u8 genmask = nft_genmask_cur(net);
	struct nft_set_ext *ext;

	list_for_each_entry_rcu(catchall, &set->catchall_list, list) {
		ext = nft_set_elem_ext(set, catchall->elem);
		if (nft_set_elem_active(ext, genmask) &&
		    !nft_set_elem_expired(ext) &&
		    !nft_set_elem_is_dead(ext))
			return ext;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(nft_set_catchall_lookup);

static int nft_setelem_catchall_insert(const struct net *net,
				       struct nft_set *set,
				       const struct nft_set_elem *elem,
				       struct nft_elem_priv **priv)
{
	struct nft_set_elem_catchall *catchall;
	u8 genmask = nft_genmask_next(net);
	struct nft_set_ext *ext;

	list_for_each_entry(catchall, &set->catchall_list, list) {
		ext = nft_set_elem_ext(set, catchall->elem);
		if (nft_set_elem_active(ext, genmask)) {
			*priv = catchall->elem;
			return -EEXIST;
		}
	}

	catchall = kmalloc(sizeof(*catchall), GFP_KERNEL_ACCOUNT);
	if (!catchall)
		return -ENOMEM;

	catchall->elem = elem->priv;
	list_add_tail_rcu(&catchall->list, &set->catchall_list);

	return 0;
}

static int nft_setelem_insert(const struct net *net,
			      struct nft_set *set,
			      const struct nft_set_elem *elem,
			      struct nft_elem_priv **elem_priv,
			      unsigned int flags)
{
	int ret;

	if (flags & NFT_SET_ELEM_CATCHALL)
		ret = nft_setelem_catchall_insert(net, set, elem, elem_priv);
	else
		ret = set->ops->insert(net, set, elem, elem_priv);

	return ret;
}

static bool nft_setelem_is_catchall(const struct nft_set *set,
				    const struct nft_elem_priv *elem_priv)
{
	struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);

	if (nft_set_ext_exists(ext, NFT_SET_EXT_FLAGS) &&
	    *nft_set_ext_flags(ext) & NFT_SET_ELEM_CATCHALL)
		return true;

	return false;
}

static void nft_setelem_activate(struct net *net, struct nft_set *set,
				 struct nft_elem_priv *elem_priv)
{
	struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);

	if (nft_setelem_is_catchall(set, elem_priv)) {
		nft_clear(net, ext);
	} else {
		set->ops->activate(net, set, elem_priv);
	}
}

static void nft_trans_elem_update(const struct nft_set *set,
				  const struct nft_trans_one_elem *elem)
{
	const struct nft_set_ext *ext = nft_set_elem_ext(set, elem->priv);
	const struct nft_elem_update *update = elem->update;

	if (update->flags & NFT_TRANS_UPD_TIMEOUT)
		WRITE_ONCE(nft_set_ext_timeout(ext)->timeout, update->timeout);

	if (update->flags & NFT_TRANS_UPD_EXPIRATION)
		WRITE_ONCE(nft_set_ext_timeout(ext)->expiration, get_jiffies_64() + update->expiration);
}

static void nft_trans_elems_add(const struct nft_ctx *ctx,
				struct nft_trans_elem *te)
{
	int i;

	for (i = 0; i < te->nelems; i++) {
		struct nft_trans_one_elem *elem = &te->elems[i];

		if (elem->update)
			nft_trans_elem_update(te->set, elem);
		else
			nft_setelem_activate(ctx->net, te->set, elem->priv);

		nf_tables_setelem_notify(ctx, te->set, elem->priv,
					 NFT_MSG_NEWSETELEM);
		kfree(elem->update);
	}
}

static int nft_setelem_catchall_deactivate(const struct net *net,
					   struct nft_set *set,
					   struct nft_set_elem *elem)
{
	struct nft_set_elem_catchall *catchall;
	struct nft_set_ext *ext;

	list_for_each_entry(catchall, &set->catchall_list, list) {
		ext = nft_set_elem_ext(set, catchall->elem);
		if (!nft_is_active_next(net, ext))
			continue;

		kfree(elem->priv);
		elem->priv = catchall->elem;
		nft_set_elem_change_active(net, set, ext);
		return 0;
	}

	return -ENOENT;
}

static int __nft_setelem_deactivate(const struct net *net,
				    struct nft_set *set,
				    struct nft_set_elem *elem)
{
	void *priv;

	priv = set->ops->deactivate(net, set, elem);
	if (!priv)
		return -ENOENT;

	kfree(elem->priv);
	elem->priv = priv;
	set->ndeact++;

	return 0;
}

static int nft_setelem_deactivate(const struct net *net,
				  struct nft_set *set,
				  struct nft_set_elem *elem, u32 flags)
{
	int ret;

	if (flags & NFT_SET_ELEM_CATCHALL)
		ret = nft_setelem_catchall_deactivate(net, set, elem);
	else
		ret = __nft_setelem_deactivate(net, set, elem);

	return ret;
}

static void nft_setelem_catchall_destroy(struct nft_set_elem_catchall *catchall)
{
	list_del_rcu(&catchall->list);
	kfree_rcu(catchall, rcu);
}

static void nft_setelem_catchall_remove(const struct net *net,
					const struct nft_set *set,
					struct nft_elem_priv *elem_priv)
{
	struct nft_set_elem_catchall *catchall, *next;

	list_for_each_entry_safe(catchall, next, &set->catchall_list, list) {
		if (catchall->elem == elem_priv) {
			nft_setelem_catchall_destroy(catchall);
			break;
		}
	}
}

static void nft_setelem_remove(const struct net *net,
			       const struct nft_set *set,
			       struct nft_elem_priv *elem_priv)
{
	if (nft_setelem_is_catchall(set, elem_priv))
		nft_setelem_catchall_remove(net, set, elem_priv);
	else
		set->ops->remove(net, set, elem_priv);
}

static void nft_trans_elems_remove(const struct nft_ctx *ctx,
				   const struct nft_trans_elem *te)
{
	int i;

	for (i = 0; i < te->nelems; i++) {
		WARN_ON_ONCE(te->elems[i].update);

		nf_tables_setelem_notify(ctx, te->set,
					 te->elems[i].priv,
					 te->nft_trans.msg_type);

		nft_setelem_remove(ctx->net, te->set, te->elems[i].priv);
		if (!nft_setelem_is_catchall(te->set, te->elems[i].priv)) {
			atomic_dec(&te->set->nelems);
			te->set->ndeact--;
		}
	}
}

static bool nft_setelem_valid_key_end(const struct nft_set *set,
				      struct nlattr **nla, u32 flags)
{
	if ((set->flags & (NFT_SET_CONCAT | NFT_SET_INTERVAL)) ==
			  (NFT_SET_CONCAT | NFT_SET_INTERVAL)) {
		if (flags & NFT_SET_ELEM_INTERVAL_END)
			return false;

		if (nla[NFTA_SET_ELEM_KEY_END] &&
		    flags & NFT_SET_ELEM_CATCHALL)
			return false;
	} else {
		if (nla[NFTA_SET_ELEM_KEY_END])
			return false;
	}

	return true;
}

static u32 nft_set_maxsize(const struct nft_set *set)
{
	u32 maxsize, delta;

	if (!set->size)
		return UINT_MAX;

	if (set->ops->adjust_maxsize)
		delta = set->ops->adjust_maxsize(set);
	else
		delta = 0;

	if (check_add_overflow(set->size, set->ndeact, &maxsize))
		return UINT_MAX;

	if (check_add_overflow(maxsize, delta, &maxsize))
		return UINT_MAX;

	return maxsize;
}

static int nft_add_set_elem(struct nft_ctx *ctx, struct nft_set *set,
			    const struct nlattr *attr, u32 nlmsg_flags)
{
	struct nft_expr *expr_array[NFT_SET_EXPR_MAX] = {};
	struct nlattr *nla[NFTA_SET_ELEM_MAX + 1];
	u8 genmask = nft_genmask_next(ctx->net);
	u32 flags = 0, size = 0, num_exprs = 0;
	struct nft_set_ext_tmpl tmpl;
	struct nft_set_ext *ext, *ext2;
	struct nft_set_elem elem;
	struct nft_set_binding *binding;
	struct nft_elem_priv *elem_priv;
	struct nft_object *obj = NULL;
	struct nft_userdata *udata;
	struct nft_data_desc desc;
	enum nft_registers dreg;
	struct nft_trans *trans;
	u64 expiration;
	u64 timeout;
	int err, i;
	u8 ulen;

	err = nla_parse_nested_deprecated(nla, NFTA_SET_ELEM_MAX, attr,
					  nft_set_elem_policy, NULL);
	if (err < 0)
		return err;

	nft_set_ext_prepare(&tmpl);

	err = nft_setelem_parse_flags(set, nla[NFTA_SET_ELEM_FLAGS], &flags);
	if (err < 0)
		return err;

	if (((flags & NFT_SET_ELEM_CATCHALL) && nla[NFTA_SET_ELEM_KEY]) ||
	    (!(flags & NFT_SET_ELEM_CATCHALL) && !nla[NFTA_SET_ELEM_KEY]))
		return -EINVAL;

	if (flags != 0) {
		err = nft_set_ext_add(&tmpl, NFT_SET_EXT_FLAGS);
		if (err < 0)
			return err;
	}

	if (set->flags & NFT_SET_MAP) {
		if (nla[NFTA_SET_ELEM_DATA] == NULL &&
		    !(flags & NFT_SET_ELEM_INTERVAL_END))
			return -EINVAL;
	} else {
		if (nla[NFTA_SET_ELEM_DATA] != NULL)
			return -EINVAL;
	}

	if (set->flags & NFT_SET_OBJECT) {
		if (!nla[NFTA_SET_ELEM_OBJREF] &&
		    !(flags & NFT_SET_ELEM_INTERVAL_END))
			return -EINVAL;
	} else {
		if (nla[NFTA_SET_ELEM_OBJREF])
			return -EINVAL;
	}

	if (!nft_setelem_valid_key_end(set, nla, flags))
		return -EINVAL;

	if ((flags & NFT_SET_ELEM_INTERVAL_END) &&
	     (nla[NFTA_SET_ELEM_DATA] ||
	      nla[NFTA_SET_ELEM_OBJREF] ||
	      nla[NFTA_SET_ELEM_TIMEOUT] ||
	      nla[NFTA_SET_ELEM_EXPIRATION] ||
	      nla[NFTA_SET_ELEM_USERDATA] ||
	      nla[NFTA_SET_ELEM_EXPR] ||
	      nla[NFTA_SET_ELEM_KEY_END] ||
	      nla[NFTA_SET_ELEM_EXPRESSIONS]))
		return -EINVAL;

	timeout = 0;
	if (nla[NFTA_SET_ELEM_TIMEOUT] != NULL) {
		if (!(set->flags & NFT_SET_TIMEOUT))
			return -EINVAL;
		err = nf_msecs_to_jiffies64(nla[NFTA_SET_ELEM_TIMEOUT],
					    &timeout);
		if (err)
			return err;
	} else if (set->flags & NFT_SET_TIMEOUT &&
		   !(flags & NFT_SET_ELEM_INTERVAL_END)) {
		timeout = set->timeout;
	}

	expiration = 0;
	if (nla[NFTA_SET_ELEM_EXPIRATION] != NULL) {
		if (!(set->flags & NFT_SET_TIMEOUT))
			return -EINVAL;
		if (timeout == 0)
			return -EOPNOTSUPP;

		err = nf_msecs_to_jiffies64(nla[NFTA_SET_ELEM_EXPIRATION],
					    &expiration);
		if (err)
			return err;

		if (expiration > timeout)
			return -ERANGE;
	}

	if (nla[NFTA_SET_ELEM_EXPR]) {
		struct nft_expr *expr;

		if (set->num_exprs && set->num_exprs != 1)
			return -EOPNOTSUPP;

		expr = nft_set_elem_expr_alloc(ctx, set,
					       nla[NFTA_SET_ELEM_EXPR]);
		if (IS_ERR(expr))
			return PTR_ERR(expr);

		expr_array[0] = expr;
		num_exprs = 1;

		if (set->num_exprs && set->exprs[0]->ops != expr->ops) {
			err = -EOPNOTSUPP;
			goto err_set_elem_expr;
		}
	} else if (nla[NFTA_SET_ELEM_EXPRESSIONS]) {
		struct nft_expr *expr;
		struct nlattr *tmp;
		int left;

		i = 0;
		nla_for_each_nested(tmp, nla[NFTA_SET_ELEM_EXPRESSIONS], left) {
			if (i == NFT_SET_EXPR_MAX ||
			    (set->num_exprs && set->num_exprs == i)) {
				err = -E2BIG;
				goto err_set_elem_expr;
			}
			if (nla_type(tmp) != NFTA_LIST_ELEM) {
				err = -EINVAL;
				goto err_set_elem_expr;
			}
			expr = nft_set_elem_expr_alloc(ctx, set, tmp);
			if (IS_ERR(expr)) {
				err = PTR_ERR(expr);
				goto err_set_elem_expr;
			}
			expr_array[i] = expr;
			num_exprs++;

			if (set->num_exprs && expr->ops != set->exprs[i]->ops) {
				err = -EOPNOTSUPP;
				goto err_set_elem_expr;
			}
			i++;
		}
		if (set->num_exprs && set->num_exprs != i) {
			err = -EOPNOTSUPP;
			goto err_set_elem_expr;
		}
	} else if (set->num_exprs > 0 &&
		   !(flags & NFT_SET_ELEM_INTERVAL_END)) {
		err = nft_set_elem_expr_clone(ctx, set, expr_array);
		if (err < 0)
			goto err_set_elem_expr_clone;

		num_exprs = set->num_exprs;
	}

	if (nla[NFTA_SET_ELEM_KEY]) {
		err = nft_setelem_parse_key(ctx, set, &elem.key.val,
					    nla[NFTA_SET_ELEM_KEY]);
		if (err < 0)
			goto err_set_elem_expr;

		err = nft_set_ext_add_length(&tmpl, NFT_SET_EXT_KEY, set->klen);
		if (err < 0)
			goto err_parse_key;
	}

	if (nla[NFTA_SET_ELEM_KEY_END]) {
		err = nft_setelem_parse_key(ctx, set, &elem.key_end.val,
					    nla[NFTA_SET_ELEM_KEY_END]);
		if (err < 0)
			goto err_parse_key;

		err = nft_set_ext_add_length(&tmpl, NFT_SET_EXT_KEY_END, set->klen);
		if (err < 0)
			goto err_parse_key_end;
	}

	if (set->flags & NFT_SET_TIMEOUT) {
		err = nft_set_ext_add(&tmpl, NFT_SET_EXT_TIMEOUT);
		if (err < 0)
			goto err_parse_key_end;
	}

	if (num_exprs) {
		for (i = 0; i < num_exprs; i++)
			size += expr_array[i]->ops->size;

		err = nft_set_ext_add_length(&tmpl, NFT_SET_EXT_EXPRESSIONS,
					     sizeof(struct nft_set_elem_expr) + size);
		if (err < 0)
			goto err_parse_key_end;
	}

	if (nla[NFTA_SET_ELEM_OBJREF] != NULL) {
		obj = nft_obj_lookup(ctx->net, ctx->table,
				     nla[NFTA_SET_ELEM_OBJREF],
				     set->objtype, genmask);
		if (IS_ERR(obj)) {
			err = PTR_ERR(obj);
			obj = NULL;
			goto err_parse_key_end;
		}

		if (!nft_use_inc(&obj->use)) {
			err = -EMFILE;
			obj = NULL;
			goto err_parse_key_end;
		}

		err = nft_set_ext_add(&tmpl, NFT_SET_EXT_OBJREF);
		if (err < 0)
			goto err_parse_key_end;
	}

	if (nla[NFTA_SET_ELEM_DATA] != NULL) {
		err = nft_setelem_parse_data(ctx, set, &desc, &elem.data.val,
					     nla[NFTA_SET_ELEM_DATA]);
		if (err < 0)
			goto err_parse_key_end;

		dreg = nft_type_to_reg(set->dtype);
		list_for_each_entry(binding, &set->bindings, list) {
			struct nft_ctx bind_ctx = {
				.net	= ctx->net,
				.family	= ctx->family,
				.table	= ctx->table,
				.chain	= (struct nft_chain *)binding->chain,
			};

			if (!(binding->flags & NFT_SET_MAP))
				continue;

			err = nft_validate_register_store(&bind_ctx, dreg,
							  &elem.data.val,
							  desc.type, desc.len);
			if (err < 0)
				goto err_parse_data;

			if (desc.type == NFT_DATA_VERDICT &&
			    (elem.data.val.verdict.code == NFT_GOTO ||
			     elem.data.val.verdict.code == NFT_JUMP))
				nft_validate_state_update(ctx->table,
							  NFT_VALIDATE_NEED);
		}

		err = nft_set_ext_add_length(&tmpl, NFT_SET_EXT_DATA, desc.len);
		if (err < 0)
			goto err_parse_data;
	}

	/* The full maximum length of userdata can exceed the maximum
	 * offset value (U8_MAX) for following extensions, therefor it
	 * must be the last extension added.
	 */
	ulen = 0;
	if (nla[NFTA_SET_ELEM_USERDATA] != NULL) {
		ulen = nla_len(nla[NFTA_SET_ELEM_USERDATA]);
		if (ulen > 0) {
			err = nft_set_ext_add_length(&tmpl, NFT_SET_EXT_USERDATA,
						     ulen);
			if (err < 0)
				goto err_parse_data;
		}
	}

	elem.priv = nft_set_elem_init(set, &tmpl, elem.key.val.data,
				      elem.key_end.val.data, elem.data.val.data,
				      timeout, expiration, GFP_KERNEL_ACCOUNT);
	if (IS_ERR(elem.priv)) {
		err = PTR_ERR(elem.priv);
		goto err_parse_data;
	}

	ext = nft_set_elem_ext(set, elem.priv);
	if (flags)
		*nft_set_ext_flags(ext) = flags;

	if (obj)
		*nft_set_ext_obj(ext) = obj;

	if (ulen > 0) {
		if (nft_set_ext_check(&tmpl, NFT_SET_EXT_USERDATA, ulen) < 0) {
			err = -EINVAL;
			goto err_elem_free;
		}
		udata = nft_set_ext_userdata(ext);
		udata->len = ulen - 1;
		nla_memcpy(&udata->data, nla[NFTA_SET_ELEM_USERDATA], ulen);
	}
	err = nft_set_elem_expr_setup(ctx, &tmpl, ext, expr_array, num_exprs);
	if (err < 0)
		goto err_elem_free;

	trans = nft_trans_elem_alloc(ctx, NFT_MSG_NEWSETELEM, set);
	if (trans == NULL) {
		err = -ENOMEM;
		goto err_elem_free;
	}

	ext->genmask = nft_genmask_cur(ctx->net);

	err = nft_setelem_insert(ctx->net, set, &elem, &elem_priv, flags);
	if (err) {
		if (err == -EEXIST) {
			ext2 = nft_set_elem_ext(set, elem_priv);
			if (nft_set_ext_exists(ext, NFT_SET_EXT_DATA) ^
			    nft_set_ext_exists(ext2, NFT_SET_EXT_DATA) ||
			    nft_set_ext_exists(ext, NFT_SET_EXT_OBJREF) ^
			    nft_set_ext_exists(ext2, NFT_SET_EXT_OBJREF))
				goto err_element_clash;
			if ((nft_set_ext_exists(ext, NFT_SET_EXT_DATA) &&
			     nft_set_ext_exists(ext2, NFT_SET_EXT_DATA) &&
			     memcmp(nft_set_ext_data(ext),
				    nft_set_ext_data(ext2), set->dlen) != 0) ||
			    (nft_set_ext_exists(ext, NFT_SET_EXT_OBJREF) &&
			     nft_set_ext_exists(ext2, NFT_SET_EXT_OBJREF) &&
			     *nft_set_ext_obj(ext) != *nft_set_ext_obj(ext2)))
				goto err_element_clash;
			else if (!(nlmsg_flags & NLM_F_EXCL)) {
				err = 0;
				if (nft_set_ext_exists(ext2, NFT_SET_EXT_TIMEOUT)) {
					struct nft_elem_update update = { };

					if (timeout != nft_set_ext_timeout(ext2)->timeout) {
						update.timeout = timeout;
						if (expiration == 0)
							expiration = timeout;

						update.flags |= NFT_TRANS_UPD_TIMEOUT;
					}
					if (expiration) {
						update.expiration = expiration;
						update.flags |= NFT_TRANS_UPD_EXPIRATION;
					}

					if (update.flags) {
						struct nft_trans_one_elem *ue;

						ue = &nft_trans_container_elem(trans)->elems[0];

						ue->update = kmemdup(&update, sizeof(update), GFP_KERNEL);
						if (!ue->update) {
							err = -ENOMEM;
							goto err_element_clash;
						}

						ue->priv = elem_priv;
						nft_trans_commit_list_add_elem(ctx->net, trans, GFP_KERNEL);
						goto err_elem_free;
					}
				}
			}
		} else if (err == -ENOTEMPTY) {
			/* ENOTEMPTY reports overlapping between this element
			 * and an existing one.
			 */
			err = -EEXIST;
		}
		goto err_element_clash;
	}

	if (!(flags & NFT_SET_ELEM_CATCHALL)) {
		unsigned int max = nft_set_maxsize(set);

		if (!atomic_add_unless(&set->nelems, 1, max)) {
			err = -ENFILE;
			goto err_set_full;
		}
	}

	nft_trans_container_elem(trans)->elems[0].priv = elem.priv;
	nft_trans_commit_list_add_elem(ctx->net, trans, GFP_KERNEL);
	return 0;

err_set_full:
	nft_setelem_remove(ctx->net, set, elem.priv);
err_element_clash:
	kfree(trans);
err_elem_free:
	nf_tables_set_elem_destroy(ctx, set, elem.priv);
err_parse_data:
	if (nla[NFTA_SET_ELEM_DATA] != NULL)
		nft_data_release(&elem.data.val, desc.type);
err_parse_key_end:
	if (obj)
		nft_use_dec_restore(&obj->use);

	nft_data_release(&elem.key_end.val, NFT_DATA_VALUE);
err_parse_key:
	nft_data_release(&elem.key.val, NFT_DATA_VALUE);
err_set_elem_expr:
	for (i = 0; i < num_exprs && expr_array[i]; i++)
		nft_expr_destroy(ctx, expr_array[i]);
err_set_elem_expr_clone:
	return err;
}

static int nf_tables_newsetelem(struct sk_buff *skb,
				const struct nfnl_info *info,
				const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_next(info->net);
	u8 family = info->nfmsg->nfgen_family;
	struct net *net = info->net;
	const struct nlattr *attr;
	struct nft_table *table;
	struct nft_set *set;
	struct nft_ctx ctx;
	int rem, err;

	if (nla[NFTA_SET_ELEM_LIST_ELEMENTS] == NULL)
		return -EINVAL;

	table = nft_table_lookup(net, nla[NFTA_SET_ELEM_LIST_TABLE], family,
				 genmask, NETLINK_CB(skb).portid);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_SET_ELEM_LIST_TABLE]);
		return PTR_ERR(table);
	}

	set = nft_set_lookup_global(net, table, nla[NFTA_SET_ELEM_LIST_SET],
				    nla[NFTA_SET_ELEM_LIST_SET_ID], genmask);
	if (IS_ERR(set)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_SET_ELEM_LIST_SET]);
		return PTR_ERR(set);
	}

	if (!list_empty(&set->bindings) &&
	    (set->flags & (NFT_SET_CONSTANT | NFT_SET_ANONYMOUS)))
		return -EBUSY;

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, NULL, nla);

	nla_for_each_nested(attr, nla[NFTA_SET_ELEM_LIST_ELEMENTS], rem) {
		err = nft_add_set_elem(&ctx, set, attr, info->nlh->nlmsg_flags);
		if (err < 0) {
			NL_SET_BAD_ATTR(extack, attr);
			return err;
		}
	}

	if (table->validate_state == NFT_VALIDATE_DO)
		return nft_table_validate(net, table);

	return 0;
}

/**
 *	nft_data_hold - hold a nft_data item
 *
 *	@data: struct nft_data to release
 *	@type: type of data
 *
 *	Hold a nft_data item. NFT_DATA_VALUE types can be silently discarded,
 *	NFT_DATA_VERDICT bumps the reference to chains in case of NFT_JUMP and
 *	NFT_GOTO verdicts. This function must be called on active data objects
 *	from the second phase of the commit protocol.
 */
void nft_data_hold(const struct nft_data *data, enum nft_data_types type)
{
	struct nft_chain *chain;

	if (type == NFT_DATA_VERDICT) {
		switch (data->verdict.code) {
		case NFT_JUMP:
		case NFT_GOTO:
			chain = data->verdict.chain;
			nft_use_inc_restore(&chain->use);
			break;
		}
	}
}

static int nft_setelem_active_next(const struct net *net,
				   const struct nft_set *set,
				   struct nft_elem_priv *elem_priv)
{
	const struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);
	u8 genmask = nft_genmask_next(net);

	return nft_set_elem_active(ext, genmask);
}

static void nft_setelem_data_activate(const struct net *net,
				      const struct nft_set *set,
				      struct nft_elem_priv *elem_priv)
{
	const struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);

	if (nft_set_ext_exists(ext, NFT_SET_EXT_DATA))
		nft_data_hold(nft_set_ext_data(ext), set->dtype);
	if (nft_set_ext_exists(ext, NFT_SET_EXT_OBJREF))
		nft_use_inc_restore(&(*nft_set_ext_obj(ext))->use);
}

void nft_setelem_data_deactivate(const struct net *net,
				 const struct nft_set *set,
				 struct nft_elem_priv *elem_priv)
{
	const struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);

	if (nft_set_ext_exists(ext, NFT_SET_EXT_DATA))
		nft_data_release(nft_set_ext_data(ext), set->dtype);
	if (nft_set_ext_exists(ext, NFT_SET_EXT_OBJREF))
		nft_use_dec(&(*nft_set_ext_obj(ext))->use);
}

/* similar to nft_trans_elems_remove, but called from abort path to undo newsetelem.
 * No notifications and no ndeact changes.
 *
 * Returns true if set had been added to (i.e., elements need to be removed again).
 */
static bool nft_trans_elems_new_abort(const struct nft_ctx *ctx,
				      struct nft_trans_elem *te)
{
	bool removed = false;
	int i;

	for (i = 0; i < te->nelems; i++) {
		if (te->elems[i].update) {
			kfree(te->elems[i].update);
			te->elems[i].update = NULL;
			/* Update request, so do not release this element */
			te->elems[i].priv = NULL;
			continue;
		}

		if (!te->set->ops->abort || nft_setelem_is_catchall(te->set, te->elems[i].priv))
			nft_setelem_remove(ctx->net, te->set, te->elems[i].priv);

		if (!nft_setelem_is_catchall(te->set, te->elems[i].priv))
			atomic_dec(&te->set->nelems);

		removed = true;
	}

	return removed;
}

/* Called from abort path to undo DELSETELEM/DESTROYSETELEM. */
static void nft_trans_elems_destroy_abort(const struct nft_ctx *ctx,
					  const struct nft_trans_elem *te)
{
	int i;

	for (i = 0; i < te->nelems; i++) {
		if (!nft_setelem_active_next(ctx->net, te->set, te->elems[i].priv)) {
			nft_setelem_data_activate(ctx->net, te->set, te->elems[i].priv);
			nft_setelem_activate(ctx->net, te->set, te->elems[i].priv);
		}

		if (!nft_setelem_is_catchall(te->set, te->elems[i].priv))
			te->set->ndeact--;
	}
}

static int nft_del_setelem(struct nft_ctx *ctx, struct nft_set *set,
			   const struct nlattr *attr)
{
	struct nlattr *nla[NFTA_SET_ELEM_MAX + 1];
	struct nft_set_ext_tmpl tmpl;
	struct nft_set_elem elem;
	struct nft_set_ext *ext;
	struct nft_trans *trans;
	u32 flags = 0;
	int err;

	err = nla_parse_nested_deprecated(nla, NFTA_SET_ELEM_MAX, attr,
					  nft_set_elem_policy, NULL);
	if (err < 0)
		return err;

	err = nft_setelem_parse_flags(set, nla[NFTA_SET_ELEM_FLAGS], &flags);
	if (err < 0)
		return err;

	if (!nla[NFTA_SET_ELEM_KEY] && !(flags & NFT_SET_ELEM_CATCHALL))
		return -EINVAL;

	if (!nft_setelem_valid_key_end(set, nla, flags))
		return -EINVAL;

	nft_set_ext_prepare(&tmpl);

	if (flags != 0) {
		err = nft_set_ext_add(&tmpl, NFT_SET_EXT_FLAGS);
		if (err < 0)
			return err;
	}

	if (nla[NFTA_SET_ELEM_KEY]) {
		err = nft_setelem_parse_key(ctx, set, &elem.key.val,
					    nla[NFTA_SET_ELEM_KEY]);
		if (err < 0)
			return err;

		err = nft_set_ext_add_length(&tmpl, NFT_SET_EXT_KEY, set->klen);
		if (err < 0)
			goto fail_elem;
	}

	if (nla[NFTA_SET_ELEM_KEY_END]) {
		err = nft_setelem_parse_key(ctx, set, &elem.key_end.val,
					    nla[NFTA_SET_ELEM_KEY_END]);
		if (err < 0)
			goto fail_elem;

		err = nft_set_ext_add_length(&tmpl, NFT_SET_EXT_KEY_END, set->klen);
		if (err < 0)
			goto fail_elem_key_end;
	}

	err = -ENOMEM;
	elem.priv = nft_set_elem_init(set, &tmpl, elem.key.val.data,
				      elem.key_end.val.data, NULL, 0, 0,
				      GFP_KERNEL_ACCOUNT);
	if (IS_ERR(elem.priv)) {
		err = PTR_ERR(elem.priv);
		goto fail_elem_key_end;
	}

	ext = nft_set_elem_ext(set, elem.priv);
	if (flags)
		*nft_set_ext_flags(ext) = flags;

	trans = nft_trans_elem_alloc(ctx, NFT_MSG_DELSETELEM, set);
	if (trans == NULL)
		goto fail_trans;

	err = nft_setelem_deactivate(ctx->net, set, &elem, flags);
	if (err < 0)
		goto fail_ops;

	nft_setelem_data_deactivate(ctx->net, set, elem.priv);

	nft_trans_container_elem(trans)->elems[0].priv = elem.priv;
	nft_trans_commit_list_add_elem(ctx->net, trans, GFP_KERNEL);
	return 0;

fail_ops:
	kfree(trans);
fail_trans:
	kfree(elem.priv);
fail_elem_key_end:
	nft_data_release(&elem.key_end.val, NFT_DATA_VALUE);
fail_elem:
	nft_data_release(&elem.key.val, NFT_DATA_VALUE);
	return err;
}

static int nft_setelem_flush(const struct nft_ctx *ctx,
			     struct nft_set *set,
			     const struct nft_set_iter *iter,
			     struct nft_elem_priv *elem_priv)
{
	const struct nft_set_ext *ext = nft_set_elem_ext(set, elem_priv);
	struct nft_trans *trans;

	if (!nft_set_elem_active(ext, iter->genmask))
		return 0;

	trans = nft_trans_alloc_gfp(ctx, NFT_MSG_DELSETELEM,
				    struct_size_t(struct nft_trans_elem, elems, 1),
				    GFP_ATOMIC);
	if (!trans)
		return -ENOMEM;

	set->ops->flush(ctx->net, set, elem_priv);
	set->ndeact++;

	nft_setelem_data_deactivate(ctx->net, set, elem_priv);
	nft_trans_elem_set(trans) = set;
	nft_trans_container_elem(trans)->nelems = 1;
	nft_trans_container_elem(trans)->elems[0].priv = elem_priv;
	nft_trans_commit_list_add_elem(ctx->net, trans, GFP_ATOMIC);

	return 0;
}

static int __nft_set_catchall_flush(const struct nft_ctx *ctx,
				    struct nft_set *set,
				    struct nft_elem_priv *elem_priv)
{
	struct nft_trans *trans;

	trans = nft_trans_elem_alloc(ctx, NFT_MSG_DELSETELEM, set);
	if (!trans)
		return -ENOMEM;

	nft_setelem_data_deactivate(ctx->net, set, elem_priv);
	nft_trans_container_elem(trans)->elems[0].priv = elem_priv;
	nft_trans_commit_list_add_elem(ctx->net, trans, GFP_KERNEL);

	return 0;
}

static int nft_set_catchall_flush(const struct nft_ctx *ctx,
				  struct nft_set *set)
{
	u8 genmask = nft_genmask_next(ctx->net);
	struct nft_set_elem_catchall *catchall;
	struct nft_set_ext *ext;
	int ret = 0;

	list_for_each_entry_rcu(catchall, &set->catchall_list, list,
				lockdep_commit_lock_is_held(ctx->net)) {
		ext = nft_set_elem_ext(set, catchall->elem);
		if (!nft_set_elem_active(ext, genmask))
			continue;

		ret = __nft_set_catchall_flush(ctx, set, catchall->elem);
		if (ret < 0)
			break;
		nft_set_elem_change_active(ctx->net, set, ext);
	}

	return ret;
}

static int nft_set_flush(struct nft_ctx *ctx, struct nft_set *set, u8 genmask)
{
	struct nft_set_iter iter = {
		.genmask	= genmask,
		.type		= NFT_ITER_UPDATE,
		.fn		= nft_setelem_flush,
	};

	set->ops->walk(ctx, set, &iter);
	if (!iter.err)
		iter.err = nft_set_catchall_flush(ctx, set);

	return iter.err;
}

static int nf_tables_delsetelem(struct sk_buff *skb,
				const struct nfnl_info *info,
				const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_next(info->net);
	u8 family = info->nfmsg->nfgen_family;
	struct net *net = info->net;
	const struct nlattr *attr;
	struct nft_table *table;
	struct nft_set *set;
	struct nft_ctx ctx;
	int rem, err = 0;

	table = nft_table_lookup(net, nla[NFTA_SET_ELEM_LIST_TABLE], family,
				 genmask, NETLINK_CB(skb).portid);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_SET_ELEM_LIST_TABLE]);
		return PTR_ERR(table);
	}

	set = nft_set_lookup(net, table, nla[NFTA_SET_ELEM_LIST_SET], genmask);
	if (IS_ERR(set)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_SET_ELEM_LIST_SET]);
		return PTR_ERR(set);
	}

	if (nft_set_is_anonymous(set))
		return -EOPNOTSUPP;

	if (!list_empty(&set->bindings) && (set->flags & NFT_SET_CONSTANT))
		return -EBUSY;

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, NULL, nla);

	if (!nla[NFTA_SET_ELEM_LIST_ELEMENTS])
		return nft_set_flush(&ctx, set, genmask);

	nla_for_each_nested(attr, nla[NFTA_SET_ELEM_LIST_ELEMENTS], rem) {
		err = nft_del_setelem(&ctx, set, attr);
		if (err == -ENOENT &&
		    NFNL_MSG_TYPE(info->nlh->nlmsg_type) == NFT_MSG_DESTROYSETELEM)
			continue;

		if (err < 0) {
			NL_SET_BAD_ATTR(extack, attr);
			return err;
		}
	}

	return 0;
}

/*
 * Stateful objects
 */

/**
 *	nft_register_obj- register nf_tables stateful object type
 *	@obj_type: object type
 *
 *	Registers the object type for use with nf_tables. Returns zero on
 *	success or a negative errno code otherwise.
 */
int nft_register_obj(struct nft_object_type *obj_type)
{
	if (obj_type->type == NFT_OBJECT_UNSPEC)
		return -EINVAL;

	nfnl_lock(NFNL_SUBSYS_NFTABLES);
	list_add_rcu(&obj_type->list, &nf_tables_objects);
	nfnl_unlock(NFNL_SUBSYS_NFTABLES);
	return 0;
}
EXPORT_SYMBOL_GPL(nft_register_obj);

/**
 *	nft_unregister_obj - unregister nf_tables object type
 *	@obj_type: object type
 *
 * 	Unregisters the object type for use with nf_tables.
 */
void nft_unregister_obj(struct nft_object_type *obj_type)
{
	nfnl_lock(NFNL_SUBSYS_NFTABLES);
	list_del_rcu(&obj_type->list);
	nfnl_unlock(NFNL_SUBSYS_NFTABLES);
}
EXPORT_SYMBOL_GPL(nft_unregister_obj);

struct nft_object *nft_obj_lookup(const struct net *net,
				  const struct nft_table *table,
				  const struct nlattr *nla, u32 objtype,
				  u8 genmask)
{
	struct nft_object_hash_key k = { .table = table };
	char search[NFT_OBJ_MAXNAMELEN];
	struct rhlist_head *tmp, *list;
	struct nft_object *obj;

	nla_strscpy(search, nla, sizeof(search));
	k.name = search;

	WARN_ON_ONCE(!rcu_read_lock_held() &&
		     !lockdep_commit_lock_is_held(net));

	rcu_read_lock();
	list = rhltable_lookup(&nft_objname_ht, &k, nft_objname_ht_params);
	if (!list)
		goto out;

	rhl_for_each_entry_rcu(obj, tmp, list, rhlhead) {
		if (objtype == obj->ops->type->type &&
		    nft_active_genmask(obj, genmask)) {
			rcu_read_unlock();
			return obj;
		}
	}
out:
	rcu_read_unlock();
	return ERR_PTR(-ENOENT);
}
EXPORT_SYMBOL_GPL(nft_obj_lookup);

static struct nft_object *nft_obj_lookup_byhandle(const struct nft_table *table,
						  const struct nlattr *nla,
						  u32 objtype, u8 genmask)
{
	struct nft_object *obj;

	list_for_each_entry(obj, &table->objects, list) {
		if (be64_to_cpu(nla_get_be64(nla)) == obj->handle &&
		    objtype == obj->ops->type->type &&
		    nft_active_genmask(obj, genmask))
			return obj;
	}
	return ERR_PTR(-ENOENT);
}

static const struct nla_policy nft_obj_policy[NFTA_OBJ_MAX + 1] = {
	[NFTA_OBJ_TABLE]	= { .type = NLA_STRING,
				    .len = NFT_TABLE_MAXNAMELEN - 1 },
	[NFTA_OBJ_NAME]		= { .type = NLA_STRING,
				    .len = NFT_OBJ_MAXNAMELEN - 1 },
	[NFTA_OBJ_TYPE]		= { .type = NLA_U32 },
	[NFTA_OBJ_DATA]		= { .type = NLA_NESTED },
	[NFTA_OBJ_HANDLE]	= { .type = NLA_U64},
	[NFTA_OBJ_USERDATA]	= { .type = NLA_BINARY,
				    .len = NFT_USERDATA_MAXLEN },
};

static struct nft_object *nft_obj_init(const struct nft_ctx *ctx,
				       const struct nft_object_type *type,
				       const struct nlattr *attr)
{
	struct nlattr **tb;
	const struct nft_object_ops *ops;
	struct nft_object *obj;
	int err = -ENOMEM;

	tb = kmalloc_array(type->maxattr + 1, sizeof(*tb), GFP_KERNEL);
	if (!tb)
		goto err1;

	if (attr) {
		err = nla_parse_nested_deprecated(tb, type->maxattr, attr,
						  type->policy, NULL);
		if (err < 0)
			goto err2;
	} else {
		memset(tb, 0, sizeof(tb[0]) * (type->maxattr + 1));
	}

	if (type->select_ops) {
		ops = type->select_ops(ctx, (const struct nlattr * const *)tb);
		if (IS_ERR(ops)) {
			err = PTR_ERR(ops);
			goto err2;
		}
	} else {
		ops = type->ops;
	}

	err = -ENOMEM;
	obj = kzalloc(sizeof(*obj) + ops->size, GFP_KERNEL_ACCOUNT);
	if (!obj)
		goto err2;

	err = ops->init(ctx, (const struct nlattr * const *)tb, obj);
	if (err < 0)
		goto err3;

	obj->ops = ops;

	kfree(tb);
	return obj;
err3:
	kfree(obj);
err2:
	kfree(tb);
err1:
	return ERR_PTR(err);
}

static int nft_object_dump(struct sk_buff *skb, unsigned int attr,
			   struct nft_object *obj, bool reset)
{
	struct nlattr *nest;

	nest = nla_nest_start_noflag(skb, attr);
	if (!nest)
		goto nla_put_failure;
	if (obj->ops->dump(skb, obj, reset) < 0)
		goto nla_put_failure;
	nla_nest_end(skb, nest);
	return 0;

nla_put_failure:
	return -1;
}

static const struct nft_object_type *__nft_obj_type_get(u32 objtype, u8 family)
{
	const struct nft_object_type *type;

	list_for_each_entry_rcu(type, &nf_tables_objects, list) {
		if (type->family != NFPROTO_UNSPEC &&
		    type->family != family)
			continue;

		if (objtype == type->type)
			return type;
	}
	return NULL;
}

static const struct nft_object_type *
nft_obj_type_get(struct net *net, u32 objtype, u8 family)
{
	const struct nft_object_type *type;

	rcu_read_lock();
	type = __nft_obj_type_get(objtype, family);
	if (type != NULL && try_module_get(type->owner)) {
		rcu_read_unlock();
		return type;
	}
	rcu_read_unlock();

	lockdep_nfnl_nft_mutex_not_held();
#ifdef CONFIG_MODULES
	if (type == NULL) {
		if (nft_request_module(net, "nft-obj-%u", objtype) == -EAGAIN)
			return ERR_PTR(-EAGAIN);
	}
#endif
	return ERR_PTR(-ENOENT);
}

static int nf_tables_updobj(const struct nft_ctx *ctx,
			    const struct nft_object_type *type,
			    const struct nlattr *attr,
			    struct nft_object *obj)
{
	struct nft_object *newobj;
	struct nft_trans *trans;
	int err = -ENOMEM;

	/* caller must have obtained type->owner reference. */
	trans = nft_trans_alloc(ctx, NFT_MSG_NEWOBJ,
				sizeof(struct nft_trans_obj));
	if (!trans)
		goto err_trans;

	newobj = nft_obj_init(ctx, type, attr);
	if (IS_ERR(newobj)) {
		err = PTR_ERR(newobj);
		goto err_free_trans;
	}

	nft_trans_obj(trans) = obj;
	nft_trans_obj_update(trans) = true;
	nft_trans_obj_newobj(trans) = newobj;
	nft_trans_commit_list_add_tail(ctx->net, trans);

	return 0;

err_free_trans:
	kfree(trans);
err_trans:
	module_put(type->owner);
	return err;
}

static int nf_tables_newobj(struct sk_buff *skb, const struct nfnl_info *info,
			    const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_next(info->net);
	u8 family = info->nfmsg->nfgen_family;
	const struct nft_object_type *type;
	struct net *net = info->net;
	struct nft_table *table;
	struct nft_object *obj;
	struct nft_ctx ctx;
	u32 objtype;
	int err;

	if (!nla[NFTA_OBJ_TYPE] ||
	    !nla[NFTA_OBJ_NAME] ||
	    !nla[NFTA_OBJ_DATA])
		return -EINVAL;

	table = nft_table_lookup(net, nla[NFTA_OBJ_TABLE], family, genmask,
				 NETLINK_CB(skb).portid);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_OBJ_TABLE]);
		return PTR_ERR(table);
	}

	objtype = ntohl(nla_get_be32(nla[NFTA_OBJ_TYPE]));
	obj = nft_obj_lookup(net, table, nla[NFTA_OBJ_NAME], objtype, genmask);
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		if (err != -ENOENT) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_OBJ_NAME]);
			return err;
		}
	} else {
		if (info->nlh->nlmsg_flags & NLM_F_EXCL) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_OBJ_NAME]);
			return -EEXIST;
		}
		if (info->nlh->nlmsg_flags & NLM_F_REPLACE)
			return -EOPNOTSUPP;

		if (!obj->ops->update)
			return 0;

		type = nft_obj_type_get(net, objtype, family);
		if (WARN_ON_ONCE(IS_ERR(type)))
			return PTR_ERR(type);

		nft_ctx_init(&ctx, net, skb, info->nlh, family, table, NULL, nla);

		/* type->owner reference is put when transaction object is released. */
		return nf_tables_updobj(&ctx, type, nla[NFTA_OBJ_DATA], obj);
	}

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, NULL, nla);

	if (!nft_use_inc(&table->use))
		return -EMFILE;

	type = nft_obj_type_get(net, objtype, family);
	if (IS_ERR(type)) {
		err = PTR_ERR(type);
		goto err_type;
	}

	obj = nft_obj_init(&ctx, type, nla[NFTA_OBJ_DATA]);
	if (IS_ERR(obj)) {
		err = PTR_ERR(obj);
		goto err_init;
	}
	obj->key.table = table;
	obj->handle = nf_tables_alloc_handle(table);

	obj->key.name = nla_strdup(nla[NFTA_OBJ_NAME], GFP_KERNEL_ACCOUNT);
	if (!obj->key.name) {
		err = -ENOMEM;
		goto err_strdup;
	}

	if (nla[NFTA_OBJ_USERDATA]) {
		obj->udata = nla_memdup(nla[NFTA_OBJ_USERDATA], GFP_KERNEL_ACCOUNT);
		if (obj->udata == NULL)
			goto err_userdata;

		obj->udlen = nla_len(nla[NFTA_OBJ_USERDATA]);
	}

	err = nft_trans_obj_add(&ctx, NFT_MSG_NEWOBJ, obj);
	if (err < 0)
		goto err_trans;

	err = rhltable_insert(&nft_objname_ht, &obj->rhlhead,
			      nft_objname_ht_params);
	if (err < 0)
		goto err_obj_ht;

	list_add_tail_rcu(&obj->list, &table->objects);

	return 0;
err_obj_ht:
	/* queued in transaction log */
	INIT_LIST_HEAD(&obj->list);
	return err;
err_trans:
	kfree(obj->udata);
err_userdata:
	kfree(obj->key.name);
err_strdup:
	if (obj->ops->destroy)
		obj->ops->destroy(&ctx, obj);
	kfree(obj);
err_init:
	module_put(type->owner);
err_type:
	nft_use_dec_restore(&table->use);

	return err;
}

static int nf_tables_fill_obj_info(struct sk_buff *skb, struct net *net,
				   u32 portid, u32 seq, int event, u32 flags,
				   int family, const struct nft_table *table,
				   struct nft_object *obj, bool reset)
{
	struct nlmsghdr *nlh;

	nlh = nfnl_msg_put(skb, portid, seq,
			   nfnl_msg_type(NFNL_SUBSYS_NFTABLES, event),
			   flags, family, NFNETLINK_V0, nft_base_seq(net));
	if (!nlh)
		goto nla_put_failure;

	if (nla_put_string(skb, NFTA_OBJ_TABLE, table->name) ||
	    nla_put_string(skb, NFTA_OBJ_NAME, obj->key.name) ||
	    nla_put_be32(skb, NFTA_OBJ_TYPE, htonl(obj->ops->type->type)) ||
	    nla_put_be64(skb, NFTA_OBJ_HANDLE, cpu_to_be64(obj->handle),
			 NFTA_OBJ_PAD))
		goto nla_put_failure;

	if (event == NFT_MSG_DELOBJ ||
	    event == NFT_MSG_DESTROYOBJ) {
		nlmsg_end(skb, nlh);
		return 0;
	}

	if (nla_put_be32(skb, NFTA_OBJ_USE, htonl(obj->use)) ||
	    nft_object_dump(skb, NFTA_OBJ_DATA, obj, reset))
		goto nla_put_failure;

	if (obj->udata &&
	    nla_put(skb, NFTA_OBJ_USERDATA, obj->udlen, obj->udata))
		goto nla_put_failure;

	nlmsg_end(skb, nlh);
	return 0;

nla_put_failure:
	nlmsg_trim(skb, nlh);
	return -1;
}

static void audit_log_obj_reset(const struct nft_table *table,
				unsigned int base_seq, unsigned int nentries)
{
	char *buf = kasprintf(GFP_ATOMIC, "%s:%u", table->name, base_seq);

	audit_log_nfcfg(buf, table->family, nentries,
			AUDIT_NFT_OP_OBJ_RESET, GFP_ATOMIC);
	kfree(buf);
}

struct nft_obj_dump_ctx {
	unsigned int	s_idx;
	char		*table;
	u32		type;
	bool		reset;
};

static int nf_tables_dump_obj(struct sk_buff *skb, struct netlink_callback *cb)
{
	const struct nfgenmsg *nfmsg = nlmsg_data(cb->nlh);
	struct nft_obj_dump_ctx *ctx = (void *)cb->ctx;
	struct net *net = sock_net(skb->sk);
	int family = nfmsg->nfgen_family;
	struct nftables_pernet *nft_net;
	const struct nft_table *table;
	unsigned int entries = 0;
	struct nft_object *obj;
	unsigned int idx = 0;
	int rc = 0;

	rcu_read_lock();
	nft_net = nft_pernet(net);
	cb->seq = READ_ONCE(nft_net->base_seq);

	list_for_each_entry_rcu(table, &nft_net->tables, list) {
		if (family != NFPROTO_UNSPEC && family != table->family)
			continue;

		entries = 0;
		list_for_each_entry_rcu(obj, &table->objects, list) {
			if (!nft_is_active(net, obj))
				goto cont;
			if (idx < ctx->s_idx)
				goto cont;
			if (ctx->table && strcmp(ctx->table, table->name))
				goto cont;
			if (ctx->type != NFT_OBJECT_UNSPEC &&
			    obj->ops->type->type != ctx->type)
				goto cont;

			rc = nf_tables_fill_obj_info(skb, net,
						     NETLINK_CB(cb->skb).portid,
						     cb->nlh->nlmsg_seq,
						     NFT_MSG_NEWOBJ,
						     NLM_F_MULTI | NLM_F_APPEND,
						     table->family, table,
						     obj, ctx->reset);
			if (rc < 0)
				break;

			entries++;
			nl_dump_check_consistent(cb, nlmsg_hdr(skb));
cont:
			idx++;
		}
		if (ctx->reset && entries)
			audit_log_obj_reset(table, nft_net->base_seq, entries);
		if (rc < 0)
			break;
	}
	rcu_read_unlock();

	ctx->s_idx = idx;
	return skb->len;
}

static int nf_tables_dumpreset_obj(struct sk_buff *skb,
				   struct netlink_callback *cb)
{
	struct nftables_pernet *nft_net = nft_pernet(sock_net(skb->sk));
	int ret;

	mutex_lock(&nft_net->commit_mutex);
	ret = nf_tables_dump_obj(skb, cb);
	mutex_unlock(&nft_net->commit_mutex);

	return ret;
}

static int nf_tables_dump_obj_start(struct netlink_callback *cb)
{
	struct nft_obj_dump_ctx *ctx = (void *)cb->ctx;
	const struct nlattr * const *nla = cb->data;

	BUILD_BUG_ON(sizeof(*ctx) > sizeof(cb->ctx));

	if (nla[NFTA_OBJ_TABLE]) {
		ctx->table = nla_strdup(nla[NFTA_OBJ_TABLE], GFP_ATOMIC);
		if (!ctx->table)
			return -ENOMEM;
	}

	if (nla[NFTA_OBJ_TYPE])
		ctx->type = ntohl(nla_get_be32(nla[NFTA_OBJ_TYPE]));

	return 0;
}

static int nf_tables_dumpreset_obj_start(struct netlink_callback *cb)
{
	struct nft_obj_dump_ctx *ctx = (void *)cb->ctx;

	ctx->reset = true;

	return nf_tables_dump_obj_start(cb);
}

static int nf_tables_dump_obj_done(struct netlink_callback *cb)
{
	struct nft_obj_dump_ctx *ctx = (void *)cb->ctx;

	kfree(ctx->table);

	return 0;
}

/* Caller must hold rcu read lock or transaction mutex */
static struct sk_buff *
nf_tables_getobj_single(u32 portid, const struct nfnl_info *info,
			const struct nlattr * const nla[], bool reset)
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_cur(info->net);
	u8 family = info->nfmsg->nfgen_family;
	const struct nft_table *table;
	struct net *net = info->net;
	struct nft_object *obj;
	struct sk_buff *skb2;
	u32 objtype;
	int err;

	if (!nla[NFTA_OBJ_NAME] ||
	    !nla[NFTA_OBJ_TYPE])
		return ERR_PTR(-EINVAL);

	table = nft_table_lookup(net, nla[NFTA_OBJ_TABLE], family, genmask, 0);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_OBJ_TABLE]);
		return ERR_CAST(table);
	}

	objtype = ntohl(nla_get_be32(nla[NFTA_OBJ_TYPE]));
	obj = nft_obj_lookup(net, table, nla[NFTA_OBJ_NAME], objtype, genmask);
	if (IS_ERR(obj)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_OBJ_NAME]);
		return ERR_CAST(obj);
	}

	skb2 = alloc_skb(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb2)
		return ERR_PTR(-ENOMEM);

	err = nf_tables_fill_obj_info(skb2, net, portid,
				      info->nlh->nlmsg_seq, NFT_MSG_NEWOBJ, 0,
				      family, table, obj, reset);
	if (err < 0) {
		kfree_skb(skb2);
		return ERR_PTR(err);
	}

	return skb2;
}

static int nf_tables_getobj(struct sk_buff *skb, const struct nfnl_info *info,
			    const struct nlattr * const nla[])
{
	u32 portid = NETLINK_CB(skb).portid;
	struct sk_buff *skb2;

	if (info->nlh->nlmsg_flags & NLM_F_DUMP) {
		struct netlink_dump_control c = {
			.start = nf_tables_dump_obj_start,
			.dump = nf_tables_dump_obj,
			.done = nf_tables_dump_obj_done,
			.module = THIS_MODULE,
			.data = (void *)nla,
		};

		return nft_netlink_dump_start_rcu(info->sk, skb, info->nlh, &c);
	}

	skb2 = nf_tables_getobj_single(portid, info, nla, false);
	if (IS_ERR(skb2))
		return PTR_ERR(skb2);

	return nfnetlink_unicast(skb2, info->net, portid);
}

static int nf_tables_getobj_reset(struct sk_buff *skb,
				  const struct nfnl_info *info,
				  const struct nlattr * const nla[])
{
	struct nftables_pernet *nft_net = nft_pernet(info->net);
	u32 portid = NETLINK_CB(skb).portid;
	struct net *net = info->net;
	struct sk_buff *skb2;
	char *buf;

	if (info->nlh->nlmsg_flags & NLM_F_DUMP) {
		struct netlink_dump_control c = {
			.start = nf_tables_dumpreset_obj_start,
			.dump = nf_tables_dumpreset_obj,
			.done = nf_tables_dump_obj_done,
			.module = THIS_MODULE,
			.data = (void *)nla,
		};

		return nft_netlink_dump_start_rcu(info->sk, skb, info->nlh, &c);
	}

	if (!try_module_get(THIS_MODULE))
		return -EINVAL;
	rcu_read_unlock();
	mutex_lock(&nft_net->commit_mutex);
	skb2 = nf_tables_getobj_single(portid, info, nla, true);
	mutex_unlock(&nft_net->commit_mutex);
	rcu_read_lock();
	module_put(THIS_MODULE);

	if (IS_ERR(skb2))
		return PTR_ERR(skb2);

	buf = kasprintf(GFP_ATOMIC, "%.*s:%u",
			nla_len(nla[NFTA_OBJ_TABLE]),
			(char *)nla_data(nla[NFTA_OBJ_TABLE]),
			nft_net->base_seq);
	audit_log_nfcfg(buf, info->nfmsg->nfgen_family, 1,
			AUDIT_NFT_OP_OBJ_RESET, GFP_ATOMIC);
	kfree(buf);

	return nfnetlink_unicast(skb2, net, portid);
}

static void nft_obj_destroy(const struct nft_ctx *ctx, struct nft_object *obj)
{
	if (obj->ops->destroy)
		obj->ops->destroy(ctx, obj);

	module_put(obj->ops->type->owner);
	kfree(obj->key.name);
	kfree(obj->udata);
	kfree(obj);
}

static int nf_tables_delobj(struct sk_buff *skb, const struct nfnl_info *info,
			    const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_next(info->net);
	u8 family = info->nfmsg->nfgen_family;
	struct net *net = info->net;
	const struct nlattr *attr;
	struct nft_table *table;
	struct nft_object *obj;
	struct nft_ctx ctx;
	u32 objtype;

	if (!nla[NFTA_OBJ_TYPE] ||
	    (!nla[NFTA_OBJ_NAME] && !nla[NFTA_OBJ_HANDLE]))
		return -EINVAL;

	table = nft_table_lookup(net, nla[NFTA_OBJ_TABLE], family, genmask,
				 NETLINK_CB(skb).portid);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_OBJ_TABLE]);
		return PTR_ERR(table);
	}

	objtype = ntohl(nla_get_be32(nla[NFTA_OBJ_TYPE]));
	if (nla[NFTA_OBJ_HANDLE]) {
		attr = nla[NFTA_OBJ_HANDLE];
		obj = nft_obj_lookup_byhandle(table, attr, objtype, genmask);
	} else {
		attr = nla[NFTA_OBJ_NAME];
		obj = nft_obj_lookup(net, table, attr, objtype, genmask);
	}

	if (IS_ERR(obj)) {
		if (PTR_ERR(obj) == -ENOENT &&
		    NFNL_MSG_TYPE(info->nlh->nlmsg_type) == NFT_MSG_DESTROYOBJ)
			return 0;

		NL_SET_BAD_ATTR(extack, attr);
		return PTR_ERR(obj);
	}
	if (obj->use > 0) {
		NL_SET_BAD_ATTR(extack, attr);
		return -EBUSY;
	}

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, NULL, nla);

	return nft_delobj(&ctx, obj);
}

static void
__nft_obj_notify(struct net *net, const struct nft_table *table,
		 struct nft_object *obj, u32 portid, u32 seq, int event,
		 u16 flags, int family, int report, gfp_t gfp)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	struct sk_buff *skb;
	int err;

	if (!report &&
	    !nfnetlink_has_listeners(net, NFNLGRP_NFTABLES))
		return;

	skb = nlmsg_new(NLMSG_GOODSIZE, gfp);
	if (skb == NULL)
		goto err;

	err = nf_tables_fill_obj_info(skb, net, portid, seq, event,
				      flags & (NLM_F_CREATE | NLM_F_EXCL),
				      family, table, obj, false);
	if (err < 0) {
		kfree_skb(skb);
		goto err;
	}

	nft_notify_enqueue(skb, report, &nft_net->notify_list);
	return;
err:
	nfnetlink_set_err(net, portid, NFNLGRP_NFTABLES, -ENOBUFS);
}

void nft_obj_notify(struct net *net, const struct nft_table *table,
		    struct nft_object *obj, u32 portid, u32 seq, int event,
		    u16 flags, int family, int report, gfp_t gfp)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	char *buf = kasprintf(gfp, "%s:%u",
			      table->name, nft_net->base_seq);

	audit_log_nfcfg(buf,
			family,
			obj->handle,
			event == NFT_MSG_NEWOBJ ?
				 AUDIT_NFT_OP_OBJ_REGISTER :
				 AUDIT_NFT_OP_OBJ_UNREGISTER,
			gfp);
	kfree(buf);

	__nft_obj_notify(net, table, obj, portid, seq, event,
			 flags, family, report, gfp);
}
EXPORT_SYMBOL_GPL(nft_obj_notify);

static void nf_tables_obj_notify(const struct nft_ctx *ctx,
				 struct nft_object *obj, int event)
{
	__nft_obj_notify(ctx->net, ctx->table, obj, ctx->portid,
			 ctx->seq, event, ctx->flags, ctx->family,
			 ctx->report, GFP_KERNEL);
}

/*
 * Flow tables
 */
void nft_register_flowtable_type(struct nf_flowtable_type *type)
{
	nfnl_lock(NFNL_SUBSYS_NFTABLES);
	list_add_tail_rcu(&type->list, &nf_tables_flowtables);
	nfnl_unlock(NFNL_SUBSYS_NFTABLES);
}
EXPORT_SYMBOL_GPL(nft_register_flowtable_type);

void nft_unregister_flowtable_type(struct nf_flowtable_type *type)
{
	nfnl_lock(NFNL_SUBSYS_NFTABLES);
	list_del_rcu(&type->list);
	nfnl_unlock(NFNL_SUBSYS_NFTABLES);
}
EXPORT_SYMBOL_GPL(nft_unregister_flowtable_type);

static const struct nla_policy nft_flowtable_policy[NFTA_FLOWTABLE_MAX + 1] = {
	[NFTA_FLOWTABLE_TABLE]		= { .type = NLA_STRING,
					    .len = NFT_NAME_MAXLEN - 1 },
	[NFTA_FLOWTABLE_NAME]		= { .type = NLA_STRING,
					    .len = NFT_NAME_MAXLEN - 1 },
	[NFTA_FLOWTABLE_HOOK]		= { .type = NLA_NESTED },
	[NFTA_FLOWTABLE_HANDLE]		= { .type = NLA_U64 },
	[NFTA_FLOWTABLE_FLAGS]		= { .type = NLA_U32 },
};

struct nft_flowtable *nft_flowtable_lookup(const struct net *net,
					   const struct nft_table *table,
					   const struct nlattr *nla, u8 genmask)
{
	struct nft_flowtable *flowtable;

	list_for_each_entry_rcu(flowtable, &table->flowtables, list,
				lockdep_commit_lock_is_held(net)) {
		if (!nla_strcmp(nla, flowtable->name) &&
		    nft_active_genmask(flowtable, genmask))
			return flowtable;
	}
	return ERR_PTR(-ENOENT);
}
EXPORT_SYMBOL_GPL(nft_flowtable_lookup);

void nf_tables_deactivate_flowtable(const struct nft_ctx *ctx,
				    struct nft_flowtable *flowtable,
				    enum nft_trans_phase phase)
{
	switch (phase) {
	case NFT_TRANS_PREPARE_ERROR:
	case NFT_TRANS_PREPARE:
	case NFT_TRANS_ABORT:
	case NFT_TRANS_RELEASE:
		nft_use_dec(&flowtable->use);
		fallthrough;
	default:
		return;
	}
}
EXPORT_SYMBOL_GPL(nf_tables_deactivate_flowtable);

static struct nft_flowtable *
nft_flowtable_lookup_byhandle(const struct nft_table *table,
			      const struct nlattr *nla, u8 genmask)
{
       struct nft_flowtable *flowtable;

       list_for_each_entry(flowtable, &table->flowtables, list) {
               if (be64_to_cpu(nla_get_be64(nla)) == flowtable->handle &&
                   nft_active_genmask(flowtable, genmask))
                       return flowtable;
       }
       return ERR_PTR(-ENOENT);
}

struct nft_flowtable_hook {
	u32			num;
	int			priority;
	struct list_head	list;
};

static const struct nla_policy nft_flowtable_hook_policy[NFTA_FLOWTABLE_HOOK_MAX + 1] = {
	[NFTA_FLOWTABLE_HOOK_NUM]	= { .type = NLA_U32 },
	[NFTA_FLOWTABLE_HOOK_PRIORITY]	= { .type = NLA_U32 },
	[NFTA_FLOWTABLE_HOOK_DEVS]	= { .type = NLA_NESTED },
};

static int nft_flowtable_parse_hook(const struct nft_ctx *ctx,
				    const struct nlattr * const nla[],
				    struct nft_flowtable_hook *flowtable_hook,
				    struct nft_flowtable *flowtable,
				    struct netlink_ext_ack *extack, bool add)
{
	struct nlattr *tb[NFTA_FLOWTABLE_HOOK_MAX + 1];
	struct nf_hook_ops *ops;
	struct nft_hook *hook;
	int hooknum, priority;
	int err;

	INIT_LIST_HEAD(&flowtable_hook->list);

	err = nla_parse_nested_deprecated(tb, NFTA_FLOWTABLE_HOOK_MAX,
					  nla[NFTA_FLOWTABLE_HOOK],
					  nft_flowtable_hook_policy, NULL);
	if (err < 0)
		return err;

	if (add) {
		if (!tb[NFTA_FLOWTABLE_HOOK_NUM] ||
		    !tb[NFTA_FLOWTABLE_HOOK_PRIORITY]) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_FLOWTABLE_NAME]);
			return -ENOENT;
		}

		hooknum = ntohl(nla_get_be32(tb[NFTA_FLOWTABLE_HOOK_NUM]));
		if (hooknum != NF_NETDEV_INGRESS)
			return -EOPNOTSUPP;

		priority = ntohl(nla_get_be32(tb[NFTA_FLOWTABLE_HOOK_PRIORITY]));

		flowtable_hook->priority	= priority;
		flowtable_hook->num		= hooknum;
	} else {
		if (tb[NFTA_FLOWTABLE_HOOK_NUM]) {
			hooknum = ntohl(nla_get_be32(tb[NFTA_FLOWTABLE_HOOK_NUM]));
			if (hooknum != flowtable->hooknum)
				return -EOPNOTSUPP;
		}

		if (tb[NFTA_FLOWTABLE_HOOK_PRIORITY]) {
			priority = ntohl(nla_get_be32(tb[NFTA_FLOWTABLE_HOOK_PRIORITY]));
			if (priority != flowtable->data.priority)
				return -EOPNOTSUPP;
		}

		flowtable_hook->priority	= flowtable->data.priority;
		flowtable_hook->num		= flowtable->hooknum;
	}

	if (tb[NFTA_FLOWTABLE_HOOK_DEVS]) {
		err = nf_tables_parse_netdev_hooks(ctx->net,
						   tb[NFTA_FLOWTABLE_HOOK_DEVS],
						   &flowtable_hook->list,
						   extack);
		if (err < 0)
			return err;
	}

	list_for_each_entry(hook, &flowtable_hook->list, list) {
		list_for_each_entry(ops, &hook->ops_list, list) {
			ops->pf			= NFPROTO_NETDEV;
			ops->hooknum		= flowtable_hook->num;
			ops->priority		= flowtable_hook->priority;
			ops->priv		= &flowtable->data;
			ops->hook		= flowtable->data.type->hook;
			ops->hook_ops_type	= NF_HOOK_OP_NFT_FT;
		}
	}

	return err;
}

/* call under rcu_read_lock */
static const struct nf_flowtable_type *__nft_flowtable_type_get(u8 family)
{
	const struct nf_flowtable_type *type;

	list_for_each_entry_rcu(type, &nf_tables_flowtables, list) {
		if (family == type->family)
			return type;
	}
	return NULL;
}

static const struct nf_flowtable_type *
nft_flowtable_type_get(struct net *net, u8 family)
{
	const struct nf_flowtable_type *type;

	rcu_read_lock();
	type = __nft_flowtable_type_get(family);
	if (type != NULL && try_module_get(type->owner)) {
		rcu_read_unlock();
		return type;
	}
	rcu_read_unlock();

	lockdep_nfnl_nft_mutex_not_held();
#ifdef CONFIG_MODULES
	if (type == NULL) {
		if (nft_request_module(net, "nf-flowtable-%u", family) == -EAGAIN)
			return ERR_PTR(-EAGAIN);
	}
#endif
	return ERR_PTR(-ENOENT);
}

/* Only called from error and netdev event paths. */
static void nft_unregister_flowtable_ops(struct net *net,
					 struct nft_flowtable *flowtable,
					 struct nf_hook_ops *ops)
{
	nf_unregister_net_hook(net, ops);
	flowtable->data.type->setup(&flowtable->data, ops->dev,
				    FLOW_BLOCK_UNBIND);
}

static void __nft_unregister_flowtable_net_hooks(struct net *net,
						 struct nft_flowtable *flowtable,
						 struct list_head *hook_list,
					         bool release_netdev)
{
	struct nft_hook *hook, *next;
	struct nf_hook_ops *ops;

	list_for_each_entry_safe(hook, next, hook_list, list) {
		list_for_each_entry(ops, &hook->ops_list, list)
			nft_unregister_flowtable_ops(net, flowtable, ops);
		if (release_netdev) {
			list_del(&hook->list);
			nft_netdev_hook_free_rcu(hook);
		}
	}
}

static void nft_unregister_flowtable_net_hooks(struct net *net,
					       struct nft_flowtable *flowtable,
					       struct list_head *hook_list)
{
	__nft_unregister_flowtable_net_hooks(net, flowtable, hook_list, false);
}

static int nft_register_flowtable_ops(struct net *net,
				      struct nft_flowtable *flowtable,
				      struct nf_hook_ops *ops)
{
	int err;

	err = flowtable->data.type->setup(&flowtable->data,
					  ops->dev, FLOW_BLOCK_BIND);
	if (err < 0)
		return err;

	err = nf_register_net_hook(net, ops);
	if (!err)
		return 0;

	flowtable->data.type->setup(&flowtable->data,
				    ops->dev, FLOW_BLOCK_UNBIND);
	return err;
}

static int nft_register_flowtable_net_hooks(struct net *net,
					    struct nft_table *table,
					    struct list_head *hook_list,
					    struct nft_flowtable *flowtable)
{
	struct nft_hook *hook, *next;
	struct nft_flowtable *ft;
	struct nf_hook_ops *ops;
	int err, i = 0;

	list_for_each_entry(hook, hook_list, list) {
		list_for_each_entry(ft, &table->flowtables, list) {
			if (!nft_is_active_next(net, ft))
				continue;

			if (nft_hook_list_find(&ft->hook_list, hook)) {
				err = -EEXIST;
				goto err_unregister_net_hooks;
			}
		}

		list_for_each_entry(ops, &hook->ops_list, list) {
			err = nft_register_flowtable_ops(net, flowtable, ops);
			if (err < 0)
				goto err_unregister_net_hooks;

			i++;
		}
	}

	return 0;

err_unregister_net_hooks:
	list_for_each_entry_safe(hook, next, hook_list, list) {
		list_for_each_entry(ops, &hook->ops_list, list) {
			if (i-- <= 0)
				break;

			nft_unregister_flowtable_ops(net, flowtable, ops);
		}
		list_del_rcu(&hook->list);
		nft_netdev_hook_free_rcu(hook);
	}

	return err;
}

static void nft_hooks_destroy(struct list_head *hook_list)
{
	struct nft_hook *hook, *next;

	list_for_each_entry_safe(hook, next, hook_list, list) {
		list_del_rcu(&hook->list);
		nft_netdev_hook_free_rcu(hook);
	}
}

static int nft_flowtable_update(struct nft_ctx *ctx, const struct nlmsghdr *nlh,
				struct nft_flowtable *flowtable,
				struct netlink_ext_ack *extack)
{
	const struct nlattr * const *nla = ctx->nla;
	struct nft_flowtable_hook flowtable_hook;
	struct nft_hook *hook, *next;
	struct nf_hook_ops *ops;
	struct nft_trans *trans;
	bool unregister = false;
	u32 flags;
	int err;

	err = nft_flowtable_parse_hook(ctx, nla, &flowtable_hook, flowtable,
				       extack, false);
	if (err < 0)
		return err;

	list_for_each_entry_safe(hook, next, &flowtable_hook.list, list) {
		if (nft_hook_list_find(&flowtable->hook_list, hook)) {
			list_del(&hook->list);
			nft_netdev_hook_free(hook);
		}
	}

	if (nla[NFTA_FLOWTABLE_FLAGS]) {
		flags = ntohl(nla_get_be32(nla[NFTA_FLOWTABLE_FLAGS]));
		if (flags & ~NFT_FLOWTABLE_MASK) {
			err = -EOPNOTSUPP;
			goto err_flowtable_update_hook;
		}
		if ((flowtable->data.flags & NFT_FLOWTABLE_HW_OFFLOAD) ^
		    (flags & NFT_FLOWTABLE_HW_OFFLOAD)) {
			err = -EOPNOTSUPP;
			goto err_flowtable_update_hook;
		}
	} else {
		flags = flowtable->data.flags;
	}

	err = nft_register_flowtable_net_hooks(ctx->net, ctx->table,
					       &flowtable_hook.list, flowtable);
	if (err < 0)
		goto err_flowtable_update_hook;

	trans = nft_trans_alloc(ctx, NFT_MSG_NEWFLOWTABLE,
				sizeof(struct nft_trans_flowtable));
	if (!trans) {
		unregister = true;
		err = -ENOMEM;
		goto err_flowtable_update_hook;
	}

	nft_trans_flowtable_flags(trans) = flags;
	nft_trans_flowtable(trans) = flowtable;
	nft_trans_flowtable_update(trans) = true;
	INIT_LIST_HEAD(&nft_trans_flowtable_hooks(trans));
	list_splice(&flowtable_hook.list, &nft_trans_flowtable_hooks(trans));

	nft_trans_commit_list_add_tail(ctx->net, trans);

	return 0;

err_flowtable_update_hook:
	list_for_each_entry_safe(hook, next, &flowtable_hook.list, list) {
		if (unregister) {
			list_for_each_entry(ops, &hook->ops_list, list)
				nft_unregister_flowtable_ops(ctx->net,
							     flowtable, ops);
		}
		list_del_rcu(&hook->list);
		nft_netdev_hook_free_rcu(hook);
	}

	return err;

}

static int nf_tables_newflowtable(struct sk_buff *skb,
				  const struct nfnl_info *info,
				  const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	struct nft_flowtable_hook flowtable_hook;
	u8 genmask = nft_genmask_next(info->net);
	u8 family = info->nfmsg->nfgen_family;
	const struct nf_flowtable_type *type;
	struct nft_flowtable *flowtable;
	struct net *net = info->net;
	struct nft_table *table;
	struct nft_trans *trans;
	struct nft_ctx ctx;
	int err;

	if (!nla[NFTA_FLOWTABLE_TABLE] ||
	    !nla[NFTA_FLOWTABLE_NAME] ||
	    !nla[NFTA_FLOWTABLE_HOOK])
		return -EINVAL;

	table = nft_table_lookup(net, nla[NFTA_FLOWTABLE_TABLE], family,
				 genmask, NETLINK_CB(skb).portid);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_FLOWTABLE_TABLE]);
		return PTR_ERR(table);
	}

	flowtable = nft_flowtable_lookup(net, table, nla[NFTA_FLOWTABLE_NAME],
					 genmask);
	if (IS_ERR(flowtable)) {
		err = PTR_ERR(flowtable);
		if (err != -ENOENT) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_FLOWTABLE_NAME]);
			return err;
		}
	} else {
		if (info->nlh->nlmsg_flags & NLM_F_EXCL) {
			NL_SET_BAD_ATTR(extack, nla[NFTA_FLOWTABLE_NAME]);
			return -EEXIST;
		}

		nft_ctx_init(&ctx, net, skb, info->nlh, family, table, NULL, nla);

		return nft_flowtable_update(&ctx, info->nlh, flowtable, extack);
	}

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, NULL, nla);

	if (!nft_use_inc(&table->use))
		return -EMFILE;

	flowtable = kzalloc(sizeof(*flowtable), GFP_KERNEL_ACCOUNT);
	if (!flowtable) {
		err = -ENOMEM;
		goto flowtable_alloc;
	}

	flowtable->table = table;
	flowtable->handle = nf_tables_alloc_handle(table);
	INIT_LIST_HEAD(&flowtable->hook_list);

	flowtable->name = nla_strdup(nla[NFTA_FLOWTABLE_NAME], GFP_KERNEL_ACCOUNT);
	if (!flowtable->name) {
		err = -ENOMEM;
		goto err1;
	}

	type = nft_flowtable_type_get(net, family);
	if (IS_ERR(type)) {
		err = PTR_ERR(type);
		goto err2;
	}

	if (nla[NFTA_FLOWTABLE_FLAGS]) {
		flowtable->data.flags =
			ntohl(nla_get_be32(nla[NFTA_FLOWTABLE_FLAGS]));
		if (flowtable->data.flags & ~NFT_FLOWTABLE_MASK) {
			err = -EOPNOTSUPP;
			goto err3;
		}
	}

	write_pnet(&flowtable->data.net, net);
	flowtable->data.type = type;
	err = type->init(&flowtable->data);
	if (err < 0)
		goto err3;

	err = nft_flowtable_parse_hook(&ctx, nla, &flowtable_hook, flowtable,
				       extack, true);
	if (err < 0)
		goto err_flowtable_parse_hooks;

	list_splice(&flowtable_hook.list, &flowtable->hook_list);
	flowtable->data.priority = flowtable_hook.priority;
	flowtable->hooknum = flowtable_hook.num;

	trans = nft_trans_flowtable_add(&ctx, NFT_MSG_NEWFLOWTABLE, flowtable);
	if (IS_ERR(trans)) {
		err = PTR_ERR(trans);
		goto err_flowtable_trans;
	}

	/* This must be LAST to ensure no packets are walking over this flowtable. */
	err = nft_register_flowtable_net_hooks(ctx.net, table,
					       &flowtable->hook_list,
					       flowtable);
	if (err < 0)
		goto err_flowtable_hooks;

	list_add_tail_rcu(&flowtable->list, &table->flowtables);

	return 0;

err_flowtable_hooks:
	nft_trans_destroy(trans);
err_flowtable_trans:
	nft_hooks_destroy(&flowtable->hook_list);
err_flowtable_parse_hooks:
	flowtable->data.type->free(&flowtable->data);
err3:
	module_put(type->owner);
err2:
	kfree(flowtable->name);
err1:
	kfree(flowtable);
flowtable_alloc:
	nft_use_dec_restore(&table->use);

	return err;
}

static void nft_flowtable_hook_release(struct nft_flowtable_hook *flowtable_hook)
{
	struct nft_hook *this, *next;

	list_for_each_entry_safe(this, next, &flowtable_hook->list, list) {
		list_del(&this->list);
		nft_netdev_hook_free(this);
	}
}

static int nft_delflowtable_hook(struct nft_ctx *ctx,
				 struct nft_flowtable *flowtable,
				 struct netlink_ext_ack *extack)
{
	const struct nlattr * const *nla = ctx->nla;
	struct nft_flowtable_hook flowtable_hook;
	LIST_HEAD(flowtable_del_list);
	struct nft_hook *this, *hook;
	struct nft_trans *trans;
	int err;

	err = nft_flowtable_parse_hook(ctx, nla, &flowtable_hook, flowtable,
				       extack, false);
	if (err < 0)
		return err;

	list_for_each_entry(this, &flowtable_hook.list, list) {
		hook = nft_hook_list_find(&flowtable->hook_list, this);
		if (!hook) {
			err = -ENOENT;
			goto err_flowtable_del_hook;
		}
		list_move(&hook->list, &flowtable_del_list);
	}

	trans = nft_trans_alloc(ctx, NFT_MSG_DELFLOWTABLE,
				sizeof(struct nft_trans_flowtable));
	if (!trans) {
		err = -ENOMEM;
		goto err_flowtable_del_hook;
	}

	nft_trans_flowtable(trans) = flowtable;
	nft_trans_flowtable_update(trans) = true;
	INIT_LIST_HEAD(&nft_trans_flowtable_hooks(trans));
	list_splice(&flowtable_del_list, &nft_trans_flowtable_hooks(trans));
	nft_flowtable_hook_release(&flowtable_hook);

	nft_trans_commit_list_add_tail(ctx->net, trans);

	return 0;

err_flowtable_del_hook:
	list_splice(&flowtable_del_list, &flowtable->hook_list);
	nft_flowtable_hook_release(&flowtable_hook);

	return err;
}

static int nf_tables_delflowtable(struct sk_buff *skb,
				  const struct nfnl_info *info,
				  const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_next(info->net);
	u8 family = info->nfmsg->nfgen_family;
	struct nft_flowtable *flowtable;
	struct net *net = info->net;
	const struct nlattr *attr;
	struct nft_table *table;
	struct nft_ctx ctx;

	if (!nla[NFTA_FLOWTABLE_TABLE] ||
	    (!nla[NFTA_FLOWTABLE_NAME] &&
	     !nla[NFTA_FLOWTABLE_HANDLE]))
		return -EINVAL;

	table = nft_table_lookup(net, nla[NFTA_FLOWTABLE_TABLE], family,
				 genmask, NETLINK_CB(skb).portid);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_FLOWTABLE_TABLE]);
		return PTR_ERR(table);
	}

	if (nla[NFTA_FLOWTABLE_HANDLE]) {
		attr = nla[NFTA_FLOWTABLE_HANDLE];
		flowtable = nft_flowtable_lookup_byhandle(table, attr, genmask);
	} else {
		attr = nla[NFTA_FLOWTABLE_NAME];
		flowtable = nft_flowtable_lookup(net, table, attr, genmask);
	}

	if (IS_ERR(flowtable)) {
		if (PTR_ERR(flowtable) == -ENOENT &&
		    NFNL_MSG_TYPE(info->nlh->nlmsg_type) == NFT_MSG_DESTROYFLOWTABLE)
			return 0;

		NL_SET_BAD_ATTR(extack, attr);
		return PTR_ERR(flowtable);
	}

	nft_ctx_init(&ctx, net, skb, info->nlh, family, table, NULL, nla);

	if (nla[NFTA_FLOWTABLE_HOOK])
		return nft_delflowtable_hook(&ctx, flowtable, extack);

	if (flowtable->use > 0) {
		NL_SET_BAD_ATTR(extack, attr);
		return -EBUSY;
	}

	return nft_delflowtable(&ctx, flowtable);
}

static int nf_tables_fill_flowtable_info(struct sk_buff *skb, struct net *net,
					 u32 portid, u32 seq, int event,
					 u32 flags, int family,
					 struct nft_flowtable *flowtable,
					 struct list_head *hook_list)
{
	struct nlattr *nest, *nest_devs;
	struct nft_hook *hook;
	struct nlmsghdr *nlh;

	nlh = nfnl_msg_put(skb, portid, seq,
			   nfnl_msg_type(NFNL_SUBSYS_NFTABLES, event),
			   flags, family, NFNETLINK_V0, nft_base_seq(net));
	if (!nlh)
		goto nla_put_failure;

	if (nla_put_string(skb, NFTA_FLOWTABLE_TABLE, flowtable->table->name) ||
	    nla_put_string(skb, NFTA_FLOWTABLE_NAME, flowtable->name) ||
	    nla_put_be64(skb, NFTA_FLOWTABLE_HANDLE, cpu_to_be64(flowtable->handle),
			 NFTA_FLOWTABLE_PAD))
		goto nla_put_failure;

	if (!hook_list &&
	    (event == NFT_MSG_DELFLOWTABLE ||
	     event == NFT_MSG_DESTROYFLOWTABLE)) {
		nlmsg_end(skb, nlh);
		return 0;
	}

	if (nla_put_be32(skb, NFTA_FLOWTABLE_USE, htonl(flowtable->use)) ||
	    nla_put_be32(skb, NFTA_FLOWTABLE_FLAGS, htonl(flowtable->data.flags)))
		goto nla_put_failure;

	nest = nla_nest_start_noflag(skb, NFTA_FLOWTABLE_HOOK);
	if (!nest)
		goto nla_put_failure;
	if (nla_put_be32(skb, NFTA_FLOWTABLE_HOOK_NUM, htonl(flowtable->hooknum)) ||
	    nla_put_be32(skb, NFTA_FLOWTABLE_HOOK_PRIORITY, htonl(flowtable->data.priority)))
		goto nla_put_failure;

	nest_devs = nla_nest_start_noflag(skb, NFTA_FLOWTABLE_HOOK_DEVS);
	if (!nest_devs)
		goto nla_put_failure;

	if (!hook_list)
		hook_list = &flowtable->hook_list;

	list_for_each_entry_rcu(hook, hook_list, list,
				lockdep_commit_lock_is_held(net)) {
		if (nla_put(skb, NFTA_DEVICE_NAME,
			    hook->ifnamelen, hook->ifname))
			goto nla_put_failure;
	}
	nla_nest_end(skb, nest_devs);
	nla_nest_end(skb, nest);

	nlmsg_end(skb, nlh);
	return 0;

nla_put_failure:
	nlmsg_trim(skb, nlh);
	return -1;
}

struct nft_flowtable_filter {
	char		*table;
};

static int nf_tables_dump_flowtable(struct sk_buff *skb,
				    struct netlink_callback *cb)
{
	const struct nfgenmsg *nfmsg = nlmsg_data(cb->nlh);
	struct nft_flowtable_filter *filter = cb->data;
	unsigned int idx = 0, s_idx = cb->args[0];
	struct net *net = sock_net(skb->sk);
	int family = nfmsg->nfgen_family;
	struct nft_flowtable *flowtable;
	struct nftables_pernet *nft_net;
	const struct nft_table *table;

	rcu_read_lock();
	nft_net = nft_pernet(net);
	cb->seq = READ_ONCE(nft_net->base_seq);

	list_for_each_entry_rcu(table, &nft_net->tables, list) {
		if (family != NFPROTO_UNSPEC && family != table->family)
			continue;

		list_for_each_entry_rcu(flowtable, &table->flowtables, list) {
			if (!nft_is_active(net, flowtable))
				goto cont;
			if (idx < s_idx)
				goto cont;
			if (idx > s_idx)
				memset(&cb->args[1], 0,
				       sizeof(cb->args) - sizeof(cb->args[0]));
			if (filter && filter->table &&
			    strcmp(filter->table, table->name))
				goto cont;

			if (nf_tables_fill_flowtable_info(skb, net, NETLINK_CB(cb->skb).portid,
							  cb->nlh->nlmsg_seq,
							  NFT_MSG_NEWFLOWTABLE,
							  NLM_F_MULTI | NLM_F_APPEND,
							  table->family,
							  flowtable, NULL) < 0)
				goto done;

			nl_dump_check_consistent(cb, nlmsg_hdr(skb));
cont:
			idx++;
		}
	}
done:
	rcu_read_unlock();

	cb->args[0] = idx;
	return skb->len;
}

static int nf_tables_dump_flowtable_start(struct netlink_callback *cb)
{
	const struct nlattr * const *nla = cb->data;
	struct nft_flowtable_filter *filter = NULL;

	if (nla[NFTA_FLOWTABLE_TABLE]) {
		filter = kzalloc(sizeof(*filter), GFP_ATOMIC);
		if (!filter)
			return -ENOMEM;

		filter->table = nla_strdup(nla[NFTA_FLOWTABLE_TABLE],
					   GFP_ATOMIC);
		if (!filter->table) {
			kfree(filter);
			return -ENOMEM;
		}
	}

	cb->data = filter;
	return 0;
}

static int nf_tables_dump_flowtable_done(struct netlink_callback *cb)
{
	struct nft_flowtable_filter *filter = cb->data;

	if (!filter)
		return 0;

	kfree(filter->table);
	kfree(filter);

	return 0;
}

/* called with rcu_read_lock held */
static int nf_tables_getflowtable(struct sk_buff *skb,
				  const struct nfnl_info *info,
				  const struct nlattr * const nla[])
{
	struct netlink_ext_ack *extack = info->extack;
	u8 genmask = nft_genmask_cur(info->net);
	u8 family = info->nfmsg->nfgen_family;
	struct nft_flowtable *flowtable;
	const struct nft_table *table;
	struct net *net = info->net;
	struct sk_buff *skb2;
	int err;

	if (info->nlh->nlmsg_flags & NLM_F_DUMP) {
		struct netlink_dump_control c = {
			.start = nf_tables_dump_flowtable_start,
			.dump = nf_tables_dump_flowtable,
			.done = nf_tables_dump_flowtable_done,
			.module = THIS_MODULE,
			.data = (void *)nla,
		};

		return nft_netlink_dump_start_rcu(info->sk, skb, info->nlh, &c);
	}

	if (!nla[NFTA_FLOWTABLE_NAME])
		return -EINVAL;

	table = nft_table_lookup(net, nla[NFTA_FLOWTABLE_TABLE], family,
				 genmask, 0);
	if (IS_ERR(table)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_FLOWTABLE_TABLE]);
		return PTR_ERR(table);
	}

	flowtable = nft_flowtable_lookup(net, table, nla[NFTA_FLOWTABLE_NAME],
					 genmask);
	if (IS_ERR(flowtable)) {
		NL_SET_BAD_ATTR(extack, nla[NFTA_FLOWTABLE_NAME]);
		return PTR_ERR(flowtable);
	}

	skb2 = alloc_skb(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb2)
		return -ENOMEM;

	err = nf_tables_fill_flowtable_info(skb2, net, NETLINK_CB(skb).portid,
					    info->nlh->nlmsg_seq,
					    NFT_MSG_NEWFLOWTABLE, 0, family,
					    flowtable, NULL);
	if (err < 0)
		goto err_fill_flowtable_info;

	return nfnetlink_unicast(skb2, net, NETLINK_CB(skb).portid);

err_fill_flowtable_info:
	kfree_skb(skb2);
	return err;
}

static void nf_tables_flowtable_notify(struct nft_ctx *ctx,
				       struct nft_flowtable *flowtable,
				       struct list_head *hook_list, int event)
{
	struct nftables_pernet *nft_net = nft_pernet(ctx->net);
	struct sk_buff *skb;
	u16 flags = 0;
	int err;

	if (!ctx->report &&
	    !nfnetlink_has_listeners(ctx->net, NFNLGRP_NFTABLES))
		return;

	skb = nlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (skb == NULL)
		goto err;

	if (ctx->flags & (NLM_F_CREATE | NLM_F_EXCL))
		flags |= ctx->flags & (NLM_F_CREATE | NLM_F_EXCL);

	err = nf_tables_fill_flowtable_info(skb, ctx->net, ctx->portid,
					    ctx->seq, event, flags,
					    ctx->family, flowtable, hook_list);
	if (err < 0) {
		kfree_skb(skb);
		goto err;
	}

	nft_notify_enqueue(skb, ctx->report, &nft_net->notify_list);
	return;
err:
	nfnetlink_set_err(ctx->net, ctx->portid, NFNLGRP_NFTABLES, -ENOBUFS);
}

static void nf_tables_flowtable_destroy(struct nft_flowtable *flowtable)
{
	struct nft_hook *hook, *next;

	flowtable->data.type->free(&flowtable->data);
	list_for_each_entry_safe(hook, next, &flowtable->hook_list, list) {
		list_del_rcu(&hook->list);
		nft_netdev_hook_free_rcu(hook);
	}
	kfree(flowtable->name);
	module_put(flowtable->data.type->owner);
	kfree(flowtable);
}

static int nf_tables_fill_gen_info(struct sk_buff *skb, struct net *net,
				   u32 portid, u32 seq)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	struct nlmsghdr *nlh;
	char buf[TASK_COMM_LEN];
	int event = nfnl_msg_type(NFNL_SUBSYS_NFTABLES, NFT_MSG_NEWGEN);

	nlh = nfnl_msg_put(skb, portid, seq, event, 0, AF_UNSPEC,
			   NFNETLINK_V0, nft_base_seq(net));
	if (!nlh)
		goto nla_put_failure;

	if (nla_put_be32(skb, NFTA_GEN_ID, htonl(nft_net->base_seq)) ||
	    nla_put_be32(skb, NFTA_GEN_PROC_PID, htonl(task_pid_nr(current))) ||
	    nla_put_string(skb, NFTA_GEN_PROC_NAME, get_task_comm(buf, current)))
		goto nla_put_failure;

	nlmsg_end(skb, nlh);
	return 0;

nla_put_failure:
	nlmsg_trim(skb, nlh);
	return -EMSGSIZE;
}

struct nf_hook_ops *nft_hook_find_ops(const struct nft_hook *hook,
				      const struct net_device *dev)
{
	struct nf_hook_ops *ops;

	list_for_each_entry(ops, &hook->ops_list, list) {
		if (ops->dev == dev)
			return ops;
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(nft_hook_find_ops);

struct nf_hook_ops *nft_hook_find_ops_rcu(const struct nft_hook *hook,
					  const struct net_device *dev)
{
	struct nf_hook_ops *ops;

	list_for_each_entry_rcu(ops, &hook->ops_list, list) {
		if (ops->dev == dev)
			return ops;
	}
	return NULL;
}
EXPORT_SYMBOL_GPL(nft_hook_find_ops_rcu);

static int nft_flowtable_event(unsigned long event, struct net_device *dev,
			       struct nft_flowtable *flowtable, bool changename)
{
	struct nf_hook_ops *ops;
	struct nft_hook *hook;
	bool match;

	list_for_each_entry(hook, &flowtable->hook_list, list) {
		ops = nft_hook_find_ops(hook, dev);
		match = !strncmp(hook->ifname, dev->name, hook->ifnamelen);

		switch (event) {
		case NETDEV_UNREGISTER:
			/* NOP if not found or new name still matching */
			if (!ops || (changename && match))
				continue;

			/* flow_offload_netdev_event() cleans up entries for us. */
			nft_unregister_flowtable_ops(dev_net(dev),
						     flowtable, ops);
			list_del_rcu(&ops->list);
			kfree_rcu(ops, rcu);
			break;
		case NETDEV_REGISTER:
			/* NOP if not matching or already registered */
			if (!match || (changename && ops))
				continue;

			ops = kzalloc(sizeof(struct nf_hook_ops),
				      GFP_KERNEL_ACCOUNT);
			if (!ops)
				return 1;

			ops->pf			= NFPROTO_NETDEV;
			ops->hooknum		= flowtable->hooknum;
			ops->priority		= flowtable->data.priority;
			ops->priv		= &flowtable->data;
			ops->hook		= flowtable->data.type->hook;
			ops->hook_ops_type	= NF_HOOK_OP_NFT_FT;
			ops->dev		= dev;
			if (nft_register_flowtable_ops(dev_net(dev),
						       flowtable, ops)) {
				kfree(ops);
				return 1;
			}
			list_add_tail_rcu(&ops->list, &hook->ops_list);
			break;
		}
		break;
	}
	return 0;
}

static int __nf_tables_flowtable_event(unsigned long event,
				       struct net_device *dev,
				       bool changename)
{
	struct nftables_pernet *nft_net = nft_pernet(dev_net(dev));
	struct nft_flowtable *flowtable;
	struct nft_table *table;

	list_for_each_entry(table, &nft_net->tables, list) {
		list_for_each_entry(flowtable, &table->flowtables, list) {
			if (nft_flowtable_event(event, dev,
						flowtable, changename))
				return 1;
		}
	}
	return 0;
}

static int nf_tables_flowtable_event(struct notifier_block *this,
				     unsigned long event, void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);
	struct nftables_pernet *nft_net;
	int ret = NOTIFY_DONE;
	struct net *net;

	if (event != NETDEV_REGISTER &&
	    event != NETDEV_UNREGISTER &&
	    event != NETDEV_CHANGENAME)
		return NOTIFY_DONE;

	net = dev_net(dev);
	nft_net = nft_pernet(net);
	mutex_lock(&nft_net->commit_mutex);

	if (event == NETDEV_CHANGENAME) {
		if (__nf_tables_flowtable_event(NETDEV_REGISTER, dev, true)) {
			ret = NOTIFY_BAD;
			goto out_unlock;
		}
		__nf_tables_flowtable_event(NETDEV_UNREGISTER, dev, true);
	} else if (__nf_tables_flowtable_event(event, dev, false)) {
		ret = NOTIFY_BAD;
	}
out_unlock:
	mutex_unlock(&nft_net->commit_mutex);
	return ret;
}

static struct notifier_block nf_tables_flowtable_notifier = {
	.notifier_call	= nf_tables_flowtable_event,
};

static void nf_tables_gen_notify(struct net *net, struct sk_buff *skb,
				 int event)
{
	struct nlmsghdr *nlh = nlmsg_hdr(skb);
	struct sk_buff *skb2;
	int err;

	if (!nlmsg_report(nlh) &&
	    !nfnetlink_has_listeners(net, NFNLGRP_NFTABLES))
		return;

	skb2 = nlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (skb2 == NULL)
		goto err;

	err = nf_tables_fill_gen_info(skb2, net, NETLINK_CB(skb).portid,
				      nlh->nlmsg_seq);
	if (err < 0) {
		kfree_skb(skb2);
		goto err;
	}

	nfnetlink_send(skb2, net, NETLINK_CB(skb).portid, NFNLGRP_NFTABLES,
		       nlmsg_report(nlh), GFP_KERNEL);
	return;
err:
	nfnetlink_set_err(net, NETLINK_CB(skb).portid, NFNLGRP_NFTABLES,
			  -ENOBUFS);
}

static int nf_tables_getgen(struct sk_buff *skb, const struct nfnl_info *info,
			    const struct nlattr * const nla[])
{
	struct sk_buff *skb2;
	int err;

	skb2 = alloc_skb(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (skb2 == NULL)
		return -ENOMEM;

	err = nf_tables_fill_gen_info(skb2, info->net, NETLINK_CB(skb).portid,
				      info->nlh->nlmsg_seq);
	if (err < 0)
		goto err_fill_gen_info;

	return nfnetlink_unicast(skb2, info->net, NETLINK_CB(skb).portid);

err_fill_gen_info:
	kfree_skb(skb2);
	return err;
}

static const struct nfnl_callback nf_tables_cb[NFT_MSG_MAX] = {
	[NFT_MSG_NEWTABLE] = {
		.call		= nf_tables_newtable,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_TABLE_MAX,
		.policy		= nft_table_policy,
	},
	[NFT_MSG_GETTABLE] = {
		.call		= nf_tables_gettable,
		.type		= NFNL_CB_RCU,
		.attr_count	= NFTA_TABLE_MAX,
		.policy		= nft_table_policy,
	},
	[NFT_MSG_DELTABLE] = {
		.call		= nf_tables_deltable,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_TABLE_MAX,
		.policy		= nft_table_policy,
	},
	[NFT_MSG_DESTROYTABLE] = {
		.call		= nf_tables_deltable,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_TABLE_MAX,
		.policy		= nft_table_policy,
	},
	[NFT_MSG_NEWCHAIN] = {
		.call		= nf_tables_newchain,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_CHAIN_MAX,
		.policy		= nft_chain_policy,
	},
	[NFT_MSG_GETCHAIN] = {
		.call		= nf_tables_getchain,
		.type		= NFNL_CB_RCU,
		.attr_count	= NFTA_CHAIN_MAX,
		.policy		= nft_chain_policy,
	},
	[NFT_MSG_DELCHAIN] = {
		.call		= nf_tables_delchain,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_CHAIN_MAX,
		.policy		= nft_chain_policy,
	},
	[NFT_MSG_DESTROYCHAIN] = {
		.call		= nf_tables_delchain,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_CHAIN_MAX,
		.policy		= nft_chain_policy,
	},
	[NFT_MSG_NEWRULE] = {
		.call		= nf_tables_newrule,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_RULE_MAX,
		.policy		= nft_rule_policy,
	},
	[NFT_MSG_GETRULE] = {
		.call		= nf_tables_getrule,
		.type		= NFNL_CB_RCU,
		.attr_count	= NFTA_RULE_MAX,
		.policy		= nft_rule_policy,
	},
	[NFT_MSG_GETRULE_RESET] = {
		.call		= nf_tables_getrule_reset,
		.type		= NFNL_CB_RCU,
		.attr_count	= NFTA_RULE_MAX,
		.policy		= nft_rule_policy,
	},
	[NFT_MSG_DELRULE] = {
		.call		= nf_tables_delrule,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_RULE_MAX,
		.policy		= nft_rule_policy,
	},
	[NFT_MSG_DESTROYRULE] = {
		.call		= nf_tables_delrule,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_RULE_MAX,
		.policy		= nft_rule_policy,
	},
	[NFT_MSG_NEWSET] = {
		.call		= nf_tables_newset,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_SET_MAX,
		.policy		= nft_set_policy,
	},
	[NFT_MSG_GETSET] = {
		.call		= nf_tables_getset,
		.type		= NFNL_CB_RCU,
		.attr_count	= NFTA_SET_MAX,
		.policy		= nft_set_policy,
	},
	[NFT_MSG_DELSET] = {
		.call		= nf_tables_delset,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_SET_MAX,
		.policy		= nft_set_policy,
	},
	[NFT_MSG_DESTROYSET] = {
		.call		= nf_tables_delset,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_SET_MAX,
		.policy		= nft_set_policy,
	},
	[NFT_MSG_NEWSETELEM] = {
		.call		= nf_tables_newsetelem,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_SET_ELEM_LIST_MAX,
		.policy		= nft_set_elem_list_policy,
	},
	[NFT_MSG_GETSETELEM] = {
		.call		= nf_tables_getsetelem,
		.type		= NFNL_CB_RCU,
		.attr_count	= NFTA_SET_ELEM_LIST_MAX,
		.policy		= nft_set_elem_list_policy,
	},
	[NFT_MSG_GETSETELEM_RESET] = {
		.call		= nf_tables_getsetelem_reset,
		.type		= NFNL_CB_RCU,
		.attr_count	= NFTA_SET_ELEM_LIST_MAX,
		.policy		= nft_set_elem_list_policy,
	},
	[NFT_MSG_DELSETELEM] = {
		.call		= nf_tables_delsetelem,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_SET_ELEM_LIST_MAX,
		.policy		= nft_set_elem_list_policy,
	},
	[NFT_MSG_DESTROYSETELEM] = {
		.call		= nf_tables_delsetelem,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_SET_ELEM_LIST_MAX,
		.policy		= nft_set_elem_list_policy,
	},
	[NFT_MSG_GETGEN] = {
		.call		= nf_tables_getgen,
		.type		= NFNL_CB_RCU,
	},
	[NFT_MSG_NEWOBJ] = {
		.call		= nf_tables_newobj,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_OBJ_MAX,
		.policy		= nft_obj_policy,
	},
	[NFT_MSG_GETOBJ] = {
		.call		= nf_tables_getobj,
		.type		= NFNL_CB_RCU,
		.attr_count	= NFTA_OBJ_MAX,
		.policy		= nft_obj_policy,
	},
	[NFT_MSG_DELOBJ] = {
		.call		= nf_tables_delobj,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_OBJ_MAX,
		.policy		= nft_obj_policy,
	},
	[NFT_MSG_DESTROYOBJ] = {
		.call		= nf_tables_delobj,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_OBJ_MAX,
		.policy		= nft_obj_policy,
	},
	[NFT_MSG_GETOBJ_RESET] = {
		.call		= nf_tables_getobj_reset,
		.type		= NFNL_CB_RCU,
		.attr_count	= NFTA_OBJ_MAX,
		.policy		= nft_obj_policy,
	},
	[NFT_MSG_NEWFLOWTABLE] = {
		.call		= nf_tables_newflowtable,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_FLOWTABLE_MAX,
		.policy		= nft_flowtable_policy,
	},
	[NFT_MSG_GETFLOWTABLE] = {
		.call		= nf_tables_getflowtable,
		.type		= NFNL_CB_RCU,
		.attr_count	= NFTA_FLOWTABLE_MAX,
		.policy		= nft_flowtable_policy,
	},
	[NFT_MSG_DELFLOWTABLE] = {
		.call		= nf_tables_delflowtable,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_FLOWTABLE_MAX,
		.policy		= nft_flowtable_policy,
	},
	[NFT_MSG_DESTROYFLOWTABLE] = {
		.call		= nf_tables_delflowtable,
		.type		= NFNL_CB_BATCH,
		.attr_count	= NFTA_FLOWTABLE_MAX,
		.policy		= nft_flowtable_policy,
	},
};

static int nf_tables_validate(struct net *net)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	struct nft_table *table;

	list_for_each_entry(table, &nft_net->tables, list) {
		switch (table->validate_state) {
		case NFT_VALIDATE_SKIP:
			continue;
		case NFT_VALIDATE_NEED:
			nft_validate_state_update(table, NFT_VALIDATE_DO);
			fallthrough;
		case NFT_VALIDATE_DO:
			if (nft_table_validate(net, table) < 0)
				return -EAGAIN;

			nft_validate_state_update(table, NFT_VALIDATE_SKIP);
			break;
		}
	}

	return 0;
}

/* a drop policy has to be deferred until all rules have been activated,
 * otherwise a large ruleset that contains a drop-policy base chain will
 * cause all packets to get dropped until the full transaction has been
 * processed.
 *
 * We defer the drop policy until the transaction has been finalized.
 */
static void nft_chain_commit_drop_policy(struct nft_trans_chain *trans)
{
	struct nft_base_chain *basechain;

	if (trans->policy != NF_DROP)
		return;

	if (!nft_is_base_chain(trans->chain))
		return;

	basechain = nft_base_chain(trans->chain);
	basechain->policy = NF_DROP;
}

static void nft_chain_commit_update(struct nft_trans_chain *trans)
{
	struct nft_table *table = trans->nft_trans_binding.nft_trans.table;
	struct nft_base_chain *basechain;

	if (trans->name) {
		rhltable_remove(&table->chains_ht,
				&trans->chain->rhlhead,
				nft_chain_ht_params);
		swap(trans->chain->name, trans->name);
		rhltable_insert_key(&table->chains_ht,
				    trans->chain->name,
				    &trans->chain->rhlhead,
				    nft_chain_ht_params);
	}

	if (!nft_is_base_chain(trans->chain))
		return;

	nft_chain_stats_replace(trans);

	basechain = nft_base_chain(trans->chain);

	switch (trans->policy) {
	case NF_DROP:
	case NF_ACCEPT:
		basechain->policy = trans->policy;
		break;
	}
}

static void nft_obj_commit_update(const struct nft_ctx *ctx,
				  struct nft_trans *trans)
{
	struct nft_object *newobj;
	struct nft_object *obj;

	obj = nft_trans_obj(trans);
	newobj = nft_trans_obj_newobj(trans);

	if (WARN_ON_ONCE(!obj->ops->update))
		return;

	obj->ops->update(obj, newobj);
	nft_obj_destroy(ctx, newobj);
}

static void nft_commit_release(struct nft_trans *trans)
{
	struct nft_ctx ctx = {
		.net = trans->net,
	};

	nft_ctx_update(&ctx, trans);

	switch (trans->msg_type) {
	case NFT_MSG_DELTABLE:
	case NFT_MSG_DESTROYTABLE:
		nf_tables_table_destroy(trans->table);
		break;
	case NFT_MSG_NEWCHAIN:
		free_percpu(nft_trans_chain_stats(trans));
		kfree(nft_trans_chain_name(trans));
		break;
	case NFT_MSG_DELCHAIN:
	case NFT_MSG_DESTROYCHAIN:
		if (nft_trans_chain_update(trans))
			nft_hooks_destroy(&nft_trans_chain_hooks(trans));
		else
			nf_tables_chain_destroy(nft_trans_chain(trans));
		break;
	case NFT_MSG_DELRULE:
	case NFT_MSG_DESTROYRULE:
		nf_tables_rule_destroy(&ctx, nft_trans_rule(trans));
		break;
	case NFT_MSG_DELSET:
	case NFT_MSG_DESTROYSET:
		nft_set_destroy(&ctx, nft_trans_set(trans));
		break;
	case NFT_MSG_DELSETELEM:
	case NFT_MSG_DESTROYSETELEM:
		nft_trans_elems_destroy(&ctx, nft_trans_container_elem(trans));
		break;
	case NFT_MSG_DELOBJ:
	case NFT_MSG_DESTROYOBJ:
		nft_obj_destroy(&ctx, nft_trans_obj(trans));
		break;
	case NFT_MSG_DELFLOWTABLE:
	case NFT_MSG_DESTROYFLOWTABLE:
		if (nft_trans_flowtable_update(trans))
			nft_hooks_destroy(&nft_trans_flowtable_hooks(trans));
		else
			nf_tables_flowtable_destroy(nft_trans_flowtable(trans));
		break;
	}

	if (trans->put_net)
		put_net(trans->net);

	kfree(trans);
}

static void nf_tables_trans_destroy_work(struct work_struct *w)
{
	struct nftables_pernet *nft_net = container_of(w, struct nftables_pernet, destroy_work);
	struct nft_trans *trans, *next;
	LIST_HEAD(head);

	spin_lock(&nf_tables_destroy_list_lock);
	list_splice_init(&nft_net->destroy_list, &head);
	spin_unlock(&nf_tables_destroy_list_lock);

	if (list_empty(&head))
		return;

	synchronize_rcu();

	list_for_each_entry_safe(trans, next, &head, list) {
		nft_trans_list_del(trans);
		nft_commit_release(trans);
	}
}

void nf_tables_trans_destroy_flush_work(struct net *net)
{
	struct nftables_pernet *nft_net = nft_pernet(net);

	flush_work(&nft_net->destroy_work);
}
EXPORT_SYMBOL_GPL(nf_tables_trans_destroy_flush_work);

static bool nft_expr_reduce(struct nft_regs_track *track,
			    const struct nft_expr *expr)
{
	return false;
}

static int nf_tables_commit_chain_prepare(struct net *net, struct nft_chain *chain)
{
	const struct nft_expr *expr, *last;
	struct nft_regs_track track = {};
	unsigned int size, data_size;
	void *data, *data_boundary;
	struct nft_rule_dp *prule;
	struct nft_rule *rule;

	/* already handled or inactive chain? */
	if (chain->blob_next || !nft_is_active_next(net, chain))
		return 0;

	data_size = 0;
	list_for_each_entry(rule, &chain->rules, list) {
		if (nft_is_active_next(net, rule)) {
			data_size += sizeof(*prule) + rule->dlen;
			if (data_size > INT_MAX)
				return -ENOMEM;
		}
	}

	chain->blob_next = nf_tables_chain_alloc_rules(chain, data_size);
	if (!chain->blob_next)
		return -ENOMEM;

	data = (void *)chain->blob_next->data;
	data_boundary = data + data_size;
	size = 0;

	list_for_each_entry(rule, &chain->rules, list) {
		if (!nft_is_active_next(net, rule))
			continue;

		prule = (struct nft_rule_dp *)data;
		data += offsetof(struct nft_rule_dp, data);
		if (WARN_ON_ONCE(data > data_boundary))
			return -ENOMEM;

		size = 0;
		track.last = nft_expr_last(rule);
		nft_rule_for_each_expr(expr, last, rule) {
			track.cur = expr;

			if (nft_expr_reduce(&track, expr)) {
				expr = track.cur;
				continue;
			}

			if (WARN_ON_ONCE(data + size + expr->ops->size > data_boundary))
				return -ENOMEM;

			memcpy(data + size, expr, expr->ops->size);
			size += expr->ops->size;
		}
		if (WARN_ON_ONCE(size >= 1 << 12))
			return -ENOMEM;

		prule->handle = rule->handle;
		prule->dlen = size;
		prule->is_last = 0;

		data += size;
		size = 0;
		chain->blob_next->size += (unsigned long)(data - (void *)prule);
	}

	if (WARN_ON_ONCE(data > data_boundary))
		return -ENOMEM;

	prule = (struct nft_rule_dp *)data;
	nft_last_rule(chain, prule);

	return 0;
}

static void nf_tables_commit_chain_prepare_cancel(struct net *net)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	struct nft_trans *trans, *next;

	list_for_each_entry_safe(trans, next, &nft_net->commit_list, list) {
		if (trans->msg_type == NFT_MSG_NEWRULE ||
		    trans->msg_type == NFT_MSG_DELRULE) {
			struct nft_chain *chain = nft_trans_rule_chain(trans);

			kvfree(chain->blob_next);
			chain->blob_next = NULL;
		}
	}
}

static void __nf_tables_commit_chain_free_rules(struct rcu_head *h)
{
	struct nft_rule_dp_last *l = container_of(h, struct nft_rule_dp_last, h);

	kvfree(l->blob);
}

static void nf_tables_commit_chain_free_rules_old(struct nft_rule_blob *blob)
{
	struct nft_rule_dp_last *last;

	/* last rule trailer is after end marker */
	last = (void *)blob + sizeof(*blob) + blob->size;
	last->blob = blob;

	call_rcu(&last->h, __nf_tables_commit_chain_free_rules);
}

static void nf_tables_commit_chain(struct net *net, struct nft_chain *chain)
{
	struct nft_rule_blob *g0, *g1;
	bool next_genbit;

	next_genbit = nft_gencursor_next(net);

	g0 = rcu_dereference_protected(chain->blob_gen_0,
				       lockdep_commit_lock_is_held(net));
	g1 = rcu_dereference_protected(chain->blob_gen_1,
				       lockdep_commit_lock_is_held(net));

	/* No changes to this chain? */
	if (chain->blob_next == NULL) {
		/* chain had no change in last or next generation */
		if (g0 == g1)
			return;
		/*
		 * chain had no change in this generation; make sure next
		 * one uses same rules as current generation.
		 */
		if (next_genbit) {
			rcu_assign_pointer(chain->blob_gen_1, g0);
			nf_tables_commit_chain_free_rules_old(g1);
		} else {
			rcu_assign_pointer(chain->blob_gen_0, g1);
			nf_tables_commit_chain_free_rules_old(g0);
		}

		return;
	}

	if (next_genbit)
		rcu_assign_pointer(chain->blob_gen_1, chain->blob_next);
	else
		rcu_assign_pointer(chain->blob_gen_0, chain->blob_next);

	chain->blob_next = NULL;

	if (g0 == g1)
		return;

	if (next_genbit)
		nf_tables_commit_chain_free_rules_old(g1);
	else
		nf_tables_commit_chain_free_rules_old(g0);
}

static void nft_obj_del(struct nft_object *obj)
{
	rhltable_remove(&nft_objname_ht, &obj->rhlhead, nft_objname_ht_params);
	list_del_rcu(&obj->list);
}

void nft_chain_del(struct nft_chain *chain)
{
	struct nft_table *table = chain->table;

	WARN_ON_ONCE(rhltable_remove(&table->chains_ht, &chain->rhlhead,
				     nft_chain_ht_params));
	list_del_rcu(&chain->list);
}

static void nft_trans_gc_setelem_remove(struct nft_ctx *ctx,
					struct nft_trans_gc *trans)
{
	struct nft_elem_priv **priv = trans->priv;
	unsigned int i;

	for (i = 0; i < trans->count; i++) {
		nft_setelem_data_deactivate(ctx->net, trans->set, priv[i]);
		nft_setelem_remove(ctx->net, trans->set, priv[i]);
	}
}

void nft_trans_gc_destroy(struct nft_trans_gc *trans)
{
	nft_set_put(trans->set);
	put_net(trans->net);
	kfree(trans);
}

static void nft_trans_gc_trans_free(struct rcu_head *rcu)
{
	struct nft_elem_priv *elem_priv;
	struct nft_trans_gc *trans;
	struct nft_ctx ctx = {};
	unsigned int i;

	trans = container_of(rcu, struct nft_trans_gc, rcu);
	ctx.net	= read_pnet(&trans->set->net);

	for (i = 0; i < trans->count; i++) {
		elem_priv = trans->priv[i];
		if (!nft_setelem_is_catchall(trans->set, elem_priv))
			atomic_dec(&trans->set->nelems);

		nf_tables_set_elem_destroy(&ctx, trans->set, elem_priv);
	}

	nft_trans_gc_destroy(trans);
}

static bool nft_trans_gc_work_done(struct nft_trans_gc *trans)
{
	struct nftables_pernet *nft_net;
	struct nft_ctx ctx = {};

	nft_net = nft_pernet(trans->net);

	mutex_lock(&nft_net->commit_mutex);

	/* Check for race with transaction, otherwise this batch refers to
	 * stale objects that might not be there anymore. Skip transaction if
	 * set has been destroyed from control plane transaction in case gc
	 * worker loses race.
	 */
	if (READ_ONCE(nft_net->gc_seq) != trans->seq || trans->set->dead) {
		mutex_unlock(&nft_net->commit_mutex);
		return false;
	}

	ctx.net = trans->net;
	ctx.table = trans->set->table;

	nft_trans_gc_setelem_remove(&ctx, trans);
	mutex_unlock(&nft_net->commit_mutex);

	return true;
}

static void nft_trans_gc_work(struct work_struct *work)
{
	struct nft_trans_gc *trans, *next;
	LIST_HEAD(trans_gc_list);

	spin_lock(&nf_tables_gc_list_lock);
	list_splice_init(&nf_tables_gc_list, &trans_gc_list);
	spin_unlock(&nf_tables_gc_list_lock);

	list_for_each_entry_safe(trans, next, &trans_gc_list, list) {
		list_del(&trans->list);
		if (!nft_trans_gc_work_done(trans)) {
			nft_trans_gc_destroy(trans);
			continue;
		}
		call_rcu(&trans->rcu, nft_trans_gc_trans_free);
	}
}

struct nft_trans_gc *nft_trans_gc_alloc(struct nft_set *set,
					unsigned int gc_seq, gfp_t gfp)
{
	struct net *net = read_pnet(&set->net);
	struct nft_trans_gc *trans;

	trans = kzalloc(sizeof(*trans), gfp);
	if (!trans)
		return NULL;

	trans->net = maybe_get_net(net);
	if (!trans->net) {
		kfree(trans);
		return NULL;
	}

	refcount_inc(&set->refs);
	trans->set = set;
	trans->seq = gc_seq;

	return trans;
}

void nft_trans_gc_elem_add(struct nft_trans_gc *trans, void *priv)
{
	trans->priv[trans->count++] = priv;
}

static void nft_trans_gc_queue_work(struct nft_trans_gc *trans)
{
	spin_lock(&nf_tables_gc_list_lock);
	list_add_tail(&trans->list, &nf_tables_gc_list);
	spin_unlock(&nf_tables_gc_list_lock);

	schedule_work(&trans_gc_work);
}

static int nft_trans_gc_space(struct nft_trans_gc *trans)
{
	return NFT_TRANS_GC_BATCHCOUNT - trans->count;
}

struct nft_trans_gc *nft_trans_gc_queue_async(struct nft_trans_gc *gc,
					      unsigned int gc_seq, gfp_t gfp)
{
	struct nft_set *set;

	if (nft_trans_gc_space(gc))
		return gc;

	set = gc->set;
	nft_trans_gc_queue_work(gc);

	return nft_trans_gc_alloc(set, gc_seq, gfp);
}

void nft_trans_gc_queue_async_done(struct nft_trans_gc *trans)
{
	if (trans->count == 0) {
		nft_trans_gc_destroy(trans);
		return;
	}

	nft_trans_gc_queue_work(trans);
}

struct nft_trans_gc *nft_trans_gc_queue_sync(struct nft_trans_gc *gc, gfp_t gfp)
{
	struct nft_set *set;

	if (WARN_ON_ONCE(!lockdep_commit_lock_is_held(gc->net)))
		return NULL;

	if (nft_trans_gc_space(gc))
		return gc;

	set = gc->set;
	call_rcu(&gc->rcu, nft_trans_gc_trans_free);

	return nft_trans_gc_alloc(set, 0, gfp);
}

void nft_trans_gc_queue_sync_done(struct nft_trans_gc *trans)
{
	WARN_ON_ONCE(!lockdep_commit_lock_is_held(trans->net));

	if (trans->count == 0) {
		nft_trans_gc_destroy(trans);
		return;
	}

	call_rcu(&trans->rcu, nft_trans_gc_trans_free);
}

struct nft_trans_gc *nft_trans_gc_catchall_async(struct nft_trans_gc *gc,
						 unsigned int gc_seq)
{
	struct nft_set_elem_catchall *catchall;
	const struct nft_set *set = gc->set;
	struct nft_set_ext *ext;

	list_for_each_entry_rcu(catchall, &set->catchall_list, list) {
		ext = nft_set_elem_ext(set, catchall->elem);

		if (!nft_set_elem_expired(ext))
			continue;
		if (nft_set_elem_is_dead(ext))
			goto dead_elem;

		nft_set_elem_dead(ext);
dead_elem:
		gc = nft_trans_gc_queue_async(gc, gc_seq, GFP_ATOMIC);
		if (!gc)
			return NULL;

		nft_trans_gc_elem_add(gc, catchall->elem);
	}

	return gc;
}

struct nft_trans_gc *nft_trans_gc_catchall_sync(struct nft_trans_gc *gc)
{
	struct nft_set_elem_catchall *catchall, *next;
	u64 tstamp = nft_net_tstamp(gc->net);
	const struct nft_set *set = gc->set;
	struct nft_elem_priv *elem_priv;
	struct nft_set_ext *ext;

	WARN_ON_ONCE(!lockdep_commit_lock_is_held(gc->net));

	list_for_each_entry_safe(catchall, next, &set->catchall_list, list) {
		ext = nft_set_elem_ext(set, catchall->elem);

		if (!__nft_set_elem_expired(ext, tstamp))
			continue;

		gc = nft_trans_gc_queue_sync(gc, GFP_KERNEL);
		if (!gc)
			return NULL;

		elem_priv = catchall->elem;
		nft_setelem_data_deactivate(gc->net, gc->set, elem_priv);
		nft_setelem_catchall_destroy(catchall);
		nft_trans_gc_elem_add(gc, elem_priv);
	}

	return gc;
}

static void nf_tables_module_autoload_cleanup(struct net *net)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	struct nft_module_request *req, *next;

	WARN_ON_ONCE(!list_empty(&nft_net->commit_list));
	list_for_each_entry_safe(req, next, &nft_net->module_list, list) {
		WARN_ON_ONCE(!req->done);
		list_del(&req->list);
		kfree(req);
	}
}

static void nf_tables_commit_release(struct net *net)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	struct nft_trans *trans;

	/* all side effects have to be made visible.
	 * For example, if a chain named 'foo' has been deleted, a
	 * new transaction must not find it anymore.
	 *
	 * Memory reclaim happens asynchronously from work queue
	 * to prevent expensive synchronize_rcu() in commit phase.
	 */
	if (list_empty(&nft_net->commit_list)) {
		nf_tables_module_autoload_cleanup(net);
		mutex_unlock(&nft_net->commit_mutex);
		return;
	}

	trans = list_last_entry(&nft_net->commit_list,
				struct nft_trans, list);
	get_net(trans->net);
	WARN_ON_ONCE(trans->put_net);

	trans->put_net = true;
	spin_lock(&nf_tables_destroy_list_lock);
	list_splice_tail_init(&nft_net->commit_list, &nft_net->destroy_list);
	spin_unlock(&nf_tables_destroy_list_lock);

	nf_tables_module_autoload_cleanup(net);
	schedule_work(&nft_net->destroy_work);

	mutex_unlock(&nft_net->commit_mutex);
}

static void nft_commit_notify(struct net *net, u32 portid)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	struct sk_buff *batch_skb = NULL, *nskb, *skb;
	unsigned char *data;
	int len;

	list_for_each_entry_safe(skb, nskb, &nft_net->notify_list, list) {
		if (!batch_skb) {
new_batch:
			batch_skb = skb;
			len = NLMSG_GOODSIZE - skb->len;
			list_del(&skb->list);
			continue;
		}
		len -= skb->len;
		if (len > 0 && NFT_CB(skb).report == NFT_CB(batch_skb).report) {
			data = skb_put(batch_skb, skb->len);
			memcpy(data, skb->data, skb->len);
			list_del(&skb->list);
			kfree_skb(skb);
			continue;
		}
		nfnetlink_send(batch_skb, net, portid, NFNLGRP_NFTABLES,
			       NFT_CB(batch_skb).report, GFP_KERNEL);
		goto new_batch;
	}

	if (batch_skb) {
		nfnetlink_send(batch_skb, net, portid, NFNLGRP_NFTABLES,
			       NFT_CB(batch_skb).report, GFP_KERNEL);
	}

	WARN_ON_ONCE(!list_empty(&nft_net->notify_list));
}

static int nf_tables_commit_audit_alloc(struct list_head *adl,
					struct nft_table *table)
{
	struct nft_audit_data *adp;

	list_for_each_entry(adp, adl, list) {
		if (adp->table == table)
			return 0;
	}
	adp = kzalloc(sizeof(*adp), GFP_KERNEL);
	if (!adp)
		return -ENOMEM;
	adp->table = table;
	list_add(&adp->list, adl);
	return 0;
}

static void nf_tables_commit_audit_free(struct list_head *adl)
{
	struct nft_audit_data *adp, *adn;

	list_for_each_entry_safe(adp, adn, adl, list) {
		list_del(&adp->list);
		kfree(adp);
	}
}

/* nft audit emits the number of elements that get added/removed/updated,
 * so NEW/DELSETELEM needs to increment based on the total elem count.
 */
static unsigned int nf_tables_commit_audit_entrycount(const struct nft_trans *trans)
{
	switch (trans->msg_type) {
	case NFT_MSG_NEWSETELEM:
	case NFT_MSG_DELSETELEM:
		return nft_trans_container_elem(trans)->nelems;
	}

	return 1;
}

static void nf_tables_commit_audit_collect(struct list_head *adl,
					   const struct nft_trans *trans, u32 op)
{
	const struct nft_table *table = trans->table;
	struct nft_audit_data *adp;

	list_for_each_entry(adp, adl, list) {
		if (adp->table == table)
			goto found;
	}
	WARN_ONCE(1, "table=%s not expected in commit list", table->name);
	return;
found:
	adp->entries += nf_tables_commit_audit_entrycount(trans);
	if (!adp->op || adp->op > op)
		adp->op = op;
}

#define AUNFTABLENAMELEN (NFT_TABLE_MAXNAMELEN + 22)

static void nf_tables_commit_audit_log(struct list_head *adl, u32 generation)
{
	struct nft_audit_data *adp, *adn;
	char aubuf[AUNFTABLENAMELEN];

	list_for_each_entry_safe(adp, adn, adl, list) {
		snprintf(aubuf, AUNFTABLENAMELEN, "%s:%u", adp->table->name,
			 generation);
		audit_log_nfcfg(aubuf, adp->table->family, adp->entries,
				nft2audit_op[adp->op], GFP_KERNEL);
		list_del(&adp->list);
		kfree(adp);
	}
}

static void nft_set_commit_update(struct list_head *set_update_list)
{
	struct nft_set *set, *next;

	list_for_each_entry_safe(set, next, set_update_list, pending_update) {
		list_del_init(&set->pending_update);

		if (!set->ops->commit || set->dead)
			continue;

		set->ops->commit(set);
	}
}

static unsigned int nft_gc_seq_begin(struct nftables_pernet *nft_net)
{
	unsigned int gc_seq;

	/* Bump gc counter, it becomes odd, this is the busy mark. */
	gc_seq = READ_ONCE(nft_net->gc_seq);
	WRITE_ONCE(nft_net->gc_seq, ++gc_seq);

	return gc_seq;
}

static void nft_gc_seq_end(struct nftables_pernet *nft_net, unsigned int gc_seq)
{
	WRITE_ONCE(nft_net->gc_seq, ++gc_seq);
}

static int nf_tables_commit(struct net *net, struct sk_buff *skb)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	const struct nlmsghdr *nlh = nlmsg_hdr(skb);
	struct nft_trans_binding *trans_binding;
	struct nft_trans *trans, *next;
	unsigned int base_seq, gc_seq;
	LIST_HEAD(set_update_list);
	struct nft_trans_elem *te;
	struct nft_chain *chain;
	struct nft_table *table;
	struct nft_ctx ctx;
	LIST_HEAD(adl);
	int err;

	if (list_empty(&nft_net->commit_list)) {
		mutex_unlock(&nft_net->commit_mutex);
		return 0;
	}

	nft_ctx_init(&ctx, net, skb, nlh, NFPROTO_UNSPEC, NULL, NULL, NULL);

	list_for_each_entry(trans_binding, &nft_net->binding_list, binding_list) {
		trans = &trans_binding->nft_trans;
		switch (trans->msg_type) {
		case NFT_MSG_NEWSET:
			if (!nft_trans_set_update(trans) &&
			    nft_set_is_anonymous(nft_trans_set(trans)) &&
			    !nft_trans_set_bound(trans)) {
				pr_warn_once("nftables ruleset with unbound set\n");
				return -EINVAL;
			}
			break;
		case NFT_MSG_NEWCHAIN:
			if (!nft_trans_chain_update(trans) &&
			    nft_chain_binding(nft_trans_chain(trans)) &&
			    !nft_trans_chain_bound(trans)) {
				pr_warn_once("nftables ruleset with unbound chain\n");
				return -EINVAL;
			}
			break;
		default:
			WARN_ONCE(1, "Unhandled bind type %d", trans->msg_type);
			break;
		}
	}

	/* 0. Validate ruleset, otherwise roll back for error reporting. */
	if (nf_tables_validate(net) < 0) {
		nft_net->validate_state = NFT_VALIDATE_DO;
		return -EAGAIN;
	}

	err = nft_flow_rule_offload_commit(net);
	if (err < 0)
		return err;

	/* 1.  Allocate space for next generation rules_gen_X[] */
	list_for_each_entry_safe(trans, next, &nft_net->commit_list, list) {
		struct nft_table *table = trans->table;
		int ret;

		ret = nf_tables_commit_audit_alloc(&adl, table);
		if (ret) {
			nf_tables_commit_chain_prepare_cancel(net);
			nf_tables_commit_audit_free(&adl);
			return ret;
		}
		if (trans->msg_type == NFT_MSG_NEWRULE ||
		    trans->msg_type == NFT_MSG_DELRULE) {
			chain = nft_trans_rule_chain(trans);

			ret = nf_tables_commit_chain_prepare(net, chain);
			if (ret < 0) {
				nf_tables_commit_chain_prepare_cancel(net);
				nf_tables_commit_audit_free(&adl);
				return ret;
			}
		}
	}

	/* step 2.  Make rules_gen_X visible to packet path */
	list_for_each_entry(table, &nft_net->tables, list) {
		list_for_each_entry(chain, &table->chains, list)
			nf_tables_commit_chain(net, chain);
	}

	/*
	 * Bump generation counter, invalidate any dump in progress.
	 * Cannot fail after this point.
	 */
	base_seq = READ_ONCE(nft_net->base_seq);
	while (++base_seq == 0)
		;

	WRITE_ONCE(nft_net->base_seq, base_seq);

	gc_seq = nft_gc_seq_begin(nft_net);

	/* step 3. Start new generation, rules_gen_X now in use. */
	net->nft.gencursor = nft_gencursor_next(net);

	list_for_each_entry_safe(trans, next, &nft_net->commit_list, list) {
		struct nft_table *table = trans->table;

		nft_ctx_update(&ctx, trans);

		nf_tables_commit_audit_collect(&adl, trans, trans->msg_type);
		switch (trans->msg_type) {
		case NFT_MSG_NEWTABLE:
			if (nft_trans_table_update(trans)) {
				if (!(table->flags & __NFT_TABLE_F_UPDATE)) {
					nft_trans_destroy(trans);
					break;
				}
				if (table->flags & NFT_TABLE_F_DORMANT)
					nf_tables_table_disable(net, table);

				table->flags &= ~__NFT_TABLE_F_UPDATE;
			} else {
				nft_clear(net, table);
			}
			nf_tables_table_notify(&ctx, NFT_MSG_NEWTABLE);
			nft_trans_destroy(trans);
			break;
		case NFT_MSG_DELTABLE:
		case NFT_MSG_DESTROYTABLE:
			list_del_rcu(&table->list);
			nf_tables_table_notify(&ctx, trans->msg_type);
			break;
		case NFT_MSG_NEWCHAIN:
			if (nft_trans_chain_update(trans)) {
				nft_chain_commit_update(nft_trans_container_chain(trans));
				nf_tables_chain_notify(&ctx, NFT_MSG_NEWCHAIN,
						       &nft_trans_chain_hooks(trans));
				list_splice(&nft_trans_chain_hooks(trans),
					    &nft_trans_basechain(trans)->hook_list);
				/* trans destroyed after rcu grace period */
			} else {
				nft_chain_commit_drop_policy(nft_trans_container_chain(trans));
				nft_clear(net, nft_trans_chain(trans));
				nf_tables_chain_notify(&ctx, NFT_MSG_NEWCHAIN, NULL);
				nft_trans_destroy(trans);
			}
			break;
		case NFT_MSG_DELCHAIN:
		case NFT_MSG_DESTROYCHAIN:
			if (nft_trans_chain_update(trans)) {
				nf_tables_chain_notify(&ctx, NFT_MSG_DELCHAIN,
						       &nft_trans_chain_hooks(trans));
				if (!(table->flags & NFT_TABLE_F_DORMANT)) {
					nft_netdev_unregister_hooks(net,
								    &nft_trans_chain_hooks(trans),
								    true);
				}
			} else {
				nft_chain_del(nft_trans_chain(trans));
				nf_tables_chain_notify(&ctx, NFT_MSG_DELCHAIN,
						       NULL);
				nf_tables_unregister_hook(ctx.net, ctx.table,
							  nft_trans_chain(trans));
			}
			break;
		case NFT_MSG_NEWRULE:
			nft_clear(net, nft_trans_rule(trans));
			nf_tables_rule_notify(&ctx, nft_trans_rule(trans),
					      NFT_MSG_NEWRULE);
			if (nft_trans_rule_chain(trans)->flags & NFT_CHAIN_HW_OFFLOAD)
				nft_flow_rule_destroy(nft_trans_flow_rule(trans));

			nft_trans_destroy(trans);
			break;
		case NFT_MSG_DELRULE:
		case NFT_MSG_DESTROYRULE:
			list_del_rcu(&nft_trans_rule(trans)->list);
			nf_tables_rule_notify(&ctx, nft_trans_rule(trans),
					      trans->msg_type);
			nft_rule_expr_deactivate(&ctx, nft_trans_rule(trans),
						 NFT_TRANS_COMMIT);

			if (nft_trans_rule_chain(trans)->flags & NFT_CHAIN_HW_OFFLOAD)
				nft_flow_rule_destroy(nft_trans_flow_rule(trans));
			break;
		case NFT_MSG_NEWSET:
			list_del(&nft_trans_container_set(trans)->list_trans_newset);
			if (nft_trans_set_update(trans)) {
				struct nft_set *set = nft_trans_set(trans);

				WRITE_ONCE(set->timeout, nft_trans_set_timeout(trans));
				WRITE_ONCE(set->gc_int, nft_trans_set_gc_int(trans));

				if (nft_trans_set_size(trans))
					WRITE_ONCE(set->size, nft_trans_set_size(trans));
			} else {
				nft_clear(net, nft_trans_set(trans));
				/* This avoids hitting -EBUSY when deleting the table
				 * from the transaction.
				 */
				if (nft_set_is_anonymous(nft_trans_set(trans)) &&
				    !list_empty(&nft_trans_set(trans)->bindings))
					nft_use_dec(&table->use);
			}
			nf_tables_set_notify(&ctx, nft_trans_set(trans),
					     NFT_MSG_NEWSET, GFP_KERNEL);
			nft_trans_destroy(trans);
			break;
		case NFT_MSG_DELSET:
		case NFT_MSG_DESTROYSET:
			nft_trans_set(trans)->dead = 1;
			list_del_rcu(&nft_trans_set(trans)->list);
			nf_tables_set_notify(&ctx, nft_trans_set(trans),
					     trans->msg_type, GFP_KERNEL);
			break;
		case NFT_MSG_NEWSETELEM:
			te = nft_trans_container_elem(trans);

			nft_trans_elems_add(&ctx, te);

			if (te->set->ops->commit &&
			    list_empty(&te->set->pending_update)) {
				list_add_tail(&te->set->pending_update,
					      &set_update_list);
			}
			nft_trans_destroy(trans);
			break;
		case NFT_MSG_DELSETELEM:
		case NFT_MSG_DESTROYSETELEM:
			te = nft_trans_container_elem(trans);

			nft_trans_elems_remove(&ctx, te);

			if (te->set->ops->commit &&
			    list_empty(&te->set->pending_update)) {
				list_add_tail(&te->set->pending_update,
					      &set_update_list);
			}
			break;
		case NFT_MSG_NEWOBJ:
			if (nft_trans_obj_update(trans)) {
				nft_obj_commit_update(&ctx, trans);
				nf_tables_obj_notify(&ctx,
						     nft_trans_obj(trans),
						     NFT_MSG_NEWOBJ);
			} else {
				nft_clear(net, nft_trans_obj(trans));
				nf_tables_obj_notify(&ctx,
						     nft_trans_obj(trans),
						     NFT_MSG_NEWOBJ);
				nft_trans_destroy(trans);
			}
			break;
		case NFT_MSG_DELOBJ:
		case NFT_MSG_DESTROYOBJ:
			nft_obj_del(nft_trans_obj(trans));
			nf_tables_obj_notify(&ctx, nft_trans_obj(trans),
					     trans->msg_type);
			break;
		case NFT_MSG_NEWFLOWTABLE:
			if (nft_trans_flowtable_update(trans)) {
				nft_trans_flowtable(trans)->data.flags =
					nft_trans_flowtable_flags(trans);
				nf_tables_flowtable_notify(&ctx,
							   nft_trans_flowtable(trans),
							   &nft_trans_flowtable_hooks(trans),
							   NFT_MSG_NEWFLOWTABLE);
				list_splice(&nft_trans_flowtable_hooks(trans),
					    &nft_trans_flowtable(trans)->hook_list);
			} else {
				nft_clear(net, nft_trans_flowtable(trans));
				nf_tables_flowtable_notify(&ctx,
							   nft_trans_flowtable(trans),
							   NULL,
							   NFT_MSG_NEWFLOWTABLE);
			}
			nft_trans_destroy(trans);
			break;
		case NFT_MSG_DELFLOWTABLE:
		case NFT_MSG_DESTROYFLOWTABLE:
			if (nft_trans_flowtable_update(trans)) {
				nf_tables_flowtable_notify(&ctx,
							   nft_trans_flowtable(trans),
							   &nft_trans_flowtable_hooks(trans),
							   trans->msg_type);
				nft_unregister_flowtable_net_hooks(net,
								   nft_trans_flowtable(trans),
								   &nft_trans_flowtable_hooks(trans));
			} else {
				list_del_rcu(&nft_trans_flowtable(trans)->list);
				nf_tables_flowtable_notify(&ctx,
							   nft_trans_flowtable(trans),
							   NULL,
							   trans->msg_type);
				nft_unregister_flowtable_net_hooks(net,
						nft_trans_flowtable(trans),
						&nft_trans_flowtable(trans)->hook_list);
			}
			break;
		}
	}

	nft_set_commit_update(&set_update_list);

	nft_commit_notify(net, NETLINK_CB(skb).portid);
	nf_tables_gen_notify(net, skb, NFT_MSG_NEWGEN);
	nf_tables_commit_audit_log(&adl, nft_net->base_seq);

	nft_gc_seq_end(nft_net, gc_seq);
	nft_net->validate_state = NFT_VALIDATE_SKIP;
	nf_tables_commit_release(net);

	return 0;
}

static void nf_tables_module_autoload(struct net *net)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	struct nft_module_request *req, *next;
	LIST_HEAD(module_list);

	list_splice_init(&nft_net->module_list, &module_list);
	mutex_unlock(&nft_net->commit_mutex);
	list_for_each_entry_safe(req, next, &module_list, list) {
		request_module("%s", req->module);
		req->done = true;
	}
	mutex_lock(&nft_net->commit_mutex);
	list_splice(&module_list, &nft_net->module_list);
}

static void nf_tables_abort_release(struct nft_trans *trans)
{
	struct nft_ctx ctx = { };

	nft_ctx_update(&ctx, trans);

	switch (trans->msg_type) {
	case NFT_MSG_NEWTABLE:
		nf_tables_table_destroy(trans->table);
		break;
	case NFT_MSG_NEWCHAIN:
		if (nft_trans_chain_update(trans))
			nft_hooks_destroy(&nft_trans_chain_hooks(trans));
		else
			nf_tables_chain_destroy(nft_trans_chain(trans));
		break;
	case NFT_MSG_NEWRULE:
		nf_tables_rule_destroy(&ctx, nft_trans_rule(trans));
		break;
	case NFT_MSG_NEWSET:
		nft_set_destroy(&ctx, nft_trans_set(trans));
		break;
	case NFT_MSG_NEWSETELEM:
		nft_trans_set_elem_destroy(&ctx, nft_trans_container_elem(trans));
		break;
	case NFT_MSG_NEWOBJ:
		nft_obj_destroy(&ctx, nft_trans_obj(trans));
		break;
	case NFT_MSG_NEWFLOWTABLE:
		if (nft_trans_flowtable_update(trans))
			nft_hooks_destroy(&nft_trans_flowtable_hooks(trans));
		else
			nf_tables_flowtable_destroy(nft_trans_flowtable(trans));
		break;
	}
	kfree(trans);
}

static void nft_set_abort_update(struct list_head *set_update_list)
{
	struct nft_set *set, *next;

	list_for_each_entry_safe(set, next, set_update_list, pending_update) {
		list_del_init(&set->pending_update);

		if (!set->ops->abort)
			continue;

		set->ops->abort(set);
	}
}

static int __nf_tables_abort(struct net *net, enum nfnl_abort_action action)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	struct nft_trans *trans, *next;
	LIST_HEAD(set_update_list);
	struct nft_trans_elem *te;
	struct nft_ctx ctx = {
		.net = net,
	};
	int err = 0;

	if (action == NFNL_ABORT_VALIDATE &&
	    nf_tables_validate(net) < 0)
		err = -EAGAIN;

	list_for_each_entry_safe_reverse(trans, next, &nft_net->commit_list,
					 list) {
		struct nft_table *table = trans->table;

		nft_ctx_update(&ctx, trans);

		switch (trans->msg_type) {
		case NFT_MSG_NEWTABLE:
			if (nft_trans_table_update(trans)) {
				if (!(table->flags & __NFT_TABLE_F_UPDATE)) {
					nft_trans_destroy(trans);
					break;
				}
				if (table->flags & __NFT_TABLE_F_WAS_DORMANT) {
					nf_tables_table_disable(net, table);
					table->flags |= NFT_TABLE_F_DORMANT;
				} else if (table->flags & __NFT_TABLE_F_WAS_AWAKEN) {
					table->flags &= ~NFT_TABLE_F_DORMANT;
				}
				if (table->flags & __NFT_TABLE_F_WAS_ORPHAN) {
					table->flags &= ~NFT_TABLE_F_OWNER;
					table->nlpid = 0;
				}
				table->flags &= ~__NFT_TABLE_F_UPDATE;
				nft_trans_destroy(trans);
			} else {
				list_del_rcu(&table->list);
			}
			break;
		case NFT_MSG_DELTABLE:
		case NFT_MSG_DESTROYTABLE:
			nft_clear(trans->net, table);
			nft_trans_destroy(trans);
			break;
		case NFT_MSG_NEWCHAIN:
			if (nft_trans_chain_update(trans)) {
				if (!(table->flags & NFT_TABLE_F_DORMANT)) {
					nft_netdev_unregister_hooks(net,
								    &nft_trans_chain_hooks(trans),
								    true);
				}
				free_percpu(nft_trans_chain_stats(trans));
				kfree(nft_trans_chain_name(trans));
				nft_trans_destroy(trans);
			} else {
				if (nft_trans_chain_bound(trans)) {
					nft_trans_destroy(trans);
					break;
				}
				nft_use_dec_restore(&table->use);
				nft_chain_del(nft_trans_chain(trans));
				nf_tables_unregister_hook(trans->net, table,
							  nft_trans_chain(trans));
			}
			break;
		case NFT_MSG_DELCHAIN:
		case NFT_MSG_DESTROYCHAIN:
			if (nft_trans_chain_update(trans)) {
				list_splice(&nft_trans_chain_hooks(trans),
					    &nft_trans_basechain(trans)->hook_list);
			} else {
				nft_use_inc_restore(&table->use);
				nft_clear(trans->net, nft_trans_chain(trans));
			}
			nft_trans_destroy(trans);
			break;
		case NFT_MSG_NEWRULE:
			if (nft_trans_rule_bound(trans)) {
				nft_trans_destroy(trans);
				break;
			}
			nft_use_dec_restore(&nft_trans_rule_chain(trans)->use);
			list_del_rcu(&nft_trans_rule(trans)->list);
			nft_rule_expr_deactivate(&ctx,
						 nft_trans_rule(trans),
						 NFT_TRANS_ABORT);
			if (nft_trans_rule_chain(trans)->flags & NFT_CHAIN_HW_OFFLOAD)
				nft_flow_rule_destroy(nft_trans_flow_rule(trans));
			break;
		case NFT_MSG_DELRULE:
		case NFT_MSG_DESTROYRULE:
			nft_use_inc_restore(&nft_trans_rule_chain(trans)->use);
			nft_clear(trans->net, nft_trans_rule(trans));
			nft_rule_expr_activate(&ctx, nft_trans_rule(trans));
			if (nft_trans_rule_chain(trans)->flags & NFT_CHAIN_HW_OFFLOAD)
				nft_flow_rule_destroy(nft_trans_flow_rule(trans));

			nft_trans_destroy(trans);
			break;
		case NFT_MSG_NEWSET:
			list_del(&nft_trans_container_set(trans)->list_trans_newset);
			if (nft_trans_set_update(trans)) {
				nft_trans_destroy(trans);
				break;
			}
			nft_use_dec_restore(&table->use);
			if (nft_trans_set_bound(trans)) {
				nft_trans_destroy(trans);
				break;
			}
			nft_trans_set(trans)->dead = 1;
			list_del_rcu(&nft_trans_set(trans)->list);
			break;
		case NFT_MSG_DELSET:
		case NFT_MSG_DESTROYSET:
			nft_use_inc_restore(&table->use);
			nft_clear(trans->net, nft_trans_set(trans));
			if (nft_trans_set(trans)->flags & (NFT_SET_MAP | NFT_SET_OBJECT))
				nft_map_activate(&ctx, nft_trans_set(trans));

			nft_trans_destroy(trans);
			break;
		case NFT_MSG_NEWSETELEM:
			if (nft_trans_elem_set_bound(trans)) {
				nft_trans_destroy(trans);
				break;
			}
			te = nft_trans_container_elem(trans);
			if (!nft_trans_elems_new_abort(&ctx, te)) {
				nft_trans_destroy(trans);
				break;
			}

			if (te->set->ops->abort &&
			    list_empty(&te->set->pending_update)) {
				list_add_tail(&te->set->pending_update,
					      &set_update_list);
			}
			break;
		case NFT_MSG_DELSETELEM:
		case NFT_MSG_DESTROYSETELEM:
			te = nft_trans_container_elem(trans);

			nft_trans_elems_destroy_abort(&ctx, te);

			if (te->set->ops->abort &&
			    list_empty(&te->set->pending_update)) {
				list_add_tail(&te->set->pending_update,
					      &set_update_list);
			}
			nft_trans_destroy(trans);
			break;
		case NFT_MSG_NEWOBJ:
			if (nft_trans_obj_update(trans)) {
				nft_obj_destroy(&ctx, nft_trans_obj_newobj(trans));
				nft_trans_destroy(trans);
			} else {
				nft_use_dec_restore(&table->use);
				nft_obj_del(nft_trans_obj(trans));
			}
			break;
		case NFT_MSG_DELOBJ:
		case NFT_MSG_DESTROYOBJ:
			nft_use_inc_restore(&table->use);
			nft_clear(trans->net, nft_trans_obj(trans));
			nft_trans_destroy(trans);
			break;
		case NFT_MSG_NEWFLOWTABLE:
			if (nft_trans_flowtable_update(trans)) {
				nft_unregister_flowtable_net_hooks(net,
						nft_trans_flowtable(trans),
						&nft_trans_flowtable_hooks(trans));
			} else {
				nft_use_dec_restore(&table->use);
				list_del_rcu(&nft_trans_flowtable(trans)->list);
				nft_unregister_flowtable_net_hooks(net,
						nft_trans_flowtable(trans),
						&nft_trans_flowtable(trans)->hook_list);
			}
			break;
		case NFT_MSG_DELFLOWTABLE:
		case NFT_MSG_DESTROYFLOWTABLE:
			if (nft_trans_flowtable_update(trans)) {
				list_splice(&nft_trans_flowtable_hooks(trans),
					    &nft_trans_flowtable(trans)->hook_list);
			} else {
				nft_use_inc_restore(&table->use);
				nft_clear(trans->net, nft_trans_flowtable(trans));
			}
			nft_trans_destroy(trans);
			break;
		}
	}

	WARN_ON_ONCE(!list_empty(&nft_net->commit_set_list));

	nft_set_abort_update(&set_update_list);

	synchronize_rcu();

	list_for_each_entry_safe_reverse(trans, next,
					 &nft_net->commit_list, list) {
		nft_trans_list_del(trans);
		nf_tables_abort_release(trans);
	}

	return err;
}

static int nf_tables_abort(struct net *net, struct sk_buff *skb,
			   enum nfnl_abort_action action)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	unsigned int gc_seq;
	int ret;

	gc_seq = nft_gc_seq_begin(nft_net);
	ret = __nf_tables_abort(net, action);
	nft_gc_seq_end(nft_net, gc_seq);

	WARN_ON_ONCE(!list_empty(&nft_net->commit_list));

	/* module autoload needs to happen after GC sequence update because it
	 * temporarily releases and grabs mutex again.
	 */
	if (action == NFNL_ABORT_AUTOLOAD)
		nf_tables_module_autoload(net);
	else
		nf_tables_module_autoload_cleanup(net);

	mutex_unlock(&nft_net->commit_mutex);

	return ret;
}

static bool nf_tables_valid_genid(struct net *net, u32 genid)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	bool genid_ok;

	mutex_lock(&nft_net->commit_mutex);
	nft_net->tstamp = get_jiffies_64();

	genid_ok = genid == 0 || nft_net->base_seq == genid;
	if (!genid_ok)
		mutex_unlock(&nft_net->commit_mutex);

	/* else, commit mutex has to be released by commit or abort function */
	return genid_ok;
}

static const struct nfnetlink_subsystem nf_tables_subsys = {
	.name		= "nf_tables",
	.subsys_id	= NFNL_SUBSYS_NFTABLES,
	.cb_count	= NFT_MSG_MAX,
	.cb		= nf_tables_cb,
	.commit		= nf_tables_commit,
	.abort		= nf_tables_abort,
	.valid_genid	= nf_tables_valid_genid,
	.owner		= THIS_MODULE,
};

int nft_chain_validate_dependency(const struct nft_chain *chain,
				  enum nft_chain_types type)
{
	const struct nft_base_chain *basechain;

	if (nft_is_base_chain(chain)) {
		basechain = nft_base_chain(chain);
		if (basechain->type->type != type)
			return -EOPNOTSUPP;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(nft_chain_validate_dependency);

int nft_chain_validate_hooks(const struct nft_chain *chain,
			     unsigned int hook_flags)
{
	struct nft_base_chain *basechain;

	if (nft_is_base_chain(chain)) {
		basechain = nft_base_chain(chain);

		if ((1 << basechain->ops.hooknum) & hook_flags)
			return 0;

		return -EOPNOTSUPP;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(nft_chain_validate_hooks);

/**
 *	nft_parse_u32_check - fetch u32 attribute and check for maximum value
 *
 *	@attr: netlink attribute to fetch value from
 *	@max: maximum value to be stored in dest
 *	@dest: pointer to the variable
 *
 *	Parse, check and store a given u32 netlink attribute into variable.
 *	This function returns -ERANGE if the value goes over maximum value.
 *	Otherwise a 0 is returned and the attribute value is stored in the
 *	destination variable.
 */
int nft_parse_u32_check(const struct nlattr *attr, int max, u32 *dest)
{
	u32 val;

	val = ntohl(nla_get_be32(attr));
	if (val > max)
		return -ERANGE;

	*dest = val;
	return 0;
}
EXPORT_SYMBOL_GPL(nft_parse_u32_check);

static int nft_parse_register(const struct nlattr *attr, u32 *preg)
{
	unsigned int reg;

	reg = ntohl(nla_get_be32(attr));
	switch (reg) {
	case NFT_REG_VERDICT...NFT_REG_4:
		*preg = reg * NFT_REG_SIZE / NFT_REG32_SIZE;
		break;
	case NFT_REG32_00...NFT_REG32_15:
		*preg = reg + NFT_REG_SIZE / NFT_REG32_SIZE - NFT_REG32_00;
		break;
	default:
		return -ERANGE;
	}

	return 0;
}

/**
 *	nft_dump_register - dump a register value to a netlink attribute
 *
 *	@skb: socket buffer
 *	@attr: attribute number
 *	@reg: register number
 *
 *	Construct a netlink attribute containing the register number. For
 *	compatibility reasons, register numbers being a multiple of 4 are
 *	translated to the corresponding 128 bit register numbers.
 */
int nft_dump_register(struct sk_buff *skb, unsigned int attr, unsigned int reg)
{
	if (reg % (NFT_REG_SIZE / NFT_REG32_SIZE) == 0)
		reg = reg / (NFT_REG_SIZE / NFT_REG32_SIZE);
	else
		reg = reg - NFT_REG_SIZE / NFT_REG32_SIZE + NFT_REG32_00;

	return nla_put_be32(skb, attr, htonl(reg));
}
EXPORT_SYMBOL_GPL(nft_dump_register);

static int nft_validate_register_load(enum nft_registers reg, unsigned int len)
{
	if (reg < NFT_REG_1 * NFT_REG_SIZE / NFT_REG32_SIZE)
		return -EINVAL;
	if (len == 0)
		return -EINVAL;
	if (reg * NFT_REG32_SIZE + len > sizeof_field(struct nft_regs, data))
		return -ERANGE;

	return 0;
}

int nft_parse_register_load(const struct nft_ctx *ctx,
			    const struct nlattr *attr, u8 *sreg, u32 len)
{
	int err, invalid_reg;
	u32 reg, next_register;

	err = nft_parse_register(attr, &reg);
	if (err < 0)
		return err;

	err = nft_validate_register_load(reg, len);
	if (err < 0)
		return err;

	next_register = DIV_ROUND_UP(len, NFT_REG32_SIZE) + reg;

	/* Can't happen: nft_validate_register_load() should have failed */
	if (WARN_ON_ONCE(next_register > NFT_REG32_NUM))
		return -EINVAL;

	/* find first register that did not see an earlier store. */
	invalid_reg = find_next_zero_bit(ctx->reg_inited, NFT_REG32_NUM, reg);

	/* invalid register within the range that we're loading from? */
	if (invalid_reg < next_register)
		return -ENODATA;

	*sreg = reg;
	return 0;
}
EXPORT_SYMBOL_GPL(nft_parse_register_load);

static void nft_saw_register_store(const struct nft_ctx *__ctx,
				   int reg, unsigned int len)
{
	unsigned int registers = DIV_ROUND_UP(len, NFT_REG32_SIZE);
	struct nft_ctx *ctx = (struct nft_ctx *)__ctx;

	if (WARN_ON_ONCE(len == 0 || reg < 0))
		return;

	bitmap_set(ctx->reg_inited, reg, registers);
}

static int nft_validate_register_store(const struct nft_ctx *ctx,
				       enum nft_registers reg,
				       const struct nft_data *data,
				       enum nft_data_types type,
				       unsigned int len)
{
	int err;

	switch (reg) {
	case NFT_REG_VERDICT:
		if (type != NFT_DATA_VERDICT)
			return -EINVAL;

		if (data != NULL &&
		    (data->verdict.code == NFT_GOTO ||
		     data->verdict.code == NFT_JUMP)) {
			err = nft_chain_validate(ctx, data->verdict.chain);
			if (err < 0)
				return err;
		}

		break;
	default:
		if (type != NFT_DATA_VALUE)
			return -EINVAL;

		if (reg < NFT_REG_1 * NFT_REG_SIZE / NFT_REG32_SIZE)
			return -EINVAL;
		if (len == 0)
			return -EINVAL;
		if (reg * NFT_REG32_SIZE + len >
		    sizeof_field(struct nft_regs, data))
			return -ERANGE;

		break;
	}

	nft_saw_register_store(ctx, reg, len);
	return 0;
}

int nft_parse_register_store(const struct nft_ctx *ctx,
			     const struct nlattr *attr, u8 *dreg,
			     const struct nft_data *data,
			     enum nft_data_types type, unsigned int len)
{
	int err;
	u32 reg;

	err = nft_parse_register(attr, &reg);
	if (err < 0)
		return err;

	err = nft_validate_register_store(ctx, reg, data, type, len);
	if (err < 0)
		return err;

	*dreg = reg;
	return 0;
}
EXPORT_SYMBOL_GPL(nft_parse_register_store);

static const struct nla_policy nft_verdict_policy[NFTA_VERDICT_MAX + 1] = {
	[NFTA_VERDICT_CODE]	= { .type = NLA_U32 },
	[NFTA_VERDICT_CHAIN]	= { .type = NLA_STRING,
				    .len = NFT_CHAIN_MAXNAMELEN - 1 },
	[NFTA_VERDICT_CHAIN_ID]	= { .type = NLA_U32 },
};

static int nft_verdict_init(const struct nft_ctx *ctx, struct nft_data *data,
			    struct nft_data_desc *desc, const struct nlattr *nla)
{
	u8 genmask = nft_genmask_next(ctx->net);
	struct nlattr *tb[NFTA_VERDICT_MAX + 1];
	struct nft_chain *chain;
	int err;

	err = nla_parse_nested_deprecated(tb, NFTA_VERDICT_MAX, nla,
					  nft_verdict_policy, NULL);
	if (err < 0)
		return err;

	if (!tb[NFTA_VERDICT_CODE])
		return -EINVAL;

	/* zero padding hole for memcmp */
	memset(data, 0, sizeof(*data));
	data->verdict.code = ntohl(nla_get_be32(tb[NFTA_VERDICT_CODE]));

	switch (data->verdict.code) {
	case NF_ACCEPT:
	case NF_DROP:
	case NF_QUEUE:
		break;
	case NFT_CONTINUE:
	case NFT_BREAK:
	case NFT_RETURN:
		break;
	case NFT_JUMP:
	case NFT_GOTO:
		if (tb[NFTA_VERDICT_CHAIN]) {
			chain = nft_chain_lookup(ctx->net, ctx->table,
						 tb[NFTA_VERDICT_CHAIN],
						 genmask);
		} else if (tb[NFTA_VERDICT_CHAIN_ID]) {
			chain = nft_chain_lookup_byid(ctx->net, ctx->table,
						      tb[NFTA_VERDICT_CHAIN_ID],
						      genmask);
			if (IS_ERR(chain))
				return PTR_ERR(chain);
		} else {
			return -EINVAL;
		}

		if (IS_ERR(chain))
			return PTR_ERR(chain);
		if (nft_is_base_chain(chain))
			return -EOPNOTSUPP;
		if (nft_chain_is_bound(chain))
			return -EINVAL;
		if (desc->flags & NFT_DATA_DESC_SETELEM &&
		    chain->flags & NFT_CHAIN_BINDING)
			return -EINVAL;
		if (!nft_use_inc(&chain->use))
			return -EMFILE;

		data->verdict.chain = chain;
		break;
	default:
		return -EINVAL;
	}

	desc->len = sizeof(data->verdict);

	return 0;
}

static void nft_verdict_uninit(const struct nft_data *data)
{
	struct nft_chain *chain;

	switch (data->verdict.code) {
	case NFT_JUMP:
	case NFT_GOTO:
		chain = data->verdict.chain;
		nft_use_dec(&chain->use);
		break;
	}
}

int nft_verdict_dump(struct sk_buff *skb, int type, const struct nft_verdict *v)
{
	struct nlattr *nest;

	nest = nla_nest_start_noflag(skb, type);
	if (!nest)
		goto nla_put_failure;

	if (nla_put_be32(skb, NFTA_VERDICT_CODE, htonl(v->code)))
		goto nla_put_failure;

	switch (v->code) {
	case NFT_JUMP:
	case NFT_GOTO:
		if (nla_put_string(skb, NFTA_VERDICT_CHAIN,
				   v->chain->name))
			goto nla_put_failure;
	}
	nla_nest_end(skb, nest);
	return 0;

nla_put_failure:
	return -1;
}

static int nft_value_init(const struct nft_ctx *ctx,
			  struct nft_data *data, struct nft_data_desc *desc,
			  const struct nlattr *nla)
{
	unsigned int len;

	len = nla_len(nla);
	if (len == 0)
		return -EINVAL;
	if (len > desc->size)
		return -EOVERFLOW;
	if (desc->len) {
		if (len != desc->len)
			return -EINVAL;
	} else {
		desc->len = len;
	}

	nla_memcpy(data->data, nla, len);

	return 0;
}

static int nft_value_dump(struct sk_buff *skb, const struct nft_data *data,
			  unsigned int len)
{
	return nla_put(skb, NFTA_DATA_VALUE, len, data->data);
}

static const struct nla_policy nft_data_policy[NFTA_DATA_MAX + 1] = {
	[NFTA_DATA_VALUE]	= { .type = NLA_BINARY },
	[NFTA_DATA_VERDICT]	= { .type = NLA_NESTED },
};

/**
 *	nft_data_init - parse nf_tables data netlink attributes
 *
 *	@ctx: context of the expression using the data
 *	@data: destination struct nft_data
 *	@desc: data description
 *	@nla: netlink attribute containing data
 *
 *	Parse the netlink data attributes and initialize a struct nft_data.
 *	The type and length of data are returned in the data description.
 *
 *	The caller can indicate that it only wants to accept data of type
 *	NFT_DATA_VALUE by passing NULL for the ctx argument.
 */
int nft_data_init(const struct nft_ctx *ctx, struct nft_data *data,
		  struct nft_data_desc *desc, const struct nlattr *nla)
{
	struct nlattr *tb[NFTA_DATA_MAX + 1];
	int err;

	if (WARN_ON_ONCE(!desc->size))
		return -EINVAL;

	err = nla_parse_nested_deprecated(tb, NFTA_DATA_MAX, nla,
					  nft_data_policy, NULL);
	if (err < 0)
		return err;

	if (tb[NFTA_DATA_VALUE]) {
		if (desc->type != NFT_DATA_VALUE)
			return -EINVAL;

		err = nft_value_init(ctx, data, desc, tb[NFTA_DATA_VALUE]);
	} else if (tb[NFTA_DATA_VERDICT] && ctx != NULL) {
		if (desc->type != NFT_DATA_VERDICT)
			return -EINVAL;

		err = nft_verdict_init(ctx, data, desc, tb[NFTA_DATA_VERDICT]);
	} else {
		err = -EINVAL;
	}

	return err;
}
EXPORT_SYMBOL_GPL(nft_data_init);

/**
 *	nft_data_release - release a nft_data item
 *
 *	@data: struct nft_data to release
 *	@type: type of data
 *
 *	Release a nft_data item. NFT_DATA_VALUE types can be silently discarded,
 *	all others need to be released by calling this function.
 */
void nft_data_release(const struct nft_data *data, enum nft_data_types type)
{
	if (type < NFT_DATA_VERDICT)
		return;
	switch (type) {
	case NFT_DATA_VERDICT:
		return nft_verdict_uninit(data);
	default:
		WARN_ON(1);
	}
}
EXPORT_SYMBOL_GPL(nft_data_release);

int nft_data_dump(struct sk_buff *skb, int attr, const struct nft_data *data,
		  enum nft_data_types type, unsigned int len)
{
	struct nlattr *nest;
	int err;

	nest = nla_nest_start_noflag(skb, attr);
	if (nest == NULL)
		return -1;

	switch (type) {
	case NFT_DATA_VALUE:
		err = nft_value_dump(skb, data, len);
		break;
	case NFT_DATA_VERDICT:
		err = nft_verdict_dump(skb, NFTA_DATA_VERDICT, &data->verdict);
		break;
	default:
		err = -EINVAL;
		WARN_ON(1);
	}

	nla_nest_end(skb, nest);
	return err;
}
EXPORT_SYMBOL_GPL(nft_data_dump);

static void __nft_release_hook(struct net *net, struct nft_table *table)
{
	struct nft_flowtable *flowtable;
	struct nft_chain *chain;

	list_for_each_entry(chain, &table->chains, list)
		__nf_tables_unregister_hook(net, table, chain, true);
	list_for_each_entry(flowtable, &table->flowtables, list)
		__nft_unregister_flowtable_net_hooks(net, flowtable,
						     &flowtable->hook_list,
						     true);
}

static void __nft_release_hooks(struct net *net)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	struct nft_table *table;

	list_for_each_entry(table, &nft_net->tables, list) {
		if (nft_table_has_owner(table))
			continue;

		__nft_release_hook(net, table);
	}
}

static void __nft_release_table(struct net *net, struct nft_table *table)
{
	struct nft_flowtable *flowtable, *nf;
	struct nft_chain *chain, *nc;
	struct nft_object *obj, *ne;
	struct nft_rule *rule, *nr;
	struct nft_set *set, *ns;
	struct nft_ctx ctx = {
		.net	= net,
		.family	= NFPROTO_NETDEV,
	};

	ctx.family = table->family;
	ctx.table = table;
	list_for_each_entry(chain, &table->chains, list) {
		if (nft_chain_binding(chain))
			continue;

		ctx.chain = chain;
		list_for_each_entry_safe(rule, nr, &chain->rules, list) {
			list_del(&rule->list);
			nft_use_dec(&chain->use);
			nf_tables_rule_release(&ctx, rule);
		}
	}
	list_for_each_entry_safe(flowtable, nf, &table->flowtables, list) {
		list_del(&flowtable->list);
		nft_use_dec(&table->use);
		nf_tables_flowtable_destroy(flowtable);
	}
	list_for_each_entry_safe(set, ns, &table->sets, list) {
		list_del(&set->list);
		nft_use_dec(&table->use);
		if (set->flags & (NFT_SET_MAP | NFT_SET_OBJECT))
			nft_map_deactivate(&ctx, set);

		nft_set_destroy(&ctx, set);
	}
	list_for_each_entry_safe(obj, ne, &table->objects, list) {
		nft_obj_del(obj);
		nft_use_dec(&table->use);
		nft_obj_destroy(&ctx, obj);
	}
	list_for_each_entry_safe(chain, nc, &table->chains, list) {
		nft_chain_del(chain);
		nft_use_dec(&table->use);
		nf_tables_chain_destroy(chain);
	}
	nf_tables_table_destroy(table);
}

static void __nft_release_tables(struct net *net)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	struct nft_table *table, *nt;

	list_for_each_entry_safe(table, nt, &nft_net->tables, list) {
		if (nft_table_has_owner(table))
			continue;

		list_del(&table->list);

		__nft_release_table(net, table);
	}
}

static int nft_rcv_nl_event(struct notifier_block *this, unsigned long event,
			    void *ptr)
{
	struct nft_table *table, *to_delete[8];
	struct nftables_pernet *nft_net;
	struct netlink_notify *n = ptr;
	struct net *net = n->net;
	unsigned int deleted;
	bool restart = false;
	unsigned int gc_seq;

	if (event != NETLINK_URELEASE || n->protocol != NETLINK_NETFILTER)
		return NOTIFY_DONE;

	nft_net = nft_pernet(net);
	deleted = 0;
	mutex_lock(&nft_net->commit_mutex);

	gc_seq = nft_gc_seq_begin(nft_net);

	nf_tables_trans_destroy_flush_work(net);
again:
	list_for_each_entry(table, &nft_net->tables, list) {
		if (nft_table_has_owner(table) &&
		    n->portid == table->nlpid) {
			if (table->flags & NFT_TABLE_F_PERSIST) {
				table->flags &= ~NFT_TABLE_F_OWNER;
				continue;
			}
			__nft_release_hook(net, table);
			list_del_rcu(&table->list);
			to_delete[deleted++] = table;
			if (deleted >= ARRAY_SIZE(to_delete))
				break;
		}
	}
	if (deleted) {
		restart = deleted >= ARRAY_SIZE(to_delete);
		synchronize_rcu();
		while (deleted)
			__nft_release_table(net, to_delete[--deleted]);

		if (restart)
			goto again;
	}
	nft_gc_seq_end(nft_net, gc_seq);

	mutex_unlock(&nft_net->commit_mutex);

	return NOTIFY_DONE;
}

static struct notifier_block nft_nl_notifier = {
	.notifier_call  = nft_rcv_nl_event,
};

static int __net_init nf_tables_init_net(struct net *net)
{
	struct nftables_pernet *nft_net = nft_pernet(net);

	INIT_LIST_HEAD(&nft_net->tables);
	INIT_LIST_HEAD(&nft_net->commit_list);
	INIT_LIST_HEAD(&nft_net->destroy_list);
	INIT_LIST_HEAD(&nft_net->commit_set_list);
	INIT_LIST_HEAD(&nft_net->binding_list);
	INIT_LIST_HEAD(&nft_net->module_list);
	INIT_LIST_HEAD(&nft_net->notify_list);
	mutex_init(&nft_net->commit_mutex);
	nft_net->base_seq = 1;
	nft_net->gc_seq = 0;
	nft_net->validate_state = NFT_VALIDATE_SKIP;
	INIT_WORK(&nft_net->destroy_work, nf_tables_trans_destroy_work);

	return 0;
}

static void __net_exit nf_tables_pre_exit_net(struct net *net)
{
	struct nftables_pernet *nft_net = nft_pernet(net);

	mutex_lock(&nft_net->commit_mutex);
	__nft_release_hooks(net);
	mutex_unlock(&nft_net->commit_mutex);
}

static void __net_exit nf_tables_exit_net(struct net *net)
{
	struct nftables_pernet *nft_net = nft_pernet(net);
	unsigned int gc_seq;

	mutex_lock(&nft_net->commit_mutex);

	gc_seq = nft_gc_seq_begin(nft_net);

	WARN_ON_ONCE(!list_empty(&nft_net->commit_list));
	WARN_ON_ONCE(!list_empty(&nft_net->commit_set_list));

	if (!list_empty(&nft_net->module_list))
		nf_tables_module_autoload_cleanup(net);

	cancel_work_sync(&nft_net->destroy_work);
	__nft_release_tables(net);

	nft_gc_seq_end(nft_net, gc_seq);

	mutex_unlock(&nft_net->commit_mutex);

	WARN_ON_ONCE(!list_empty(&nft_net->tables));
	WARN_ON_ONCE(!list_empty(&nft_net->module_list));
	WARN_ON_ONCE(!list_empty(&nft_net->notify_list));
	WARN_ON_ONCE(!list_empty(&nft_net->destroy_list));
}

static void nf_tables_exit_batch(struct list_head *net_exit_list)
{
	flush_work(&trans_gc_work);
}

static struct pernet_operations nf_tables_net_ops = {
	.init		= nf_tables_init_net,
	.pre_exit	= nf_tables_pre_exit_net,
	.exit		= nf_tables_exit_net,
	.exit_batch	= nf_tables_exit_batch,
	.id		= &nf_tables_net_id,
	.size		= sizeof(struct nftables_pernet),
};

static int __init nf_tables_module_init(void)
{
	int err;

	BUILD_BUG_ON(offsetof(struct nft_trans_table, nft_trans) != 0);
	BUILD_BUG_ON(offsetof(struct nft_trans_chain, nft_trans_binding.nft_trans) != 0);
	BUILD_BUG_ON(offsetof(struct nft_trans_rule, nft_trans) != 0);
	BUILD_BUG_ON(offsetof(struct nft_trans_set, nft_trans_binding.nft_trans) != 0);
	BUILD_BUG_ON(offsetof(struct nft_trans_elem, nft_trans) != 0);
	BUILD_BUG_ON(offsetof(struct nft_trans_obj, nft_trans) != 0);
	BUILD_BUG_ON(offsetof(struct nft_trans_flowtable, nft_trans) != 0);

	err = register_pernet_subsys(&nf_tables_net_ops);
	if (err < 0)
		return err;

	err = nft_chain_filter_init();
	if (err < 0)
		goto err_chain_filter;

	err = nf_tables_core_module_init();
	if (err < 0)
		goto err_core_module;

	err = register_netdevice_notifier(&nf_tables_flowtable_notifier);
	if (err < 0)
		goto err_netdev_notifier;

	err = rhltable_init(&nft_objname_ht, &nft_objname_ht_params);
	if (err < 0)
		goto err_rht_objname;

	err = nft_offload_init();
	if (err < 0)
		goto err_offload;

	err = netlink_register_notifier(&nft_nl_notifier);
	if (err < 0)
		goto err_netlink_notifier;

	/* must be last */
	err = nfnetlink_subsys_register(&nf_tables_subsys);
	if (err < 0)
		goto err_nfnl_subsys;

	nft_chain_route_init();

	return err;

err_nfnl_subsys:
	netlink_unregister_notifier(&nft_nl_notifier);
err_netlink_notifier:
	nft_offload_exit();
err_offload:
	rhltable_destroy(&nft_objname_ht);
err_rht_objname:
	unregister_netdevice_notifier(&nf_tables_flowtable_notifier);
err_netdev_notifier:
	nf_tables_core_module_exit();
err_core_module:
	nft_chain_filter_fini();
err_chain_filter:
	unregister_pernet_subsys(&nf_tables_net_ops);
	return err;
}

static void __exit nf_tables_module_exit(void)
{
	nfnetlink_subsys_unregister(&nf_tables_subsys);
	netlink_unregister_notifier(&nft_nl_notifier);
	nft_offload_exit();
	unregister_netdevice_notifier(&nf_tables_flowtable_notifier);
	nft_chain_filter_fini();
	nft_chain_route_fini();
	unregister_pernet_subsys(&nf_tables_net_ops);
	cancel_work_sync(&trans_gc_work);
	rcu_barrier();
	rhltable_destroy(&nft_objname_ht);
	nf_tables_core_module_exit();
}

module_init(nf_tables_module_init);
module_exit(nf_tables_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Patrick McHardy <kaber@trash.net>");
MODULE_DESCRIPTION("Framework for packet filtering and classification");
MODULE_ALIAS_NFNL_SUBSYS(NFNL_SUBSYS_NFTABLES);
