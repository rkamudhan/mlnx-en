/*
 * Copyright (c) 2013-2015, Mellanox Technologies. All rights reserved.
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
#include <linux/device.h>
#include <linux/netdevice.h>
#include "en.h"

#define MLX5E_MAX_PRIORITY 8
#define MLX5E_GBPS_TO_KBPS 1000000
#define MLX5E_100MBPS_TO_KBPS 100000

#ifdef HAVE_IEEE_DCBNL_ETS
static int mlx5e_dbcnl_validate_ets(struct ieee_ets *ets)
{
	int bw_sum = 0;
	int i;

	/* Validate Priority */
	for (i = 0; i < IEEE_8021QAZ_MAX_TCS; i++) {
		if (ets->prio_tc[i] >= MLX5E_MAX_PRIORITY)
			return -EINVAL;
	}

	/* Validate Bandwidth Sum */
	for (i = 0; i < IEEE_8021QAZ_MAX_TCS; i++) {
		if (ets->tc_tsa[i] == IEEE_8021QAZ_TSA_ETS)
			bw_sum += ets->tc_tx_bw[i];
	}

	if (bw_sum != 0 && bw_sum != 100)
		return -EINVAL;
	return 0;
}

static int mlx5e_dcbnl_ieee_getets(struct net_device *netdev,
				   struct ieee_ets *ets)
{
	struct mlx5e_priv *priv = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;
	int err;

	ets->ets_cap = IEEE_8021QAZ_MAX_TCS;
	err = mlx5_query_port_priority2tc(mdev, ets->prio_tc);
	err |= mlx5_query_port_ets_tc_bw_alloc(mdev, ets->tc_tx_bw);
	return err;
}

static int mlx5e_dcbnl_ieee_setets(struct net_device *netdev,
				   struct ieee_ets *ets)
{
	struct mlx5e_priv *priv    = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;
	u8 tc_tx_bw[IEEE_8021QAZ_MAX_TCS] = { 0 };
	u8 tc_group[IEEE_8021QAZ_MAX_TCS] = { 0 };
	int next_group = 0;
	int err;
	int i;

	err = mlx5e_dbcnl_validate_ets(ets);
	if (err)
		return err;

	err = mlx5_modify_port_priority2tc(mdev, ets->prio_tc);
	if (err)
		return err;

	/* higher TC means higher priority => lower pg */
	for (i = IEEE_8021QAZ_MAX_TCS - 1; i >= 0; i--) {
		switch (ets->tc_tsa[i]) {
		case IEEE_8021QAZ_TSA_VENDOR:
			tc_group[i] = 0;
			tc_tx_bw[i] = 100;
			break;
		case IEEE_8021QAZ_TSA_STRICT:
			tc_group[i] = next_group++;
			tc_tx_bw[i] = 100;
			break;
		case IEEE_8021QAZ_TSA_ETS:
			tc_group[i] = 7;
			tc_tx_bw[i] = ets->tc_tx_bw[i] ?: 1;
			break;
		}
	}

	return mlx5_modify_port_ets_tc_bw_alloc(mdev, tc_tx_bw, tc_group);
}

static u8 mlx5e_dcbnl_getdcbx(struct net_device *netdev)
{
	return DCB_CAP_DCBX_HOST | DCB_CAP_DCBX_VER_IEEE;
}

static u8 mlx5e_dcbnl_setdcbx(struct net_device *netdev, u8 mode)
{
	if ((mode & DCB_CAP_DCBX_LLD_MANAGED) ||
	    (mode & DCB_CAP_DCBX_VER_CEE) ||
	    !(mode & DCB_CAP_DCBX_VER_IEEE) ||
	    !(mode & DCB_CAP_DCBX_HOST))
		return 1;

	return 0;
}

int mlx5e_dcbnl_ieee_getmaxrate(struct net_device *netdev,
				struct ieee_maxrate *maxrate)
{
	struct mlx5e_priv *priv    = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;
	u8 max_bw_value[MLX5_MAX_NUM_TC];
	u8 max_bw_unit[MLX5_MAX_NUM_TC];
	int err;
	int i;

	err = mlx5_query_port_ets_rate_limit(mdev, max_bw_value, max_bw_unit);
	if (err)
		return err;

	memset(maxrate->tc_maxrate, 0, sizeof(maxrate->tc_maxrate));

	for (i = 0; i < IEEE_8021QAZ_MAX_TCS; i++) {
		if (max_bw_unit[i] == MLX5_100_MBPS_UNIT)
			maxrate->tc_maxrate[i] = max_bw_value[i] *
						 MLX5E_100MBPS_TO_KBPS;
		else if (max_bw_unit[i] == MLX5_GBPS_UNIT)
			maxrate->tc_maxrate[i] = max_bw_value[i] *
						 MLX5E_GBPS_TO_KBPS;
	}

	return 0;
}

int mlx5e_dcbnl_ieee_setmaxrate(struct net_device *netdev,
				struct ieee_maxrate *maxrate)
{
	struct mlx5e_priv *priv    = netdev_priv(netdev);
	struct mlx5_core_dev *mdev = priv->mdev;
	u8 max_bw_value[IEEE_8021QAZ_MAX_TCS];
	int i;

	for (i = 0; i < IEEE_8021QAZ_MAX_TCS; i++)
		max_bw_value[i] = DIV_ROUND_UP(maxrate->tc_maxrate[i],
					       MLX5E_GBPS_TO_KBPS);

	return mlx5_modify_port_ets_rate_limit(mdev, max_bw_value);
}

const struct dcbnl_rtnl_ops mlx5e_dcbnl_ops = {
	.ieee_getets	= mlx5e_dcbnl_ieee_getets,
	.ieee_setets	= mlx5e_dcbnl_ieee_setets,
#ifdef HAVE_IEEE_GET_SET_MAXRATE
	.ieee_getmaxrate = mlx5e_dcbnl_ieee_getmaxrate,
	.ieee_setmaxrate = mlx5e_dcbnl_ieee_setmaxrate,
#endif
	.getdcbx	= mlx5e_dcbnl_getdcbx,
	.setdcbx	= mlx5e_dcbnl_setdcbx,
};
#endif
