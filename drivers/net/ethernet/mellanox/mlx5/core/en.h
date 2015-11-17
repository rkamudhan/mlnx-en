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

#include <linux/if_vlan.h>
#include <linux/etherdevice.h>
#include <linux/mlx5/driver.h>
#include <linux/mlx5/device.h>
#include <linux/mlx5/qp.h>
#include <linux/mlx5/cq.h>
#include <linux/mlx5/transobj.h>
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
#include <linux/inet_lro.h>
#else
#include <net/ip.h>
#endif

#include "linux/mlx5/vport.h"
#include "wq.h"
#include "mlx5_core.h"

#define MLX5E_MAX_NUM_CHANNELS  64
#define MLX5E_MAX_NUM_TC	8
#define MLX5E_MAX_NUM_PRIO	8
#define MLX5E_MAX_MTU		9600
#define MLX5E_MIN_MTU		46

#define MLX5E_PARAMS_MINIMUM_LOG_SQ_SIZE                0x7
#define MLX5E_PARAMS_DEFAULT_LOG_SQ_SIZE                0xa
#define MLX5E_PARAMS_MAXIMUM_LOG_SQ_SIZE                0xd
#define MLX5E_PARAMS_DEFAULT_LOG_STRIDING_RQ_SIZE     0x5
#define MLX5E_PARAMS_MAXIMUM_LOG_STRIDING_RQ_SIZE     0x6
#define MLX5E_PARAMS_DEFAULT_LOG_WQE_STRIDE_SIZE      0x0
#define MLX5E_PARAMS_DEFAULT_LOG_WQE_NUM_STRIDES      0x0
#define MLX5E_PARAMS_HW_NUM_STRIDES_BASIC_VAL         512
#define MLX5E_PARAMS_HW_STRIDE_SIZE_BASIC_VAL         64
#define MLX5E_PARAMS_DEFAULT_MIN_STRIDING_RX_WQES     16

#define MLX5E_PARAMS_MINIMUM_LOG_RQ_SIZE                0x7
#define MLX5E_PARAMS_DEFAULT_LOG_RQ_SIZE                0xa
#define MLX5E_PARAMS_MAXIMUM_LOG_RQ_SIZE                0xd

#define MLX5E_PARAMS_DEFAULT_LRO_WQE_SZ                 (63 * 1024)
#define MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_USEC      0x10
#define MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_USEC_FROM_CQE	0x3
#define MLX5E_PARAMS_DEFAULT_RX_CQ_MODERATION_PKTS      0x20
#define MLX5E_PARAMS_DEFAULT_TX_CQ_MODERATION_USEC      0x10
#define MLX5E_PARAMS_DEFAULT_TX_CQ_MODERATION_PKTS      0x20
#define MLX5E_PARAMS_DEFAULT_MIN_RX_WQES                0x80
#define MLX5E_PARAMS_DEFAULT_RX_HASH_LOG_TBL_SZ         0x7

#define MLX5E_TX_CQ_POLL_BUDGET        128
#define MLX5E_UPDATE_STATS_INTERVAL    200 /* msecs */
#define MLX5E_SQ_BF_BUDGET             16

#define MLX5E_INDICATE_WQE_ERR	       0xffff
#define MLX5E_MSG_LEVEL                NETIF_MSG_LINK

