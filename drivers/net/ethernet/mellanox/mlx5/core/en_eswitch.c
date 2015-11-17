/*
 * Copyright (c) 2015, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/etherdevice.h>
#include <linux/mlx5/driver.h>
#include <linux/mlx5/mlx5_ifc.h>
#include <linux/mlx5/vport.h>
#include <linux/mlx5/fs.h>
#include "mlx5_core.h"

#define UPLINK_VPORT (0xffffff)
#define MLX5_ESWITCH_DEBUG 1

#ifdef MLX5_ESWITCH_DEBUG

int mlx5_eswitch_debug;
module_param_named(eswitch_debug, mlx5_eswitch_debug, int, 0644);
MODULE_PARM_DESC(eswitch_debug, "Enable E-Switch debug tracing if > 0");

#endif /* MLX5_ESWITCH_DEBUG */

#define esw_info(dev, format, ...)				\
	pr_info("(%s): E-Switch: " format, (dev)->priv.name, ##__VA_ARGS__)

#define esw_warn(dev, format, ...)				\
	pr_warn("(%s): E-Switch: " format, (dev)->priv.name, ##__VA_ARGS__)

#ifdef MLX5_ESWITCH_DEBUG
#define esw_debug(dev, format, ...) ({				\
	if (mlx5_eswitch_debug)					\
		pr_err("(%s):%s:%d " format, (dev)->priv.name,	\
			__func__, __LINE__, ##__VA_ARGS__);	\
	})

#define esw_debug_pk(format, ...) ({				\
	if (mlx5_eswitch_debug)					\
		printk(KERN_INFO "" format, ##__VA_ARGS__);	\
	})
#else
#define esw_debug(dev, format, ...)
#define esw_debug_pk(format, ...)
#endif

#define MLX5_MAX_UC_PER_VPORT(dev) \
	(1 << MLX5_CAP_GEN(dev, log_max_current_uc_list))

#define MLX5_MAX_MC_PER_VPORT(dev) \
	(1 << MLX5_CAP_GEN(dev, log_max_current_mc_list))

enum {
	MLX5_ACTION_NONE = 0,
	MLX5_ACTION_ADD  = 1,
	MLX5_ACTION_DEL  = 2,
};

struct mlx5_l2_addr_node {
	struct hlist_node hlist;
	u8                addr[ETH_ALEN];
};
#define mlx5_for_each_hash_node(hn, tmp, hash, i) \
	for (i = 0; i < MLX5_L2_ADDR_HASH_SIZE; i++) \
		compat_hlist_for_each_entry_safe(hn, tmp, &hash[i], hlist)

#ifndef HAVE_HLIST_FOR_EACH_ENTRY_3_PARAMS
#define COMPAT_HL_NODE struct hlist_node *hlnode;
#else
#define COMPAT_HL_NODE
#endif

#define mlx5_addr_hash_find(hash, mac, type) ({            \
	struct mlx5_l2_addr_node *hn;                      \
	int ix = MLX5_L2_ADDR_HASH(mac);                   \
	bool found = false;                                \
	COMPAT_HL_NODE                                     \
							   \
	compat_hlist_for_each_entry(hn, &hash[ix], hlist)  \
		if (ether_addr_equal(hn->addr, mac)) {     \
			found = true;                      \
			break;                             \
		}                                          \
	if (!found) {                                      \
		hn = NULL;                                 \
	}                                                  \
	(type *)hn;                                        \
})

#define mlx5_addr_hash_add(hash, mac, type, gfp) ({        \
	struct mlx5_l2_addr_node *hn;                      \
	int ix = MLX5_L2_ADDR_HASH(mac);                   \
							   \
	hn = kzalloc(sizeof(type), gfp);                   \
	if (hn) {                                          \
		ether_addr_copy(hn->addr, mac);            \
		hlist_add_head(&hn->hlist, &hash[ix]);     \
	}                                                  \
	(type *)hn;                                        \
})

#define mlx5_addr_hash_del(node) ({                   \
	hlist_del(&(node)->hlist);                    \
	kfree(node);                                  \
})

struct mlx5_esw_uc_addr {
	struct mlx5_l2_addr_node node;
	u32                      table_index;
	u32                      vport;
};

struct mlx5_esw_mc_addr { /* SRIOV only */
	struct mlx5_l2_addr_node  node;
	struct mlx5_flow_rule    *uplink_rule;
	u32                       refcnt;
};

struct mlx5_vport_addr {
	struct mlx5_l2_addr_node node;
	u8                       action;
	u32                      vport;
	struct mlx5_flow_rule   *flow_rule; /* SRIOV only */
};

enum {
	UC_ADDR_CHANGE = BIT(0),
	MC_ADDR_CHANGE = BIT(1),
	VLAN_CHANGE    = BIT(2),
	PROMISC_CHANGE = BIT(3),
	MTU_CHANGE     = BIT(4),
};

/* Vport context events */
#define SRIOV_VPORT_EVENTS (UC_ADDR_CHANGE | \
			    MC_ADDR_CHANGE | \
			    PROMISC_CHANGE)

static int mlx5_arm_vport_context_events(struct mlx5_core_dev *dev, int vport,
					 u32 events_mask)
{
	int in[MLX5_ST_SZ_DW(modify_nic_vport_context_in)];
	int out[MLX5_ST_SZ_DW(modify_nic_vport_context_out)];
	void *nic_vport_ctx;
	int err;

	memset(out, 0, sizeof(out));
	memset(in, 0, sizeof(in));

	MLX5_SET(modify_nic_vport_context_in, in, opcode, MLX5_CMD_OP_MODIFY_NIC_VPORT_CONTEXT);
	MLX5_SET(modify_nic_vport_context_in, in, field_select.change_event, 1);
	MLX5_SET(modify_nic_vport_context_in, in, vport_number, vport);
	if (vport)
		MLX5_SET(modify_nic_vport_context_in, in, other_vport, 1);
	nic_vport_ctx = MLX5_ADDR_OF(modify_nic_vport_context_in, in, nic_vport_context);

	MLX5_SET(nic_vport_context, nic_vport_ctx, arm_change_event, 1);

	if (events_mask & UC_ADDR_CHANGE)
		MLX5_SET(nic_vport_context, nic_vport_ctx, event_on_uc_address_change, 1);
	if (events_mask & MC_ADDR_CHANGE)
		MLX5_SET(nic_vport_context, nic_vport_ctx, event_on_mc_address_change, 1);

	err = mlx5_cmd_exec(dev, in, sizeof(in), out, sizeof(out));
	if (err)
		goto ex;
	err = mlx5_cmd_status_to_err_v2(out);
	if (err)
		goto ex;
	return 0;
ex:
	return err;
}

