/*
 * Copyright (c) 2013-2015, Mellanox Technologies, Ltd.  All rights reserved.
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

#ifndef __MLX5_CORE_H__
#define __MLX5_CORE_H__

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mlx5/driver.h>


#define DRIVER_NAME "mlx5_core"
#define DRIVER_VERSION	"3.1-1.0.4"
#define DRIVER_RELDATE	"30 Sep 2015"

extern int mlx5_core_debug_mask;

#define MLX5_MAX_NUM_TC 8

#define mlx5_core_dbg(__dev, format, ...)				\
	dev_dbg(&(__dev)->pdev->dev, "%s:%s:%d:(pid %d): " format,	\
		 (__dev)->priv.name, __func__, __LINE__, current->pid,	\
		 ##__VA_ARGS__)

#define mlx5_core_dbg_mask(__dev, mask, format, ...)			\
do {									\
	if ((mask) & mlx5_core_debug_mask)				\
		mlx5_core_dbg(__dev, format, ##__VA_ARGS__);		\
} while (0)

#define mlx5_core_err(__dev, format, ...)				\
	dev_err(&(__dev)->pdev->dev, "%s:%s:%d:(pid %d): " format,	\
	       (__dev)->priv.name, __func__, __LINE__, current->pid,	\
	       ##__VA_ARGS__)

#define mlx5_core_warn(__dev, format, ...)				\
	dev_warn(&(__dev)->pdev->dev, "%s:%s:%d:(pid %d): " format,	\
		(__dev)->priv.name, __func__, __LINE__, current->pid,	\
		##__VA_ARGS__)

enum {
	MLX5_CMD_DATA, /* print command payload only */
	MLX5_CMD_TIME, /* print command execution time */
};

enum {
	MLX5_100_MBPS_UNIT = 3,
	MLX5_GBPS_UNIT	   = 4,
};

static inline int mlx5_cmd_exec_check_status(struct mlx5_core_dev *dev, u32 *in,
					     int in_size, u32 *out,
					     int out_size)
{
	int err;

	err = mlx5_cmd_exec(dev, in, in_size, out, out_size);
	if (err)
		return err;

	return mlx5_cmd_status_to_err((struct mlx5_outbox_hdr *)out);
}

int mlx5_query_hca_caps(struct mlx5_core_dev *dev);
int mlx5_query_board_id(struct mlx5_core_dev *dev);

int mlx5_cmd_init_hca(struct mlx5_core_dev *dev);
int mlx5_cmd_teardown_hca(struct mlx5_core_dev *dev);
void mlx5_core_event(struct mlx5_core_dev *dev, enum mlx5_dev_event event,
			    unsigned long param);
void mlx5_enter_error_state(struct mlx5_core_dev *dev);
int mlx5_rename_eq(struct mlx5_core_dev *dev, int eq_ix, char *name);
int mlx5_core_sriov_configure(struct pci_dev *dev, int num_vfs);
int mlx5_core_enable_hca(struct mlx5_core_dev *dev, u16 func_id);
int mlx5_core_disable_hca(struct mlx5_core_dev *dev, u16 func_id);
int mlx5_modify_port_ets_tc_bw_alloc(struct mlx5_core_dev *mdev,
				     u8 tc_tx_bw[MLX5_MAX_NUM_TC],
				     u8 tc_group[MLX5_MAX_NUM_TC]);
int mlx5_query_port_ets_tc_bw_alloc(struct mlx5_core_dev *mdev,
				    u8 tc_tx_bw[MLX5_MAX_NUM_TC]);
int mlx5_modify_port_priority2tc(struct mlx5_core_dev *mdev,
				 u8 prio2tc[MLX5_MAX_NUM_TC]);
int mlx5_query_port_priority2tc(struct mlx5_core_dev *mdev,
				u8 prio2tc[MLX5_MAX_NUM_TC]);
int mlx5_modify_port_ets_rate_limit(struct mlx5_core_dev *mdev,
				    u8 max_bw_value[MLX5_MAX_NUM_TC]);
int mlx5_query_port_ets_rate_limit(struct mlx5_core_dev *mdev,
				   u8 max_bw_value[MLX5_MAX_NUM_TC],
				   u8 max_bw_unit[MLX5_MAX_NUM_TC]);

int mlx5_eswitch_init(struct mlx5_core_dev *mdev);
void mlx5_eswitch_cleanup(struct mlx5_core_dev *mdev);
void mlx5_vport_change(struct mlx5_core_dev *dev, struct mlx5_eqe *eqe);
int mlx5_eswitch_enable_sriov(struct mlx5_core_dev *dev, int nvfs);
int mlx5_eswitch_disable_sriov(struct mlx5_core_dev *dev);
int mlx5_eswitch_set_vport_mac(struct mlx5_core_dev *dev,
			       int vport, u8 mac[ETH_ALEN]);
int mlx5_eswitch_set_vport_link_state(struct mlx5_core_dev *dev,
				      int vport, int link_state);
int mlx5_eswitch_set_vport_vlan(struct mlx5_core_dev *dev,
				int vport, u16 vlan, u8 qos);
int mlx5_eswitch_set_vport_spoofchk(struct mlx5_core_dev *dev,
				    int vport, bool setting);
#ifdef HAVE_NDO_SET_VF_MAC
int mlx5_eswitch_get_vport_config(struct mlx5_core_dev *dev,
				  int vport, struct ifla_vf_info *ivi);
#endif

void mlx5e_init(void);
void mlx5e_cleanup(void);

#endif /* __MLX5_CORE_H__ */