#define mlx5e_dbg(mlevel, priv, format, ...)                    \
do {                                                            \
	if (NETIF_MSG_##mlevel & (priv)->msg_level)             \
		netdev_warn(priv->netdev, format,               \
			    ##__VA_ARGS__);                     \
} while (0)

#if !defined(HAVE_NETDEV_EXTENDED_HW_FEATURES)     && \
    !defined(HAVE_NETDEV_OPS_EXT_NDO_FIX_FEATURES) && \
    !defined(HAVE_NETDEV_OPS_EXT_NDO_SET_FEATURES)
#define LEGACY_ETHTOOL_OPS
#endif

enum {
	MLX5E_LINK_SPEED,
	MLX5E_LINK_STATE,
	MLX5E_HEALTH_INFO,
	MLX5E_LOOPBACK,
	MLX5E_NUM_SELF_TEST,
};

enum {
	MLX5E_CON_PROTOCOL_802_1_RP,
	MLX5E_CON_PROTOCOL_R_ROCE_RP,
	MLX5E_CON_PROTOCOL_R_ROCE_NP,
	MLX5E_CONG_PROTOCOL_NUM,
};

#define HEADER_COPY_SIZE		(128 - NET_IP_ALIGN)
#define MLX5E_LOOPBACK_TEST_PAYLOAD	(HEADER_COPY_SIZE - ETH_HLEN)

static const char vport_strings[][ETH_GSTRING_LEN] = {
	/* vport statistics */
	"rx_packets",
	"rx_bytes",
	"tx_packets",
	"tx_bytes",
	"rx_error_packets",
	"rx_error_bytes",
	"tx_error_packets",
	"tx_error_bytes",
	"rx_unicast_packets",
	"rx_unicast_bytes",
	"tx_unicast_packets",
	"tx_unicast_bytes",
	"rx_multicast_packets",
	"rx_multicast_bytes",
	"tx_multicast_packets",
	"tx_multicast_bytes",
	"rx_broadcast_packets",
	"rx_broadcast_bytes",
	"tx_broadcast_packets",
	"tx_broadcast_bytes",

	/* SW counters */
	"tso_packets",
	"tso_bytes",
	"lro_packets",
	"lro_bytes",
	"rx_csum_good",
	"rx_csum_none",
	"rx_csum_sw",
	"tx_csum_offload",
	"tx_csum_none",
	"tx_queue_stopped",
	"tx_queue_wake",
	"tx_queue_dropped",
	"rx_wqe_err",
	/* port statistics */
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	"sw_lro_aggregated", "sw_lro_flushed", "sw_lro_no_desc",
#endif
};

struct mlx5e_vport_stats {
	/* HW counters */
	u64 rx_packets;
	u64 rx_bytes;
	u64 tx_packets;
	u64 tx_bytes;
	u64 rx_error_packets;
	u64 rx_error_bytes;
	u64 tx_error_packets;
	u64 tx_error_bytes;
	u64 rx_unicast_packets;
	u64 rx_unicast_bytes;
	u64 tx_unicast_packets;
	u64 tx_unicast_bytes;
	u64 rx_multicast_packets;
	u64 rx_multicast_bytes;
	u64 tx_multicast_packets;
	u64 tx_multicast_bytes;
	u64 rx_broadcast_packets;
	u64 rx_broadcast_bytes;
	u64 tx_broadcast_packets;
	u64 tx_broadcast_bytes;

	/* SW counters */
	u64 tso_packets;
	u64 tso_bytes;
	u64 lro_packets;
	u64 lro_bytes;
	u64 rx_csum_good;
	u64 rx_csum_none;
	u64 rx_csum_sw;
	u64 tx_csum_offload;
	u64 tx_csum_none;
	u64 tx_queue_stopped;
	u64 tx_queue_wake;
	u64 tx_queue_dropped;
	u64 rx_wqe_err;

	/* SW LRO statistics */    
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	u64 sw_lro_aggregated;
	u64 sw_lro_flushed;
	u64 sw_lro_no_desc; 
#define NUM_VPORT_COUNTERS     36
#else
#define NUM_VPORT_COUNTERS     33
#endif
};

#if !(defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED))
/* Minimum packet number till arming the CQ */
#define MLX5_EN_MIN_RX_ARM	2097152
#endif

static const char pport_strings[][ETH_GSTRING_LEN] = {
	/* IEEE802.3 counters */
	"frames_tx",
	"frames_rx",
	"check_seq_err",
	"alignment_err",
	"octets_tx",
	"octets_received",
	"multicast_xmitted",
	"broadcast_xmitted",
	"multicast_rx",
	"broadcast_rx",
	"in_range_len_errors",
	"out_of_range_len",
	"too_long_errors",
	"symbol_err",
	"mac_control_tx",
	"mac_control_rx",
	"unsupported_op_rx",
	"pause_ctrl_rx",
	"pause_ctrl_tx",

	/* RFC2863 counters */
	"in_octets",
	"in_ucast_pkts",
	"in_discards",
	"in_errors",
	"in_unknown_protos",
	"out_octets",
	"out_ucast_pkts",
	"out_discards",
	"out_errors",
	"in_multicast_pkts",
	"in_broadcast_pkts",
	"out_multicast_pkts",
	"out_broadcast_pkts",

	/* RFC2819 counters */
	"drop_events",
	"octets",
	"pkts",
	"broadcast_pkts",
	"multicast_pkts",
	"crc_align_errors",
	"undersize_pkts",
	"oversize_pkts",
	"fragments",
	"jabbers",
	"collisions",
	"p64octets",
	"p65to127octets",
	"p128to255octets",
	"p256to511octets",
	"p512to1023octets",
	"p1024to1518octets",
	"p1519to2047octets",
	"p2048to4095octets",
	"p4096to8191octets",
	"p8192to10239octets",

	/* physical layer counters */
	"time_since_last_clear",
	"symbol_errors",
	"sync_headers_errors",
	"edpl/bip_errors_lane0",
	"edpl/bip_errors_lane1",
	"edpl/bip_errors_lane2",
	"edpl/bip_errors_lane3",
	"fc_corrected_blocks_lane0",
	"fc_corrected_blocks_lane1",
	"fc_corrected_blocks_lane2",
	"fc_corrected_blocks_lane3",
	"fc_uncorrectable_blocks_lane0",
	"fc_uncorrectable_blocks_lane1",
	"fc_uncorrectable_blocks_lane2",
	"fc_uncorrectable_blocks_lane3",
	"rs_corrected_blocks",
	"rs_uncorrectable_blocks",
	"rs_no_errors_blocks",
	"rs_single_error_blocks",
	"rs_corrected_symbols_total",
	"rs_corrected_symbols_lane0",
	"rs_corrected_symbols_lane1",
	"rs_corrected_symbols_lane2",
	"rs_corrected_symbols_lane3",
};

#define NUM_IEEE_802_3_COUNTERS		19
#define NUM_RFC_2863_COUNTERS		13
#define NUM_RFC_2819_COUNTERS		21
#define NUM_PHYSICAL_LAYER_COUNTERS	24
#define NUM_PPORT_COUNTERS		(NUM_IEEE_802_3_COUNTERS + \
					 NUM_RFC_2863_COUNTERS + \
					 NUM_RFC_2819_COUNTERS + \
					 NUM_PHYSICAL_LAYER_COUNTERS)

struct mlx5e_pport_stats {
	__be64 IEEE_802_3_counters[NUM_IEEE_802_3_COUNTERS];
	__be64 RFC_2863_counters[NUM_RFC_2863_COUNTERS];
	__be64 RFC_2819_counters[NUM_RFC_2819_COUNTERS];
	__be64 PHYSICAL_LAYER_counters[NUM_PHYSICAL_LAYER_COUNTERS];
};

static const char rq_stats_strings[][ETH_GSTRING_LEN] = {
	"packets",
	"csum_none",
	"csum_good",
	"csum_sw",
	"lro_packets",
	"lro_bytes",
	"wqe_err"
};

struct mlx5e_rq_stats {
	u64 packets;
	u64 csum_none;
	u64 csum_good;
	u64 csum_sw;
	u64 lro_packets;
	u64 lro_bytes;
	u64 wqe_err;
#define NUM_RQ_STATS 7
};

static const char sq_stats_strings[][ETH_GSTRING_LEN] = {
	"packets",
	"tso_packets",
	"tso_bytes",
	"csum_offload_none",
	"csum_offload_part",
	"stopped",
	"wake",
	"dropped",
	"nop"
};

struct mlx5e_sq_stats {
	u64 packets;
	u64 tso_packets;
	u64 tso_bytes;
	u64 csum_offload_none;
	u64 csum_offload_part;
	u64 stopped;
	u64 wake;
	u64 dropped;
	u64 nop;
#define NUM_SQ_STATS 9
};

static const char qcounter_stats_strings[][ETH_GSTRING_LEN] = {
	"out_of_buffer",
#define NUM_Q_COUNTERS 1
};

struct mlx5e_stats {
	struct mlx5e_vport_stats   vport;
	struct mlx5e_pport_stats   pport;
	u32                        out_of_buffer;
};

struct mlx5e_params {
	u8  log_sq_size;
	u8  log_rq_size;
	u16 num_channels;
	u8  default_vlan_prio;
	u8  num_tc;
	u16 rx_cq_moderation_usec;
	u16 rx_cq_moderation_pkts;
	u16 tx_cq_moderation_usec;
	u16 tx_cq_moderation_pkts;
	u16 min_rx_wqes;
	u16 rx_hash_log_tbl_sz;
	bool lro_en;
	u32 lro_wqe_sz;
	bool rss_hash_xor;
};

enum {
	MLX5E_RQ_STATE_POST_WQES_ENABLE,
};

enum cq_flags {
	MLX5E_CQ_HAS_CQES = 1,
};

struct mlx5e_rq;
struct mlx5e_rx_wqe;
struct mlx5e_cq;
typedef void (*mlx5e_clean_rq_fn)(struct mlx5e_rq *rq);
typedef int (*mlx5e_alloc_rx_wqe_fn)(struct mlx5e_rq *rq,
				     struct mlx5e_rx_wqe *wqe, u16 ix);
/* poll rx cq is depend on the type of the RQ */
typedef struct sk_buff* (*mlx5e_poll_rx_cq_fn)(struct mlx5_cqe64 *cqe,
						   struct mlx5e_rq *rq,
						   u16 *bytes_recv,
						   struct mlx5e_rx_wqe **ret_wqe,
						   __be16 *ret_wqe_id_be);
typedef bool (*mlx5e_is_rx_pop_fn)(struct mlx5e_rq *rq);

struct mlx5e_rx_wqe_info {
	struct page	     *page;
	dma_addr_t	     dma_addr;
	u16		     used_strides;
};

struct mlx5e_cq {
	/* data path - accessed per cqe */
	struct mlx5_cqwq           wq;
	unsigned long              flags;

	/* data path - accessed per napi poll */
	struct napi_struct        *napi;
	struct mlx5_core_cq        mcq;
	struct mlx5e_channel      *channel;

	/* control */
	struct mlx5_wq_ctrl        wq_ctrl;
} ____cacheline_aligned_in_smp;

#ifdef HAVE_GET_SET_PRIV_FLAGS
static const char mlx5e_priv_flags[][ETH_GSTRING_LEN] = {
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	"hw_lro",
#endif
};
#endif

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
#define IS_HW_LRO(priv) \
	(priv->params.lro_en && (priv->pflags & MLX5E_PRIV_FLAG_HWLRO))
#define IS_SW_LRO(priv) \
	(priv->params.lro_en && !(priv->pflags & MLX5E_PRIV_FLAG_HWLRO))

/* SW LRO defines for MLX5 */
#define MLX5E_RQ_FLAG_SWLRO	(1<<0)

#define MLX5E_LRO_MAX_DESC	32
struct mlx5e_sw_lro {
	struct net_lro_mgr	lro_mgr;
	struct net_lro_desc	lro_desc[MLX5E_LRO_MAX_DESC];
};
#endif

struct mlx5e_rq {
	/* data path */
	struct mlx5_wq_ll      wq;
	u32                    wqe_sz;
	struct sk_buff       **skb;
	struct mlx5e_rx_wqe_info *wqe_info;
	u8		       page_order;
	u16		       num_of_strides_in_wqe;
	u16		       stride_size;
	u16		       current_wqe;
	mlx5e_alloc_rx_wqe_fn  alloc_wqe;
	mlx5e_poll_rx_cq_fn    mlx5e_poll_specific_rx_cq;
	mlx5e_is_rx_pop_fn     is_poll;
	struct device         *pdev;
	struct net_device     *netdev;
	struct mlx5e_rq_stats  stats;
	struct mlx5e_cq        cq;

	unsigned long          state;
	int                    ix;
#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
	struct mlx5e_sw_lro sw_lro;
#endif

	/* control */
	struct mlx5_wq_ctrl    wq_ctrl;
	u32                    rqn;
	struct mlx5e_channel  *channel;
	enum rq_type	       rq_type;
	mlx5e_clean_rq_fn      clean_rq;
} ____cacheline_aligned_in_smp;

struct mlx5e_tx_skb_cb {
	u32 num_bytes;
	u8  num_wqebbs;
	u8  num_dma;
};

#define MLX5E_TX_SKB_CB(__skb) ((struct mlx5e_tx_skb_cb *)__skb->cb)

struct mlx5e_sq_dma {
	dma_addr_t addr;
	u32        size;
};

enum {
	MLX5E_SQ_STATE_WAKE_TXQ_ENABLE,
};

struct mlx5e_sq {
	/* data path */

	/* dirtied @completion */
	u16                        cc;
	u32                        dma_fifo_cc;

	/* dirtied @xmit */
	u16                        pc ____cacheline_aligned_in_smp;
	u32                        dma_fifo_pc;
	u16                        bf_offset;
	u16                        prev_cc;
	u8                         bf_budget;
	struct mlx5e_sq_stats      stats;

	struct mlx5e_cq            cq;

	/* pointers to per packet info: write@xmit, read@completion */
	struct sk_buff           **skb;
	struct mlx5e_sq_dma       *dma_fifo;

	/* read only */
	struct mlx5_wq_cyc         wq;
	u32                        dma_fifo_mask;
	void __iomem              *uar_map;
	void __iomem              *uar_bf_map;
	struct netdev_queue       *txq;
	u32                        sqn;
	u16                        bf_buf_size;
	u16                        max_inline;
	u16                        edge;
	struct device             *pdev;
	__be32                     mkey_be;
	unsigned long              state;

	/* control path */
	struct mlx5_wq_ctrl        wq_ctrl;
	struct mlx5_uar            uar;
	struct mlx5e_channel      *channel;
	int                        tc;
} ____cacheline_aligned_in_smp;

static inline bool mlx5e_sq_has_room_for(struct mlx5e_sq *sq, u16 n)
{
	return (((sq->wq.sz_m1 & (sq->cc - sq->pc)) >= n) ||
		(sq->cc  == sq->pc));
}

enum channel_flags {
	MLX5E_CHANNEL_NAPI_SCHED = 1,
};

struct mlx5e_channel {
	/* data path */
	struct mlx5e_rq            rq;
	struct mlx5e_sq            sq[MLX5E_MAX_NUM_TC];
	struct napi_struct         napi;
	struct device             *pdev;
	struct net_device         *netdev;
	__be32                     mkey_be;
	u8                         num_tc;
	unsigned long              flags;

	/* data path - accessed per napi poll */
	struct irq_desc           *irq_desc;
#if !(defined(HAVE_IRQ_DESC_GET_IRQ_DATA) && defined(HAVE_IRQ_TO_DESC_EXPORTED))
	u32 tot_rx;
#endif
	/* control */
	struct mlx5e_priv         *priv;
	int                        ix;
	int                        cpu;

	struct dentry             *dfs_root;
};

enum mlx5e_traffic_types {
	MLX5E_TT_IPV4_TCP,
	MLX5E_TT_IPV6_TCP,
	MLX5E_TT_IPV4_UDP,
	MLX5E_TT_IPV6_UDP,
	MLX5E_TT_IPV4_IPSEC_AH,
	MLX5E_TT_IPV6_IPSEC_AH,
	MLX5E_TT_IPV4_IPSEC_ESP,
	MLX5E_TT_IPV6_IPSEC_ESP,
	MLX5E_TT_IPV4,
	MLX5E_TT_IPV6,
	MLX5E_TT_ANY,
	MLX5E_NUM_TT,
};

enum {
	MLX5E_RQT_SPREADING  = 0,
	MLX5E_RQT_DEFAULT_RQ = 1,
	MLX5E_NUM_RQT        = 2,
};

struct mlx5e_eth_ft_type {
	struct mlx5_core_fs_dst *dst;
};

struct mlx5e_eth_addr_info {
	u8			addr[ETH_ALEN + 2];
	u32			tt_vec;
	struct mlx5_flow_rule   *ft_rule[MLX5E_NUM_TT];
};

struct mlx5e_flow_vlan {
	struct mlx5_core_fs_ft		*vlan_ft;
	struct mlx5_core_fs_fg		*tagged_fg;
	struct mlx5_core_fs_fg		*untagged_fg;
};

struct mlx5e_flow_main {
	struct mlx5_core_fs_ft		*main_ft;
	struct mlx5_core_fs_fg		*fg[9];
};

#define MLX5E_ETH_ADDR_HASH_SIZE (1 << BITS_PER_BYTE)

struct mlx5e_eth_addr_db {
	struct hlist_head          netdev_uc[MLX5E_ETH_ADDR_HASH_SIZE];
	struct hlist_head          netdev_mc[MLX5E_ETH_ADDR_HASH_SIZE];
	struct mlx5e_eth_addr_info broadcast;
	struct mlx5e_eth_addr_info allmulti;
	struct mlx5e_eth_addr_info promisc;
	bool                       broadcast_enabled;
	bool                       allmulti_enabled;
	bool                       promisc_enabled;
};

enum {
	MLX5E_STATE_ASYNC_EVENTS_ENABLE,
	MLX5E_STATE_OPENED,
};

enum {
	MLX5E_WOL_DISABLE	= 0,
	MLX5E_WOL_SECURED_MAGIC = 1 << 1,
	MLX5E_WOL_MAGIC		= 1 << 2,
	MLX5E_WOL_ARP		= 1 << 3,
	MLX5E_WOL_BROADCAST	= 1 << 4,
	MLX5E_WOL_MULTICAST	= 1 << 5,
	MLX5E_WOL_UNICAST	= 1 << 6,
	MLX5E_WOL_PHY_ACTIVITY	= 1 << 7,
};

struct mlx5e_vlan_db {
	unsigned long active_vlans[BITS_TO_LONGS(VLAN_N_VID)];
	struct mlx5_flow_rule	*active_vlans_rule[VLAN_N_VID];
	struct mlx5_flow_rule	*untagged_rule;
	struct mlx5_flow_rule	*any_vlan_rule;
	bool			filter_disabled;
};

struct mlx5e_flow_table {
	int num_groups;
	struct mlx5_flow_table		*t;
	struct mlx5_flow_group		**g;
};

struct mlx5e_flow_tables {
	struct mlx5_flow_namespace	*ns;
	struct mlx5e_flow_table		vlan;
	struct mlx5e_flow_table		main;
};

struct mlx5e_ecn_rp_attributes {
	struct mlx5_core_dev	*mdev;
	/* ATTRIBUTES */
	struct kobj_attribute	enable;
	struct kobj_attribute	clamp_tgt_rate;
	struct kobj_attribute	clamp_tgt_rate_ati;
	struct kobj_attribute	rpg_time_reset;
	struct kobj_attribute	rpg_byte_reset;
	struct kobj_attribute	rpg_threshold;
	struct kobj_attribute	rpg_max_rate;
	struct kobj_attribute	rpg_ai_rate;
	struct kobj_attribute	rpg_hai_rate;
	struct kobj_attribute	rpg_gd;
	struct kobj_attribute	rpg_min_dec_fac;
	struct kobj_attribute	rpg_min_rate;
	struct kobj_attribute	rate2set_fcnp;
	struct kobj_attribute	dce_tcp_g;
	struct kobj_attribute	dce_tcp_rtt;
	struct kobj_attribute	rreduce_mperiod;
	struct kobj_attribute	initial_alpha_value;
};

struct mlx5e_ecn_np_attributes {
	struct mlx5_core_dev	*mdev;
	/* ATTRIBUTES */
	struct kobj_attribute	enable;
	struct kobj_attribute	min_time_between_cnps;
	struct kobj_attribute	cnp_dscp;
	struct kobj_attribute	cnp_802p_prio;
};

union mlx5e_ecn_attributes {
	struct mlx5e_ecn_rp_attributes rp_attr;
	struct mlx5e_ecn_np_attributes np_attr;
};

struct mlx5e_ecn_ctx {
	union mlx5e_ecn_attributes ecn_attr;
	struct kobject *ecn_proto_kobj;
	struct kobject *ecn_enable_kobj;
};

struct mlx5e_ecn_enable_ctx {
	int cong_protocol;
	int priority;
	struct mlx5_core_dev	*mdev;

	struct kobj_attribute	enable;
};

#define MLX5E_NIC_DEFAULT_PRIO	0

#ifdef CONFIG_COMPAT_LRO_ENABLED_IPOIB
#define MLX5E_PRIV_FLAG_HWLRO (1<<0)
#endif

struct mlx5e_priv {
	/* priv data path fields - start */
	int                        default_vlan_prio;
	struct mlx5e_sq            **txq_to_sq_map;
	int tc_to_txq_map[MLX5E_MAX_NUM_CHANNELS][MLX5E_MAX_NUM_TC];
#if defined HAVE_VLAN_GRO_RECEIVE || defined HAVE_VLAN_HWACCEL_RX
	struct vlan_group          *vlan_grp;
#endif
	/* priv data path fields - end */

	unsigned long              state;
	struct mutex               state_lock; /* Protects Interface state */
	struct mlx5_uar            cq_uar;
	u32                        pdn;
	u32                        tdn;
	struct mlx5_core_mr        mr;

	struct mlx5e_channel     **channel;
	u32                        tisn[MLX5E_MAX_NUM_TC];
	u32                        rqtn;
	u32                        tirn[MLX5E_NUM_TT];

	struct mlx5e_flow_tables   fts;
	struct mlx5e_eth_addr_db   eth_addr;
	struct mlx5e_vlan_db       vlan;
	bool                       loopback_ok;
	bool                       validate_loopback;

	struct mlx5e_params        params;
	spinlock_t                 async_events_spinlock; /* sync hw events */
	struct work_struct         update_carrier_work;
	struct work_struct         set_rx_mode_work;
	struct delayed_work        update_stats_work;

	struct mlx5_core_dev      *mdev;
	struct net_device         *netdev;
	struct mlx5e_stats         stats;
#ifdef HAVE_GET_SET_PRIV_FLAGS
	u32                        pflags;
#endif
#ifndef HAVE_NDO_GET_STATS64
	struct net_device_stats    netdev_stats;
#endif

	int                        counter_set_id;

	struct dentry *dfs_root;
	u32 msg_level;

	struct kobject *ecn_root_kobj;

	struct mlx5e_ecn_ctx ecn_ctx[MLX5E_CONG_PROTOCOL_NUM];
	struct mlx5e_ecn_enable_ctx ecn_enable_ctx[MLX5E_CONG_PROTOCOL_NUM][8];
	int			   internal_error;
};

#define MLX5E_NET_IP_ALIGN 2

struct mlx5e_tx_wqe {
	struct mlx5_wqe_ctrl_seg ctrl;
	struct mlx5_wqe_eth_seg  eth;
};

struct mlx5e_rx_wqe {
	struct mlx5_wqe_srq_next_seg  next;
	struct mlx5_wqe_data_seg      data;
};

void mlx5e_send_nop(struct mlx5e_sq *sq, bool notify_hw);
#if defined(NDO_SELECT_QUEUE_HAS_ACCEL_PRIV) || defined(HAVE_SELECT_QUEUE_FALLBACK_T)
u16 mlx5e_select_queue(struct net_device *dev, struct sk_buff *skb,
#ifdef HAVE_SELECT_QUEUE_FALLBACK_T
		       void *accel_priv, select_queue_fallback_t fallback);
#else
		       void *accel_priv);