/* L2 table management */

static int mlx5_set_l2_table_entry_cmd(struct mlx5_core_dev *dev, u32 index,
				       u8 *mac, u8 vlan_valid, u16 vlan)
{
	u32 in[MLX5_ST_SZ_DW(set_l2_table_entry_in)];
	u32 out[MLX5_ST_SZ_DW(set_l2_table_entry_out)];
	u8 *in_mac_addr;

	memset(in, 0, sizeof(in));
	memset(out, 0, sizeof(out));

	MLX5_SET(set_l2_table_entry_in, in, opcode,
		 MLX5_CMD_OP_SET_L2_TABLE_ENTRY);
	MLX5_SET(set_l2_table_entry_in, in, table_index, index);
	MLX5_SET(set_l2_table_entry_in, in, vlan_valid, vlan_valid);
	MLX5_SET(set_l2_table_entry_in, in, vlan, vlan);

	in_mac_addr = MLX5_ADDR_OF(set_l2_table_entry_in, in, mac_address);
	ether_addr_copy(&in_mac_addr[2], mac);

	return mlx5_cmd_exec_check_status(dev, in, sizeof(in),
					  out, sizeof(out));
}

static int mlx5_del_l2_table_entry_cmd(struct mlx5_core_dev *dev, u32 index)
{
	u32 in[MLX5_ST_SZ_DW(delete_l2_table_entry_in)];
	u32 out[MLX5_ST_SZ_DW(delete_l2_table_entry_out)];

	memset(in, 0, sizeof(in));
	memset(out, 0, sizeof(out));

	MLX5_SET(delete_l2_table_entry_in, in, opcode,
		 MLX5_CMD_OP_DELETE_L2_TABLE_ENTRY);
	MLX5_SET(delete_l2_table_entry_in, in, table_index, index);
	return mlx5_cmd_exec_check_status(dev, in, sizeof(in),
					  out, sizeof(out));
}

static int alloc_l2_table_index(struct mlx5_l2_table *l2_table, u32 *ix)
{
	int err = 0;

	*ix = find_first_zero_bit(l2_table->bitmap, l2_table->size);
	if (*ix >= l2_table->size)
		err = -ENOSPC;
	else
		__set_bit(*ix, l2_table->bitmap);

	return err;
}

static void free_l2_table_index(struct mlx5_l2_table *l2_table, u32 ix)
{
	__clear_bit(ix, l2_table->bitmap);
}

static int mlx5_set_l2_table_entry(struct mlx5_core_dev *dev, u8 *mac,
				   u8 vlan_valid, u16 vlan,
				   u32 *index)
{
	struct mlx5_l2_table *l2_table = &dev->priv.eswitch.l2_table;
	int err;

	err = alloc_l2_table_index(l2_table, index);
	if (err)
		return err;

	err = mlx5_set_l2_table_entry_cmd(dev, *index, mac, vlan_valid, vlan);
	if (err)
		free_l2_table_index(l2_table, *index);

	return err;
}

static void mlx5_del_l2_table_entry(struct mlx5_core_dev *dev, u32 index)
{
	struct mlx5_l2_table *l2_table = &dev->priv.eswitch.l2_table;

	mlx5_del_l2_table_entry_cmd(dev, index);
	free_l2_table_index(l2_table, index);
}

/* E-Switch FDB management */

enum fdb_rule_type {
	FDB_FULL_MATCH = 0,
	FDB_MC_PROMISC = 1,
	FDB_PROMISC = 2,
};

static int mlx5_query_esw_vport_context(struct mlx5_core_dev *mdev, u32 vport,
					u32 *out, int outlen)
{
	u32 in[MLX5_ST_SZ_DW(query_esw_vport_context_in)];

	memset(in, 0, sizeof(in));

	MLX5_SET(query_nic_vport_context_in, in, opcode,
		 MLX5_CMD_OP_QUERY_ESW_VPORT_CONTEXT);

	MLX5_SET(query_esw_vport_context_in, in, vport_number, vport);
	if (vport)
		MLX5_SET(query_esw_vport_context_in, in, other_vport, 1);

	return mlx5_cmd_exec_check_status(mdev, in, sizeof(in), out, outlen);
}

int mlx5_query_esw_vport_cvlan(struct mlx5_core_dev *dev, u32 vport,
			       u16 *vlan, u8 *qos)
{
	u32 out[MLX5_ST_SZ_DW(query_esw_vport_context_out)];
	int err;
	bool cvlan_strip;
	bool cvlan_insert;

	memset(out, 0, sizeof(out));

	*vlan = 0;
	*qos = 0;

	if (!MLX5_CAP_ESW(dev, vport_cvlan_strip) ||
	    !MLX5_CAP_ESW(dev, vport_cvlan_insert_if_not_exist))
		return -ENOTSUPP;

	err = mlx5_query_esw_vport_context(dev, vport, out, sizeof(out));
	if (err)
		goto out;

	cvlan_strip = MLX5_GET(query_esw_vport_context_out, out,
			       esw_vport_context.vport_cvlan_strip);

	cvlan_insert = MLX5_GET(query_esw_vport_context_out, out,
				esw_vport_context.vport_cvlan_insert);

	if (cvlan_strip || cvlan_insert) {
		*vlan = MLX5_GET(query_esw_vport_context_out, out,
				 esw_vport_context.cvlan_id);
		*qos = MLX5_GET(query_esw_vport_context_out, out,
				esw_vport_context.cvlan_pcp);
	}

	esw_debug(dev, "Query Vport[%d] cvlan: VLAN %d qos=%d\n",
		  vport, *vlan, *qos);
out:
	return err;
}

int mlx5_modify_esw_vport_context(struct mlx5_core_dev *dev, u16 vport,
				  void *in, int inlen)
{
	u32 out[MLX5_ST_SZ_DW(modify_esw_vport_context_out)];

	memset(out, 0, sizeof(out));

	MLX5_SET(modify_esw_vport_context_in, in, vport_number, vport);
	if (vport)
		MLX5_SET(modify_esw_vport_context_in, in, other_vport, 1);

	MLX5_SET(modify_esw_vport_context_in, in, opcode,
		 MLX5_CMD_OP_MODIFY_ESW_VPORT_CONTEXT);

	return mlx5_cmd_exec_check_status(dev, in, inlen,
					  out, sizeof(out));
}