#endif
#else /* NDO_SELECT_QUEUE_HAS_ACCEL_PRIV || HAVE_SELECT_QUEUE_FALLBACK_T */
u16 mlx5e_select_queue(struct net_device *dev, struct sk_buff *skb);
#endif

netdev_tx_t mlx5e_xmit(struct sk_buff *skb, struct net_device *dev);

void mlx5e_completion_event(struct mlx5_core_cq *mcq);
void mlx5e_cq_error_event(struct mlx5_core_cq *mcq, enum mlx5_event event);
int mlx5e_napi_poll(struct napi_struct *napi, int budget);
bool mlx5e_poll_tx_cq(struct mlx5e_cq *cq);
struct sk_buff *mlx5e_poll_default_rx_cq(struct mlx5_cqe64 *cqe,
					 struct mlx5e_rq *rq,
					 u16 *ret_bytes_recv,
					 struct mlx5e_rx_wqe **ret_wqe,
					 __be16 *ret_wqe_id_be);
struct sk_buff *mlx5e_poll_striding_rx_cq(struct mlx5_cqe64 *cqe,
					  struct mlx5e_rq *rq,
					  u16 *ret_bytes_recv,
					  struct mlx5e_rx_wqe **ret_wqe,
					  __be16 *ret_wqe_id_be);