int mlx5_modify_esw_vport_cvlan(struct mlx5_core_dev *dev, u32 vport,
				u16 vlan, u8 qos, bool set)
{
	u32 in[MLX5_ST_SZ_DW(modify_esw_vport_context_in)];

	memset(in, 0, sizeof(in));

	if (!MLX5_CAP_ESW(dev, vport_cvlan_strip) ||
	    !MLX5_CAP_ESW(dev, vport_cvlan_insert_if_not_exist))
		return -ENOTSUPP;

	esw_debug(dev, "Set Vport[%d] VLAN %d qos %d set=%d\n",
		  vport, vlan, qos, set);

	if (set) {
		MLX5_SET(modify_esw_vport_context_in, in,
			 esw_vport_context.vport_cvlan_strip, 1);
		/* insert only if no vlan in packet */
		MLX5_SET(modify_esw_vport_context_in, in,
			 esw_vport_context.vport_cvlan_insert, 1);
		MLX5_SET(modify_esw_vport_context_in, in,
			 esw_vport_context.cvlan_pcp, qos);
		MLX5_SET(modify_esw_vport_context_in, in,
			 esw_vport_context.cvlan_id, vlan);
	}

	MLX5_SET(modify_esw_vport_context_in, in,
		 field_select.vport_cvlan_strip, 1);
	MLX5_SET(modify_esw_vport_context_in, in,
		 field_select.vport_cvlan_insert, 1);

	return mlx5_modify_esw_vport_context(dev, vport, in, sizeof(in));
}

struct mlx5_flow_rule *mlx5_fdb_add_vport_rule(struct mlx5_flow_table *fdb,
					       u8 mac[ETH_ALEN],
					       u32 vport,
					       enum fdb_rule_type frt)
{
	int match_header = MLX5_MATCH_OUTER_HEADERS;
	struct mlx5_flow_destination dest;
	struct mlx5_flow_rule *flow_rule = NULL;
	u32 *match_v;
	u32 *match_c;
	u8 *dmac_v;
	u8 *dmac_c;

	match_v = kzalloc(MLX5_ST_SZ_BYTES(fte_match_param), GFP_KERNEL);
	match_c = kzalloc(MLX5_ST_SZ_BYTES(fte_match_param), GFP_KERNEL);
	if (!match_v || !match_v) {
		pr_warn("FDB: Failed to alloc match parameters\n");
		goto out;
	}
	dmac_v = MLX5_ADDR_OF(fte_match_param, match_v,
			      outer_headers.dmac_47_16);
	dmac_c = MLX5_ADDR_OF(fte_match_param, match_c,
			      outer_headers.dmac_47_16);

	switch (frt) {
	case FDB_FULL_MATCH:
		ether_addr_copy(dmac_v, mac);
		memset(dmac_c, 0xff, ETH_ALEN);
		break;

	case FDB_MC_PROMISC:
		dmac_v[0] = 0x01;
		dmac_c[0] = 0x01;
		break;

	case FDB_PROMISC: /* fallthrough */
	default:
		match_header = 0;
		break;
	}

	dest.type = MLX5_FLOW_DESTINATION_TYPE_VPORT;
	dest.vport_num = vport;

	esw_debug_pk("\tFDB add rule dmac_v(%pM) dmac_c(%pM) -> vport(%d)\n",
		     dmac_v, dmac_c, vport);
	flow_rule =
		mlx5_add_flow_rule(fdb,
				   match_header,
				   match_c,
				   match_v,
				   MLX5_FLOW_CONTEXT_ACTION_FWD_DEST,
				   0, &dest);
	if (IS_ERR_OR_NULL(flow_rule)) {
		pr_warn(
			"FDB: Failed to add flow rule: dmac_v(%pM) dmac_c(%pM) -> vport(%d), err(%ld)\n",
			 dmac_v, dmac_c, vport, PTR_ERR(flow_rule));
		flow_rule = NULL;
	}
out:
	kfree(match_v);
	kfree(match_c);
	return flow_rule;
}

static void mlx5_eswfdb_init_promisc_rules(struct mlx5_core_dev *dev)
{
	struct mlx5_eswitch *esw = &dev->priv.eswitch;
	struct mlx5_esw_mc_addr *promisc;
	struct mlx5_esw_mc_addr *mc_promisc;

	promisc = kzalloc(sizeof(*promisc), GFP_KERNEL);
	if (!promisc) {
		esw_warn(dev, "Eswitch FDB: Failed to allocate promisc addr\n");
		return;
	}

	promisc->uplink_rule = mlx5_fdb_add_vport_rule(esw->fdb_table.fdb,
						       promisc->node.addr,
						       UPLINK_VPORT,
						       FDB_PROMISC);
	esw->promisc = promisc;

	mc_promisc = kzalloc(sizeof(*mc_promisc), GFP_KERNEL);
	if (!mc_promisc) {
		esw_warn(dev, "Eswitch FDB: Failed to allocate MC promisc addr\n");
		return;
	}

	mc_promisc->node.addr[0] = 0x01;
	mc_promisc->uplink_rule =
		mlx5_fdb_add_vport_rule(esw->fdb_table.fdb,
					mc_promisc->node.addr,
					UPLINK_VPORT,
					FDB_MC_PROMISC);
	esw->mc_promisc = mc_promisc;
}

static void mlx5_eswfdb_cleanup_promisc_rules(struct mlx5_core_dev *dev)
{
	struct mlx5_eswitch *esw = &dev->priv.eswitch;
	struct mlx5_esw_mc_addr *promisc = esw->promisc;
	struct mlx5_esw_mc_addr *mc_promisc = esw->mc_promisc;

	if (promisc && promisc->uplink_rule)
		mlx5_del_flow_rule(promisc->uplink_rule);

	if (mc_promisc && mc_promisc->uplink_rule)
		mlx5_del_flow_rule(mc_promisc->uplink_rule);

	kfree(promisc);
	kfree(mc_promisc);

	esw->promisc = NULL;
	esw->mc_promisc = NULL;
}

static int mlx5_esw_create_fdb_table(struct mlx5_core_dev *dev, int nvports)
{
	int max_fdb_size = 1 << MLX5_CAP_ESW_FLOWTABLE_FDB(dev, log_max_ft_size);
	struct mlx5_eswitch *esw = &dev->priv.eswitch;
	struct mlx5_flow_group *mc_promisc_grp;
	struct mlx5_flow_namespace *root_ns;
	struct mlx5_flow_group *promisc_grp;
	struct mlx5_flow_group *addr_grp;
	struct mlx5_flow_table *fdb;
	void *match_criteria;
	int table_size;
	u32 *grp_in;
	u8 *dmac;
	int err;

	max_fdb_size -= 2; /* FW reserve */
	table_size = MLX5_MAX_UC_PER_VPORT(dev);
	table_size += MLX5_MAX_MC_PER_VPORT(dev);
	table_size *= nvports;
	table_size += 1; /* TODO: reserve 1 as a FW W/A for modify Rule */
	table_size += 4; /* 2 Promisc groups reserve 1 per each */
	table_size = min(table_size, max_fdb_size - 2);

	esw_debug(dev, "Create FDB talbe size(%d) max_size(%d)\n",
		  table_size, max_fdb_size - 2);
	/* TODO: Don't allow vports to get more than they deserve */
	root_ns = mlx5_get_flow_namespace(dev, MLX5_FLOW_NAMESPACE_FDB);
	if (!root_ns) {
		esw_warn(dev, "Failed to get FDB flow namespace\n");
		return -ENOMEM;
	}

	fdb = mlx5_create_flow_table(root_ns, 0, "eswitch-fdb", table_size);
	if (IS_ERR_OR_NULL(fdb)) {
		esw_warn(dev, "Failed to create FDB flow table err(%ld)\n",
			 PTR_ERR(fdb));
		return PTR_ERR(fdb);
	}

	grp_in = kzalloc(MLX5_ST_SZ_BYTES(create_flow_group_in), GFP_KERNEL);
	if (!grp_in) {
		esw_warn(dev, "Failed to alloc group inbox\n");
		err = -ENOMEM;
		goto err_fdb;
	}

	MLX5_SET(create_flow_group_in, grp_in, match_criteria_enable,
		 MLX5_MATCH_OUTER_HEADERS);
	match_criteria = MLX5_ADDR_OF(create_flow_group_in, grp_in,
				      match_criteria);
	dmac = MLX5_ADDR_OF(fte_match_param, match_criteria,
			    outer_headers.dmac_47_16);

	/* MC/UC ADDR Group */
	MLX5_SET(create_flow_group_in, grp_in, start_flow_index, 0);
	MLX5_SET(create_flow_group_in, grp_in, end_flow_index, table_size - 5);
	memset(dmac, 0xff, ETH_ALEN);

	addr_grp = mlx5_create_flow_group(fdb, grp_in);
	if (IS_ERR_OR_NULL(addr_grp)) {
		esw_warn(dev, "Failed to create uc/mc addr group err(%ld)\n",
			 PTR_ERR(addr_grp));
		err = PTR_ERR(addr_grp);
		goto err_fdb;
	}

	/* MC promisc Group */
	MLX5_SET(create_flow_group_in, grp_in,
		 start_flow_index, table_size - 4);
	MLX5_SET(create_flow_group_in, grp_in,
		 end_flow_index, table_size - 3);
	memset(dmac, 0, ETH_ALEN);
	dmac[0] = 0x01;

	mc_promisc_grp = mlx5_create_flow_group(fdb, grp_in);
	if (IS_ERR_OR_NULL(mc_promisc_grp)) {
		esw_warn(dev, "Failed to create FDB MC promisc group err(%ld)\n",
			 PTR_ERR(mc_promisc_grp));
		err = PTR_ERR(mc_promisc_grp);
		goto err_addr_grp;
	}

	/* Promisc Group */
	MLX5_SET(create_flow_group_in, grp_in, match_criteria_enable, 0);
	MLX5_SET(create_flow_group_in, grp_in,
		 start_flow_index, table_size - 2);
	MLX5_SET(create_flow_group_in, grp_in,
		 end_flow_index, table_size - 1);
	memset(dmac, 0, ETH_ALEN);

	promisc_grp = mlx5_create_flow_group(fdb, grp_in);
	if (IS_ERR_OR_NULL(promisc_grp)) {
		esw_warn(dev, "Failed to create FDB promisc group err(%ld)\n",
			 PTR_ERR(promisc_grp));
		err = PTR_ERR(promisc_grp);
		goto err_mc_promisc_grp;
	}
	kfree(grp_in);

	esw->fdb_table.fdb = fdb;
	esw->fdb_table.addr_grp = addr_grp;
	esw->fdb_table.mc_promisc_grp = mc_promisc_grp;
	esw->fdb_table.promisc_grp = promisc_grp;

	return 0;

err_mc_promisc_grp:
	mlx5_destroy_flow_group(mc_promisc_grp);
err_addr_grp:
	mlx5_destroy_flow_group(addr_grp);
err_fdb:
	mlx5_destroy_flow_table(fdb);
	kfree(grp_in);
	return err;
}

static void mlx5_esw_destroy_fdb_table(struct mlx5_core_dev *dev)
{
	struct mlx5_eswitch *esw = &dev->priv.eswitch;

	if (!esw->fdb_table.fdb)
		return;

	esw_debug(dev, "Destroy FDB Table fdb(%p)\n",
		  esw->fdb_table.fdb);

	mlx5_destroy_flow_group(esw->fdb_table.promisc_grp);
	mlx5_destroy_flow_group(esw->fdb_table.mc_promisc_grp);
	mlx5_destroy_flow_group(esw->fdb_table.addr_grp);
	mlx5_destroy_flow_table(esw->fdb_table.fdb);
	esw->fdb_table.fdb = NULL;
}

/* VPORT UC/MC address management */
typedef int (*vport_addr_action)(struct mlx5_core_dev *dev,
				 struct mlx5_vport_addr *vaddr);

static int mlx5_esw_add_uc_addr(struct mlx5_core_dev *dev,
				struct mlx5_vport_addr *vaddr)
{
	struct mlx5_flow_table *fdb = dev->priv.eswitch.fdb_table.fdb;
	struct hlist_head *hash = dev->priv.eswitch.l2_table.l2_hash;
	struct mlx5_esw_uc_addr *esw_uc_addr;
	u8 *mac = vaddr->node.addr;
	u32 vport = vaddr->vport;
	int err;

	esw_uc_addr = mlx5_addr_hash_find(hash, mac, struct mlx5_esw_uc_addr);
	if (esw_uc_addr) {
		esw_warn(dev,
			 "Failed to set L2 mac(%pM) for vport(%d), mac is already in use by vport(%d)\n",
			 mac, vport, esw_uc_addr->vport);
		return -EEXIST;
	}