bool mlx5e_poll_rx_cq(struct mlx5e_cq *cq, int budget);
bool is_poll_striding_wqe(struct mlx5e_rq *rq);
void free_rq_res(struct mlx5e_rq *rq);
void free_striding_rq_res(struct mlx5e_rq *rq);
int mlx5e_alloc_rx_wqe(struct mlx5e_rq *rq, struct mlx5e_rx_wqe *wqe, u16 ix);
inline int mlx5e_alloc_striding_rx_wqe(struct mlx5e_rq *rq,
				       struct mlx5e_rx_wqe *wqe, u16 ix);

bool mlx5e_post_rx_wqes(struct mlx5e_rq *rq);
void mlx5e_prefetch_cqe(struct mlx5e_cq *cq);
struct mlx5_cqe64 *mlx5e_get_cqe(struct mlx5e_cq *cq);

void mlx5e_update_stats(struct mlx5e_priv *priv);

int mlx5e_open_flow_table(struct mlx5e_priv *priv);
void mlx5e_close_flow_table(struct mlx5e_priv *priv);
void mlx5e_init_eth_addr(struct mlx5e_priv *priv);
void mlx5e_set_rx_mode_core(struct mlx5e_priv *priv);
void mlx5e_set_rx_mode_work(struct work_struct *work);