	esw_uc_addr = mlx5_addr_hash_add(hash, mac, struct mlx5_esw_uc_addr,
					 GFP_KERNEL);
	if (!esw_uc_addr)
		return -ENOMEM;

	esw_uc_addr->vport = vport;

	err = mlx5_set_l2_table_entry(dev, mac, 0, 0,
				      &esw_uc_addr->table_index);
	if (err)
		goto abort;

	if (fdb) /* SRIOV is enabled */
		vaddr->flow_rule = mlx5_fdb_add_vport_rule(fdb, mac, vport,
							   FDB_FULL_MATCH);

	esw_debug(dev, "UC+ Added UC addr mac(%pM) vport(%d) l2_index(%d) flow_rule(%p)\n",
		  mac, vport, esw_uc_addr->table_index, vaddr->flow_rule);
	return err;
abort:
	mlx5_addr_hash_del(&esw_uc_addr->node);
	return err;
}

static int mlx5_esw_del_uc_addr(struct mlx5_core_dev *dev,
				struct mlx5_vport_addr *vaddr)
{
	struct hlist_head *hash = dev->priv.eswitch.l2_table.l2_hash;
	struct mlx5_esw_uc_addr *esw_uc_addr;
	u8 *mac = vaddr->node.addr;
	u32 vport = vaddr->vport;

	esw_uc_addr = mlx5_addr_hash_find(hash, mac, struct mlx5_esw_uc_addr);
	if (!esw_uc_addr || esw_uc_addr->vport != vport) {
		esw_debug(dev,
			  "MAC(%pM) doesn't belong to vport (%d)\n",
			  mac, vport);
		return -EINVAL;
	}

	mlx5_del_l2_table_entry(dev, esw_uc_addr->table_index);

	esw_debug(dev,
		  "UC- Deleted mac(%pM) vport(%d) l2_index(%d) flow_rule(%p)\n",
		  mac, vport, esw_uc_addr->table_index, vaddr->flow_rule);

	if (vaddr->flow_rule)
		mlx5_del_flow_rule(vaddr->flow_rule);
	vaddr->flow_rule = NULL;

	mlx5_addr_hash_del(&esw_uc_addr->node);
	return 0;
}

static int mlx5_esw_add_mc_addr(struct mlx5_core_dev *dev,
				struct mlx5_vport_addr *vaddr)
{
	struct mlx5_eswitch *esw = &dev->priv.eswitch;
	struct mlx5_flow_table *fdb = esw->fdb_table.fdb;
	struct hlist_head *hash = esw->mc_table;
	struct mlx5_esw_mc_addr *esw_mc_addr;
	u8 *mac = vaddr->node.addr;
	u32 vport = vaddr->vport;

	if (!fdb)
		return 0;

	esw_mc_addr = mlx5_addr_hash_find(hash, mac, struct mlx5_esw_mc_addr);
	if (esw_mc_addr)
		goto add;

	esw_mc_addr = mlx5_addr_hash_add(hash, mac, struct mlx5_esw_mc_addr,
					 GFP_KERNEL);
	if (!esw_mc_addr)
		return -ENOMEM;

	esw_mc_addr->uplink_rule =
		mlx5_fdb_add_vport_rule(fdb, mac, UPLINK_VPORT,
					FDB_FULL_MATCH);
	esw_debug(dev,
		  "MC+ Added mac(%pM) vport(%d) flow_rule(%p)\n",
		  mac, UPLINK_VPORT, esw_mc_addr->uplink_rule);
add:
	esw_mc_addr->refcnt++;
	vaddr->flow_rule = mlx5_fdb_add_vport_rule(fdb, mac, vport,
						   FDB_FULL_MATCH);
	esw_debug(dev,
		  "MC+ Added mac(%pM) vport(%d) flow_rule(%p) refcnt(%d)\n",
		  mac, vport, vaddr->flow_rule, esw_mc_addr->refcnt);
	return 0;
}

static int mlx5_esw_del_mc_addr(struct mlx5_core_dev *dev,
				struct mlx5_vport_addr *vaddr)
{
	struct mlx5_eswitch *esw = &dev->priv.eswitch;
	struct hlist_head *hash = esw->mc_table;
	struct mlx5_esw_mc_addr *esw_mc_addr;
	u8 *mac = vaddr->node.addr;
	u32 vport = vaddr->vport;

	if (!esw->fdb_table.fdb)
		return 0;

	esw_mc_addr = mlx5_addr_hash_find(hash, mac, struct mlx5_esw_mc_addr);
	if (!esw_mc_addr) {
		esw_warn(dev,
			 "Failed to find eswitch MC addr for MAC(%pM) vport(%d)",
			 mac, vport);
		return -EINVAL;
	}

	esw_debug(dev,
		  "MC- Deleting MC mac(%pM) -> vport(%d) flow_rule(%p) refcnt(%d)\n",
		  mac, vport, vaddr->flow_rule,  esw_mc_addr->refcnt);
	if (vaddr->flow_rule)
		mlx5_del_flow_rule(vaddr->flow_rule);
	vaddr->flow_rule = NULL;

	if (--esw_mc_addr->refcnt)
		return 0;

	esw_debug(dev,
		  "MC- Deleting MC mac(%pM) -> vport(%d) flow_rule(%p) refcnt(%d)\n",
		  mac, UPLINK_VPORT, esw_mc_addr->uplink_rule,
		  esw_mc_addr->refcnt);
	if (esw_mc_addr->uplink_rule)
		mlx5_del_flow_rule(esw_mc_addr->uplink_rule);

	mlx5_addr_hash_del(&esw_mc_addr->node);
	return 0;
}

static void mlx5_esw_apply_vport_addr_list(struct mlx5_core_dev *dev,
					   u32 vport_num, int list_type)
{
	struct mlx5_vport *vport =
		&dev->priv.eswitch.vports[vport_num];
	bool is_uc = list_type == MLX5_NVPRT_LIST_TYPE_UC;
	struct mlx5_l2_addr_node *node;
	struct mlx5_vport_addr *addr;
	struct hlist_head *hash;
#ifndef HAVE_HLIST_FOR_EACH_ENTRY_3_PARAMS
	struct hlist_node *hlnode;
#endif
	struct hlist_node *tmp;
	vport_addr_action vport_addr_add;
	vport_addr_action vport_addr_del;
	int hi;

	vport_addr_add = is_uc ? mlx5_esw_add_uc_addr :
				 mlx5_esw_add_mc_addr;
	vport_addr_del = is_uc ? mlx5_esw_del_uc_addr :
				 mlx5_esw_del_mc_addr;

	hash = is_uc ? vport->uc_list : vport->mc_list;
	mlx5_for_each_hash_node(node, tmp, hash, hi) {
		addr = container_of(node, struct mlx5_vport_addr, node);
		switch (addr->action) {
		case MLX5_ACTION_ADD:
			vport_addr_add(dev, addr);
			addr->action = MLX5_ACTION_NONE;
			break;
		case MLX5_ACTION_DEL:
			vport_addr_del(dev, addr);
			mlx5_addr_hash_del(&addr->node);
			break;
		}
	}
}

static void mlx5_esw_update_vport_addr_list(struct mlx5_core_dev *dev,
					    u32 vport_num, int list_type)
{
	struct mlx5_vport *vport = &dev->priv.eswitch.vports[vport_num];
	bool is_uc = list_type == MLX5_NVPRT_LIST_TYPE_UC;
	struct mlx5_l2_addr_node *node;
	struct mlx5_vport_addr *addr;
	struct hlist_head *hash;
#ifndef HAVE_HLIST_FOR_EACH_ENTRY_3_PARAMS
	struct hlist_node *hlnode;
#endif
	struct hlist_node *tmp;
	u8 (*mac_list)[ETH_ALEN];
	int list_size;
	int i;
	int hi;

	list_size = is_uc ? MLX5_MAX_UC_PER_VPORT(dev) :
			    MLX5_MAX_MC_PER_VPORT(dev);

	mac_list = kcalloc(list_size, ETH_ALEN, GFP_KERNEL);
	if (!mac_list) {
		esw_warn(dev, "Failed to allocate uc mac list\n");
		return;
	}

	hash = is_uc ? vport->uc_list : vport->mc_list;

	mlx5_for_each_hash_node(node, tmp, hash, hi) {
		addr = container_of(node, struct mlx5_vport_addr, node);
		addr->action = MLX5_ACTION_DEL;
	}

	mlx5_query_nic_vport_mac_list(dev, vport_num, list_type,
				      mac_list, &list_size);

	for (i = 0; i < list_size; i++) {
		if (is_uc && !is_valid_ether_addr(mac_list[i]))
			continue;

		if (!is_uc && !is_multicast_ether_addr(mac_list[i]))
			continue;

		addr = mlx5_addr_hash_find(hash, mac_list[i],
					   struct mlx5_vport_addr);
		if (addr) {
			addr->action = MLX5_ACTION_NONE;
			continue;
		}

		addr = mlx5_addr_hash_add(hash, mac_list[i],
					  struct mlx5_vport_addr,
					  GFP_KERNEL);
		if (!addr) {
			esw_warn(dev, "Failed to add MAC(%pM) to vport[%d] hash\n",
				 mac_list[i], vport_num);
			continue;
		}
		addr->vport = vport_num;
		addr->action = MLX5_ACTION_ADD;
	}
	kfree(mac_list);
}

static void mlx5_esw_update_vport_promsic(struct mlx5_core_dev *dev,
					  u32 vport_num)
{
	struct mlx5_vport *vport = &dev->priv.eswitch.vports[vport_num];
	struct mlx5_flow_table *fdb = dev->priv.eswitch.fdb_table.fdb;
	u8 promisc_all_dmac[ETH_ALEN] = {0};
	u8 promisc_mc_dmac[ETH_ALEN] = {0};
	int promisc_mc;
	int promisc_uc;
	int promisc_all;

	if (!fdb)
		return;

	mlx5_query_nic_vport_promisc(dev, vport->vport,
				     &promisc_uc, &promisc_mc, &promisc_all);

	promisc_mc_dmac[0] = 0x01;

	if (promisc_mc && !vport->mc_promisc)
		vport->mc_promisc =
			mlx5_fdb_add_vport_rule(fdb, promisc_mc_dmac,
						vport->vport, FDB_MC_PROMISC);
	if (!promisc_mc && vport->mc_promisc) {
		mlx5_del_flow_rule(vport->mc_promisc);
		vport->mc_promisc = NULL;
	}

	if (promisc_all && !vport->promisc)
		vport->promisc =
			mlx5_fdb_add_vport_rule(fdb, promisc_all_dmac,
						vport->vport, FDB_PROMISC);
	if (!promisc_all && vport->promisc) {
		mlx5_del_flow_rule(vport->promisc);
		vport->promisc = NULL;
	}

	esw_debug(dev,
		  "vport[%d] Prmisc: promisc_uc(%d), promisc_mc(%d), promisc_all(%d)\n",
		  vport->vport, promisc_uc, promisc_mc, promisc_all);
	esw_debug(dev,
		  "\t\t Current: vport->mc_promisc(%p) vport->promisc(%p)\n",
		  vport->mc_promisc, vport->promisc);
}

static void mlx5_esw_cleanup_vport(struct mlx5_core_dev *dev,
				   u32 vport_num)
{
	struct mlx5_vport *vport = &dev->priv.eswitch.vports[vport_num];
	struct mlx5_l2_addr_node *node;
	struct mlx5_vport_addr *addr;
#ifndef HAVE_HLIST_FOR_EACH_ENTRY_3_PARAMS
	struct hlist_node *hlnode;
#endif
	struct hlist_node *tmp;
	int hi;

	mlx5_for_each_hash_node(node, tmp, vport->uc_list, hi) {
		addr = container_of(node, struct mlx5_vport_addr, node);
		addr->action = MLX5_ACTION_DEL;
	}
	mlx5_esw_apply_vport_addr_list(dev, vport_num,
				       MLX5_NVPRT_LIST_TYPE_UC);

	mlx5_for_each_hash_node(node, tmp, vport->mc_list, hi) {
		addr = container_of(node, struct mlx5_vport_addr, node);
		addr->action = MLX5_ACTION_DEL;
	}
	mlx5_esw_apply_vport_addr_list(dev, vport_num,
				       MLX5_NVPRT_LIST_TYPE_MC);

	if (vport->promisc) {
		mlx5_del_flow_rule(vport->promisc);
		vport->promisc = NULL;
	}

	if (vport->mc_promisc) {
		mlx5_del_flow_rule(vport->mc_promisc);
		vport->mc_promisc = NULL;
	}
}