#if defined(HAVE_NDO_RX_ADD_VID_HAS_3_PARAMS)
int mlx5e_vlan_rx_add_vid(struct net_device *dev, __always_unused __be16 proto,
			  u16 vid);
#elif defined(HAVE_NDO_RX_ADD_VID_HAS_2_PARAMS_RET_INT)
int mlx5e_vlan_rx_add_vid(struct net_device *dev, u16 vid);
#else
void mlx5e_vlan_rx_add_vid(struct net_device *dev, u16 vid);
#endif
#if defined(HAVE_NDO_RX_ADD_VID_HAS_3_PARAMS)
int mlx5e_vlan_rx_kill_vid(struct net_device *dev, __always_unused __be16 proto,
			   u16 vid);
#elif defined(HAVE_NDO_RX_ADD_VID_HAS_2_PARAMS_RET_INT)
int mlx5e_vlan_rx_kill_vid(struct net_device *dev, u16 vid);
#else
void mlx5e_vlan_rx_kill_vid(struct net_device *dev, u16 vid);
#endif
void mlx5e_enable_vlan_filter(struct mlx5e_priv *priv);
void mlx5e_disable_vlan_filter(struct mlx5e_priv *priv);
int mlx5e_add_all_vlan_rules(struct mlx5e_priv *priv);
void mlx5e_del_all_vlan_rules(struct mlx5e_priv *priv);