static void mlx5_eswitch_vport_change_handler(struct work_struct *work)
{
	struct mlx5_vport *vport =
		container_of(work, struct mlx5_vport, vport_change_handler);
	struct mlx5_core_dev *dev = vport->dev;
	u8 mac[ETH_ALEN];

	esw_debug(dev, "vport[%d] EVENT START perm mac: %pM\n",
		  vport->vport, mac);
	mlx5_query_nic_vport_mac_address(dev, vport->vport, mac);
	esw_debug(dev, "vport[%d] perm mac: %pM\n", vport->vport, mac);

	if (vport->enabled_events & UC_ADDR_CHANGE) {
		mlx5_esw_update_vport_addr_list(dev, vport->vport,
						MLX5_NVPRT_LIST_TYPE_UC);
		mlx5_esw_apply_vport_addr_list(dev, vport->vport,
					       MLX5_NVPRT_LIST_TYPE_UC);
	}

	if (vport->enabled_events & MC_ADDR_CHANGE) {
		mlx5_esw_update_vport_addr_list(dev, vport->vport,
						MLX5_NVPRT_LIST_TYPE_MC);
		mlx5_esw_apply_vport_addr_list(dev, vport->vport,
					       MLX5_NVPRT_LIST_TYPE_MC);
	}

	mlx5_esw_update_vport_promsic(dev, vport->vport);

	esw_debug(dev, "vport[%d] EVENT DONE perm mac: %pM\n",
		  vport->vport, mac);
	if (vport->enabled)
		mlx5_arm_vport_context_events(dev, vport->vport,
					      vport->enabled_events);
}

void mlx5_vport_change(struct mlx5_core_dev *dev, struct mlx5_eqe *eqe)
{
	struct mlx5_eqe_vport_change *vc_eqe = &eqe->data.vport_change;
	struct mlx5_eswitch *esw = &dev->priv.eswitch;
	u16 vport_num = be16_to_cpu(vc_eqe->vport_num);
	struct mlx5_vport *vport;

	esw_debug(dev, "vport[%d]\n", vport_num);
	vport = &dev->priv.eswitch.vports[vport_num];
	spin_lock(&vport->lock);
	if (vport->enabled)
		queue_work(esw->work_queue, &vport->vport_change_handler);
	spin_unlock(&vport->lock);
}

static void mlx5_eswitch_enable_vport(struct mlx5_core_dev *dev, int vport_num,
				      int enable_events)
{
	struct mlx5_eswitch *esw = &dev->priv.eswitch;
	struct mlx5_vport *vport = &esw->vports[vport_num];

	WARN_ON(vport->enabled);
	vport->vport = vport_num;
	vport->dev = dev;
	INIT_WORK(&vport->vport_change_handler,
		  mlx5_eswitch_vport_change_handler);

	spin_lock_init(&vport->lock);

	vport->enabled_events = enable_events;
	vport->enabled = true;

	mlx5_modify_vport_admin_state(dev,
				      MLX5_QUERY_VPORT_STATE_IN_OP_MOD_ESW_VPORT,
				      vport_num,
				      MLX5_ESW_VPORT_ADMIN_STATE_AUTO);

	esw->enabled_vports++;
	/* synch current vport context */
	mlx5_eswitch_vport_change_handler(&vport->vport_change_handler);
	mlx5_arm_vport_context_events(dev, vport_num, enable_events);
}

static void mlx5_eswitch_disable_vport(struct mlx5_core_dev *dev, int vport_num)
{
	struct mlx5_eswitch *esw = &dev->priv.eswitch;
	struct mlx5_vport *vport = &esw->vports[vport_num];

	if (!vport->enabled)
		return;

	spin_lock(&vport->lock);
	vport->enabled_events = 0;
	vport->enabled = false;
	spin_unlock(&vport->lock);

	mlx5_modify_vport_admin_state(dev,
				      MLX5_QUERY_VPORT_STATE_IN_OP_MOD_ESW_VPORT,
				      vport_num,
				      MLX5_ESW_VPORT_ADMIN_STATE_DOWN);

	/* Wait for current events completion */
	flush_workqueue(esw->work_queue);

	/* Disable events from this vport */
	mlx5_arm_vport_context_events(dev, vport->vport, 0);

	/* Sorry, but I don't count on VFs to cleanup their own shit */
	mlx5_esw_cleanup_vport(dev, vport_num);
	esw->enabled_vports--;
}

int mlx5_eswitch_enable_sriov(struct mlx5_core_dev *dev, int nvfs)
{
	struct mlx5_eswitch *esw = &dev->priv.eswitch;
	int err;
	int i;

	if (!MLX5_CAP_GEN(dev, vport_group_manager) ||
	    MLX5_CAP_GEN(dev, port_type) != MLX5_CAP_PORT_TYPE_ETH)
		return 0;

	if (!MLX5_CAP_GEN(dev, eswitch_flow_table) ||
	    !MLX5_CAP_ESW_FLOWTABLE_FDB(dev, ft_support)) {
		esw_warn(dev, "E-Switch FDB is not supported, aborting ...\n");
		return -ENOTSUPP;
	}

	esw_info(dev, "E-Switch enable SRIOV: nvfs(%d)\n", nvfs);

	/* VPORT 0 (PF) must be disabled prior enabling with SRIOV_VPORT_EVENTS */
	mlx5_eswitch_disable_vport(dev, 0);

	err = mlx5_esw_create_fdb_table(dev, nvfs + 1);
	if (err)
		return err;

	for (i = 0; i <= nvfs; i++)
		mlx5_eswitch_enable_vport(dev, i, SRIOV_VPORT_EVENTS);

	mlx5_eswfdb_init_promisc_rules(dev);

	esw_info(dev, "SRIOV enabled: active vports(%d)\n",
		 esw->enabled_vports);

	return 0;
}

int mlx5_eswitch_disable_sriov(struct mlx5_core_dev *dev)
{
	struct mlx5_eswitch *esw = &dev->priv.eswitch;
	int i;

	if (!MLX5_CAP_GEN(dev, vport_group_manager) ||
	    MLX5_CAP_GEN(dev, port_type) != MLX5_CAP_PORT_TYPE_ETH)
		return 0;

	esw_info(dev, "disable SRIOV: active vports(%d)\n",
		 esw->enabled_vports);

	for (i = 0; i < esw->total_vports; i++)
		mlx5_eswitch_disable_vport(dev, i);

	mlx5_eswfdb_cleanup_promisc_rules(dev);

	/* TODO: check that UC/MC ESW lists are clear */
	mlx5_esw_destroy_fdb_table(dev);

	/* VPORT 0 (PF) must be enabled back with non-sriov configuration */
	mlx5_eswitch_enable_vport(dev, 0, UC_ADDR_CHANGE);
	return 0;
}

int mlx5_eswitch_init(struct mlx5_core_dev *dev)
{
	int l2_table_size = 1 << MLX5_CAP_GEN(dev, log_max_l2_table);
	int total_vports = 1 + pci_sriov_get_totalvfs(dev->pdev);
	struct mlx5_eswitch *esw = &dev->priv.eswitch;
	struct mlx5_l2_table *l2_table = &esw->l2_table;
	int err;

	if (!MLX5_CAP_GEN(dev, vport_group_manager) ||
	    MLX5_CAP_GEN(dev, port_type) != MLX5_CAP_PORT_TYPE_ETH)
		return 0;

	if (total_vports <= 0)
		total_vports = 1;

	esw_info(dev,
		 "Total vports %d, l2 table size(%d), per vport: max uc(%d) max mc(%d)\n",
		 total_vports, l2_table_size,
		 MLX5_MAX_UC_PER_VPORT(dev),
		 MLX5_MAX_MC_PER_VPORT(dev));

	l2_table->bitmap = kcalloc(BITS_TO_LONGS(l2_table_size),
				   sizeof(uintptr_t), GFP_KERNEL);
	if (!l2_table->bitmap)
		return -ENOMEM;

	l2_table->size = l2_table_size;

	esw->work_queue = create_singlethread_workqueue("mlx5_esw_wq");
	if (!esw->work_queue) {
		err = -ENOMEM;
		goto abort;
	}

	esw->vports = kcalloc(total_vports, sizeof(struct mlx5_vport),
			      GFP_KERNEL);
	if (!esw->vports) {
		err = -ENOMEM;
		goto abort;
	}

	esw->total_vports = total_vports;
	esw->enabled_vports = 0;

	mlx5_eswitch_enable_vport(dev, 0, UC_ADDR_CHANGE);
	/* VF Vports will be enabled when SRIOV is enabled */

	return 0;
abort:
	kfree(l2_table->bitmap);

	if (esw->work_queue)
		destroy_workqueue(esw->work_queue);

	return err;
}

void mlx5_eswitch_cleanup(struct mlx5_core_dev *dev)
{
	struct mlx5_eswitch *esw = &dev->priv.eswitch;
	struct mlx5_l2_table *l2_table = &esw->l2_table;

	if (!MLX5_CAP_GEN(dev, vport_group_manager) ||
	    MLX5_CAP_GEN(dev, port_type) != MLX5_CAP_PORT_TYPE_ETH)
		return;

	esw_info(dev, "cleanup\n");
	mlx5_eswitch_disable_vport(dev, 0);

	destroy_workqueue(esw->work_queue);
	kfree(esw->vports);
	kfree(l2_table->bitmap);
}

/* Vport Administration */

int mlx5_eswitch_set_vport_mac(struct mlx5_core_dev *dev,
			       int vport, u8 mac[ETH_ALEN])
{
	int err = 0;

	if (!MLX5_CAP_GEN(dev, vport_group_manager) ||
	    !mlx5_core_is_pf(dev))
		return -EPERM;

	if (vport < 0 || vport >= dev->priv.eswitch.total_vports)
		return -EINVAL;

	err = mlx5_modify_nic_vport_mac_address(dev, vport, mac);
	if (err) {
		mlx5_core_warn(dev, "Failed to mlx5_modify_nic_vport_mac vport(%d) err=(%d)\n",
			       vport, err);
		return err;
	}

	return err;
}

int mlx5_eswitch_set_vport_link_state(struct mlx5_core_dev *dev,
				      int vport, int link_state)
{
	if (!MLX5_CAP_GEN(dev, vport_group_manager) ||
	    !mlx5_core_is_pf(dev))
		return -EPERM;

	if (vport < 0 || vport >= dev->priv.eswitch.total_vports)
		return -EINVAL;

	return mlx5_modify_vport_admin_state(dev,
					     MLX5_QUERY_VPORT_STATE_IN_OP_MOD_ESW_VPORT,
					     vport, link_state);
}

#ifdef HAVE_NDO_SET_VF_MAC
int mlx5_eswitch_get_vport_config(struct mlx5_core_dev *dev,
				  int vport, struct ifla_vf_info *ivi)
{
	u16 vlan;
	u8 qos;
	if (!MLX5_CAP_GEN(dev, vport_group_manager) ||
	    !mlx5_core_is_pf(dev))
		return -EPERM;

	if (vport < 0 || vport >= dev->priv.eswitch.total_vports)
		return -EINVAL;

	ivi->vf = vport - 1;

	mlx5_query_nic_vport_mac_address(dev, vport, ivi->mac);
#ifdef HAVE_LINKSTATE
	ivi->linkstate = mlx5_query_vport_admin_state(dev,
						      MLX5_QUERY_VPORT_STATE_IN_OP_MOD_ESW_VPORT,
						      vport);
#endif
	mlx5_query_esw_vport_cvlan(dev, vport, &vlan, &qos);
	ivi->vlan = vlan;
	ivi->qos = qos;
#ifdef HAVE_VF_INFO_SPOOFCHK
	ivi->spoofchk = 0;
#endif

	return 0;
}
#endif

int mlx5_eswitch_set_vport_vlan(struct mlx5_core_dev *dev,
				int vport, u16 vlan, u8 qos)
{
	int set = 0;

	if (!MLX5_CAP_GEN(dev, vport_group_manager) ||
	    !mlx5_core_is_pf(dev))
		return -EPERM;

	if (vport < 0 || vport >= dev->priv.eswitch.total_vports ||
	    (vlan > 4095) || (qos > 7))
		return -EINVAL;

	if (vlan || qos)
		set = 1;

	return mlx5_modify_esw_vport_cvlan(dev, vport, vlan, qos, set);
}

int mlx5_eswitch_set_vport_spoofchk(struct mlx5_core_dev *dev,
				    int vport, bool setting)
{
	if (!MLX5_CAP_GEN(dev, vport_group_manager) ||
	    !mlx5_core_is_pf(dev))
		return -EPERM;

	if (vport < 0 || vport >= dev->priv.eswitch.total_vports)
		return -EINVAL;

	return -ENOTSUPP;
}