int mlx5e_open_locked(struct net_device *netdev);
int mlx5e_close_locked(struct net_device *netdev);
int mlx5e_update_priv_params(struct mlx5e_priv *priv,
			     struct mlx5e_params *new_params);

void mlx5e_create_debugfs(struct mlx5e_priv *priv);
void mlx5e_destroy_debugfs(struct mlx5e_priv *priv);
void mlx5e_self_test(struct net_device *dev,
		     struct ethtool_test *etest,
		     u64 *buf);

int mlx5e_sysfs_create(struct net_device *dev);
void mlx5e_sysfs_remove(struct net_device *dev);

static inline void mlx5e_tx_notify_hw(struct mlx5e_sq *sq,
				      struct mlx5e_tx_wqe *wqe, int bf_sz)
{
	u16 ofst = MLX5_BF_OFFSET + sq->bf_offset;

	/* ensure wqe is visible to device before updating doorbell record */
	wmb();

	*sq->wq.db = cpu_to_be32(sq->pc);

	/* ensure doorbell record is visible to device before ringing the
	 * doorbell */
	wmb();

	if (bf_sz) {
		__iowrite64_copy(sq->uar_bf_map + ofst, &wqe->ctrl, bf_sz);

		/* flush the write-combining mapped buffer */
		wmb();

	} else {
		mlx5_write64((__be32 *)&wqe->ctrl, sq->uar_map + ofst, NULL);
	}

	sq->bf_offset ^= sq->bf_buf_size;
}

static inline void mlx5e_cq_arm(struct mlx5e_cq *cq)
{
	struct mlx5_core_cq *mcq;

	mcq = &cq->mcq;
	mlx5_cq_arm(mcq, MLX5_CQ_DB_REQ_NOT, mcq->uar->map, NULL, cq->wq.cc);
}

extern const struct ethtool_ops mlx5e_ethtool_ops;
#ifdef HAVE_ETHTOOL_OPS_EXT
extern const struct ethtool_ops_ext mlx5e_ethtool_ops_ext;
#endif
#ifdef HAVE_IEEE_DCBNL_ETS
extern const struct dcbnl_rtnl_ops mlx5e_dcbnl_ops;
#endif
