/*
 * Copyright (c) 2014, Mellanox Technologies inc.  All rights reserved.
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

#include <linux/pci.h>
#include <linux/sysfs.h>
#include <linux/mlx5/driver.h>
#include "mlx5_core.h"

static void mlx5_destroy_vfs_sysfs(struct mlx5_core_dev *dev);
static int mlx5_create_vfs_sysfs(struct mlx5_core_dev *dev, int num_vfs);

static void mlx5_core_destroy_vfs(struct pci_dev *pdev)
{
	struct mlx5_core_dev *dev  = pci_get_drvdata(pdev);
	struct mlx5_core_sriov *sriov = &dev->priv.sriov;
	int num_vfs = pci_num_vf(pdev);
	int err;
	int vf;

	for (vf = 1; vf <= num_vfs; vf++) {
		if (sriov->vfs_ctx[vf - 1].enabled) {
			err = mlx5_core_disable_hca(dev, vf);
			if (err)
				mlx5_core_warn(dev, "disable_hca for vf %d failed: %d\n", vf, err);
		}
	}
}

static int mlx5_core_create_vfs(struct pci_dev *pdev, int num_vfs)
{
	int err;

	if (pci_num_vf(pdev))
		pci_disable_sriov(pdev);

	err = pci_enable_sriov(pdev, num_vfs);
	if (err)
		dev_warn(&pdev->dev, "enable sriov failed %d\n", err);

	return err;
}

static int mlx5_core_sriov_enable(struct pci_dev *pdev, int num_vfs)
{
	struct mlx5_core_dev *dev  = pci_get_drvdata(pdev);
	struct mlx5_core_sriov *sriov = &dev->priv.sriov;
	int cur_vfs = pci_num_vf(pdev);
	int err;

	if (cur_vfs) {
		if (cur_vfs != num_vfs) {
			mlx5_core_destroy_vfs(pdev);
		} else {
			err = 0;
			goto out;
		}
	}
	kfree(sriov->vfs_ctx);
	sriov->vfs_ctx = kcalloc(num_vfs, sizeof(*sriov->vfs_ctx), GFP_ATOMIC);
	if (!sriov->vfs_ctx)
		return -ENOMEM;

	sriov->enabled_vfs = num_vfs;
	err = mlx5_core_create_vfs(pdev, num_vfs);
	if (err) {
		kfree(sriov->vfs_ctx);
		sriov->vfs_ctx = NULL;
		return err;
	}

out:
	return err;
}

static void mlx5_core_free_vfs(struct mlx5_core_dev *dev)
{
	struct mlx5_core_sriov *sriov;
	int i;

	if (!mlx5_core_is_pf(dev))
		return;

	sriov = &dev->priv.sriov;
	for (i = 0; i < sriov->num_vfs; ++i)
		if (sriov->vfs_ctx[i].enabled) {
			mlx5_core_disable_hca(dev, i + 1);
			sriov->vfs_ctx[i].enabled = 0;
		}

	if (mlx5_wait_for_vf_pages(dev))
		mlx5_core_warn(dev, "timeout claiming VFs pages\n");
}

int mlx5_core_sriov_configure(struct pci_dev *pdev, int num_vfs)
{
	struct mlx5_core_dev *dev  = pci_get_drvdata(pdev);
	struct mlx5_core_sriov *sriov = &dev->priv.sriov;
	int err;
	int nvf;

	if (!mlx5_core_is_pf(dev))
		return -EPERM;

	if (num_vfs < 0)
		return -EINVAL;

	if (num_vfs == pci_num_vf(pdev)) {
		mlx5_core_warn(dev, "Current number of VFs is %d, requested number of VFs is %d, aborting.\n",
			       num_vfs, pci_num_vf(pdev));
		return 0;
	}

again:
	pr_info("%s: pci_num_vf() %d\n", __func__, pci_num_vf(pdev));
	if (!pci_num_vf(pdev)) {
		if (num_vfs == 0) {
			mlx5_core_warn(dev, "sriov is not enabled\n");
			return -EINVAL;
		}
		if (num_vfs == MLX5_SRIOV_UNLOAD_MAGIC)
			return 0;
	}

	if (pci_vfs_assigned(pdev) && num_vfs != MLX5_SRIOV_UNLOAD_MAGIC) {
		mlx5_core_warn(dev, "cannot modify number of VFs while VFs are assigned\n");
		return -EPERM;
	}

	if (num_vfs == MLX5_SRIOV_UNLOAD_MAGIC)
		nvf = 0;
	else
		nvf = num_vfs;

	mlx5_destroy_vfs_sysfs(dev);

	if (!nvf) {
		mlx5_eswitch_disable_sriov(dev);
		kfree(sriov->vfs_ctx);
		sriov->vfs_ctx = NULL;
		if (!pci_vfs_assigned(pdev))
			pci_disable_sriov(pdev);
		else
			pr_info("unloading PF driver while leaving orphan VFs\n");

		return 0;
	}

	err = mlx5_core_sriov_enable(pdev, nvf);
	if (err) {
		dev_warn(&pdev->dev, "mlx5_core_sriov_enable failed %d\n", err);
		return err;
	}

	err = mlx5_create_vfs_sysfs(dev, nvf);
	if (err) {
		mlx5_core_warn(dev, "failed to create sysfs\n");
		num_vfs = 0;
		goto again;
	}

	mlx5_eswitch_enable_sriov(dev, nvf);
	return nvf;
}

struct guid_attribute {
	struct attribute attr;
	ssize_t (*show)(struct mlx5_sriov_vf *, struct guid_attribute *, char *buf);
	ssize_t (*store)(struct mlx5_sriov_vf *, struct guid_attribute *,
			 const char *buf, size_t count);
};

static ssize_t guid_attr_show(struct kobject *kobj,
			      struct attribute *attr, char *buf)
{
	struct guid_attribute *ga =
		container_of(attr, struct guid_attribute, attr);
	struct mlx5_sriov_vf *g = container_of(kobj, struct mlx5_sriov_vf, kobj);

	if (!ga->show)
		return -EIO;

	return ga->show(g, ga, buf);
}

static ssize_t guid_attr_store(struct kobject *kobj,
			       struct attribute *attr,
			       const char *buf, size_t size)
{
	struct guid_attribute *ga =
		container_of(attr, struct guid_attribute, attr);
	struct mlx5_sriov_vf *g = container_of(kobj, struct mlx5_sriov_vf, kobj);

	if (!ga->store)
		return -EIO;

	return ga->store(g, ga, buf, size);
}

static ssize_t port_show(struct mlx5_sriov_vf *g, struct guid_attribute *oa,
			 char *buf)
{
	struct mlx5_core_dev *dev = g->dev;
	union ib_gid gid;
	int err;
	u8 *p;

	err = mlx5_core_query_gids(dev, 1, 1, g->vf, 0 , &gid);
	if (err) {
		mlx5_core_warn(dev, "failed to query gid at index 0 for vf %d\n", g->vf);
		return err;
	}

	p = &gid.raw[8];
	err = sprintf(buf, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
		      p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
	return err;
}

static ssize_t port_store(struct mlx5_sriov_vf *g, struct guid_attribute *oa,
			  const char *buf, size_t count)
{
	struct mlx5_core_dev *dev = g->dev;
	struct mlx5_hca_vport_context *in;
	u64 guid = 0;
	int err;
	int tmp[8];
	int i;

	err = sscanf(buf, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
		     &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4], &tmp[5], &tmp[6], &tmp[7]);
	if (err != 8)
		return -EINVAL;

	for (i = 0; i < 8; i++)
		guid += ((u64)tmp[i] << ((7 - i) * 8));

	in = kzalloc(sizeof(*in), GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	in->field_select = MLX5_HCA_VPORT_SEL_PORT_GUID;
	in->port_guid = guid;
	err = mlx5_core_modify_hca_vport_context(dev, 1, 1, g->vf, in);
	kfree(in);
	if (err)
		return err;

	return count;
}

static ssize_t node_show(struct mlx5_sriov_vf *g, struct guid_attribute *oa,
			 char *buf)
{
	struct mlx5_core_dev *dev = g->dev;
	struct mlx5_hca_vport_context *rep;
	__be64 guid;

	int err;
	u8 *p;

	rep = kzalloc(sizeof(*rep), GFP_KERNEL);
	if (!rep) {
		err = -ENOMEM;
		goto out;
	}

	err = mlx5_core_query_hca_vport_context(dev, 1, 1,  g->vf, rep);
	if (err) {
		mlx5_core_warn(dev, "failed to query node guid for vf %d (%d)\n",
			       g->vf, err);
		goto free;
	}

	guid = cpu_to_be64(rep->node_guid);
	p = (u8 *)&guid;
	err = sprintf(buf, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
		      p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);

free:
	kfree(rep);
out:
	return err;
}

static ssize_t node_store(struct mlx5_sriov_vf *g, struct guid_attribute *oa,
			  const char *buf, size_t count)
{
	struct mlx5_core_dev *dev = g->dev;
	struct mlx5_hca_vport_context *in;
	u64 guid = 0;
	int err;
	int tmp[8];
	int i;

	err = sscanf(buf, "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
		     &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4], &tmp[5], &tmp[6], &tmp[7]);
	if (err != 8)
		return -EINVAL;

	for (i = 0; i < 8; i++)
		guid += ((u64)tmp[i] << ((7 - i) * 8));

	in = kzalloc(sizeof(*in), GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	in->field_select = MLX5_HCA_VPORT_SEL_NODE_GUID;
	in->node_guid = guid;
	err = mlx5_core_modify_hca_vport_context(dev, 1, 1, g->vf, in);
	kfree(in);
	return count;
}

static const char *policy_str(enum port_state_policy policy)
{
	switch (policy) {
	case MLX5_POLICY_DOWN:		return "Down\n";
	case MLX5_POLICY_UP:		return "Up\n";
	case MLX5_POLICY_FOLLOW:	return "Follow\n";
	}
	return "Invalid policy\n";
}

static ssize_t policy_show(struct mlx5_sriov_vf *g, struct guid_attribute *oa,
			   char *buf)
{
	struct mlx5_core_dev *dev = g->dev;
	struct mlx5_hca_vport_context *rep;
	const char *p = NULL;
	int err;

	rep = kzalloc(sizeof(*rep), GFP_KERNEL);
	if (!rep)
		return -ENOMEM;

	err = mlx5_core_query_hca_vport_context(dev, 1, 1,  g->vf, rep);
	if (err) {
		mlx5_core_warn(dev, "failed to query port policy for vf %d (%d)\n",
			       g->vf, err);
		goto free;
	}
	p = policy_str(rep->policy);
	strcpy(buf, p);

free:
	kfree(rep);
	return p ? strlen(p) : err;
}

static int strpolicy(const char *buf, enum port_state_policy *policy)
{
	if (!strcmp(buf, "Down\n")) {
		*policy = MLX5_POLICY_DOWN;
		return 0;
	}

	if (!strcmp(buf, "Up\n")) {
		*policy = MLX5_POLICY_UP;
		return 0;
	}

	if (!strcmp(buf, "Follow\n")) {
		*policy = MLX5_POLICY_FOLLOW;
		return 0;
	}
	return -EINVAL;
}

static ssize_t policy_store(struct mlx5_sriov_vf *g, struct guid_attribute *oa,
			    const char *buf, size_t count)
{
	struct mlx5_core_dev *dev = g->dev;
	struct mlx5_hca_vport_context *in;
	enum port_state_policy policy;
	int err;

	err = strpolicy(buf, &policy);
	if (err)
		return err;

	in = kzalloc(sizeof(*in), GFP_KERNEL);
	if (!in)
		return -ENOMEM;

	in->policy = policy;
	in->field_select = MLX5_HCA_VPORT_SEL_STATE_POLICY;
	err = mlx5_core_modify_hca_vport_context(dev, 1, 1, g->vf, in);
	kfree(in);
	if (err)
		return err;

	return count;
}

#ifndef CONFIG_COMPAT_IS_CONST_KOBJECT_SYSFS_OPS
static struct sysfs_ops guid_sysfs_ops = {
#else
static const struct sysfs_ops guid_sysfs_ops = {
#endif
	.show = guid_attr_show,
	.store = guid_attr_store,
};

#define GUID_ATTR(_name) struct guid_attribute guid_attr_##_name = \
	__ATTR(_name, 0644, _name##_show, _name##_store)

GUID_ATTR(node);
GUID_ATTR(port);
GUID_ATTR(policy);

static struct attribute *guid_default_attrs[] = {
	&guid_attr_node.attr,
	&guid_attr_port.attr,
	&guid_attr_policy.attr,
	NULL
};

static struct kobj_type guid_type = {
	.sysfs_ops     = &guid_sysfs_ops,
	.default_attrs = guid_default_attrs
};

static int mlx5_create_vfs_sysfs(struct mlx5_core_dev *dev, int num_vfs)
{
	struct mlx5_core_sriov *sriov = &dev->priv.sriov;
	struct mlx5_sriov_vf *tmp;
	int err;
	int vf;

	sriov->vfs = kcalloc(num_vfs, sizeof(*sriov->vfs), GFP_KERNEL);
	if (!sriov->vfs)
		return -ENOMEM;

	for (vf = 0; vf < num_vfs; vf++) {
		tmp = &sriov->vfs[vf];
		tmp->dev = dev;
		tmp->vf = vf;
		err = kobject_init_and_add(&tmp->kobj, &guid_type, sriov->config,
					   "%d", vf);
		if (err)
			goto err_vf;

		kobject_uevent(&tmp->kobj, KOBJ_ADD);
	}
	sriov->num_vfs = num_vfs;

	return 0;

err_vf:
	for (; vf >= 1; vf--) {
		tmp = &sriov->vfs[vf - 1];
		kobject_put(&tmp->kobj);
	}
	kfree(sriov->vfs);
	sriov->vfs = NULL;
	return err;
}

static void mlx5_destroy_vfs_sysfs(struct mlx5_core_dev *dev)
{
	struct mlx5_core_sriov *sriov = &dev->priv.sriov;
	struct mlx5_sriov_vf *tmp;
	int vf;

	mlx5_core_free_vfs(dev);
	for (vf = 1; vf <= sriov->num_vfs; vf++) {
		tmp = &sriov->vfs[vf - 1];
		kobject_put(&tmp->kobj);
	}
	sriov->num_vfs = 0;
	kfree(sriov->vfs);
	sriov->vfs = NULL;
}

static ssize_t num_vf_store(struct device *device, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct pci_dev *pdev = container_of(device, struct pci_dev, dev);
	int req_vfs;
	int err;

	if (kstrtoint(buf, 0, &req_vfs) || req_vfs < 0
#ifdef HAVE_PCI_SRIOV_GET_TOTALVFS
	    || req_vfs > pci_sriov_get_totalvfs(pdev))
#else
	)
#endif
		return -EINVAL;

	err = mlx5_core_sriov_configure(pdev, req_vfs);
	if (err < 0)
		return err;

	return count;
}

static ssize_t num_vf_show(struct device *device, struct device_attribute *attr,
			   char *buf)
{
	struct pci_dev *pdev = container_of(device, struct pci_dev, dev);
	struct mlx5_core_dev *dev  = pci_get_drvdata(pdev);
	struct mlx5_core_sriov *sriov = &dev->priv.sriov;

	return sprintf(buf, "%d\n", sriov->num_vfs);
}

static DEVICE_ATTR(mlx5_num_vfs, 0600, num_vf_show, num_vf_store);

static struct device_attribute *mlx5_class_attributes[] = {
	&dev_attr_mlx5_num_vfs,
};

static int mlx5_sriov_sysfs_init(struct mlx5_core_dev *dev)
{
	struct mlx5_core_sriov *sriov = &dev->priv.sriov;
	struct device *device = &dev->pdev->dev;
	int err;
	int i;

	sriov->config = kobject_create_and_add("sriov", &device->kobj);
	if (!sriov->config)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(mlx5_class_attributes); i++) {
		err = device_create_file(device, mlx5_class_attributes[i]);
		if (err)
			goto err_attr;
	}

	return 0;

err_attr:
	kobject_put(sriov->config);
	sriov->config = NULL;
	return err;
}

static void mlx5_sriov_sysfs_cleanup(struct mlx5_core_dev *dev)
{
	struct mlx5_core_sriov *sriov = &dev->priv.sriov;
	struct device *device = &dev->pdev->dev;
	int i;

	for (i = 0; i < ARRAY_SIZE(mlx5_class_attributes); i++)
		device_remove_file(device, mlx5_class_attributes[i]);

	kobject_put(sriov->config);
	sriov->config = NULL;
}

static int sync_req(struct pci_dev *pdev)
{
	struct mlx5_core_dev *dev  = pci_get_drvdata(pdev);
	struct mlx5_core_sriov *sriov = &dev->priv.sriov;
	int cur_vfs = pci_num_vf(pdev);

	if (cur_vfs != sriov->num_vfs) {
		pr_info("current VFs %d, registered %d - sync needed\n", cur_vfs, sriov->num_vfs);
		return 1;
	}

	return 0;
}

int mlx5_sriov_init(struct mlx5_core_dev *dev)
{
	struct mlx5_core_sriov *sriov = &dev->priv.sriov;
	struct pci_dev *pdev = dev->pdev;
	int cur_vfs;
	int err;
	int vf;

	if (!mlx5_core_is_pf(dev))
		return 0;

	err = mlx5_sriov_sysfs_init(dev);
	if (err)
		return err;

	if (!sync_req(dev->pdev))
		return 0;

	cur_vfs = pci_num_vf(pdev);
	sriov->vfs_ctx = kcalloc(cur_vfs, sizeof(*sriov->vfs_ctx), GFP_KERNEL);
	if (!sriov->vfs_ctx)
		return -ENOMEM;

	sriov->enabled_vfs = cur_vfs;

	err = mlx5_create_vfs_sysfs(dev, cur_vfs);
	if (err) {
		mlx5_core_warn(dev, "failed to restore sriov sysyfs\n");
		sriov->enabled_vfs = 0;
		sriov->num_vfs = 0;
		kfree(sriov->vfs_ctx);
		sriov->vfs_ctx = NULL;
		return err;
	}

	for (vf = 1; vf <= cur_vfs; vf++) {
		err = mlx5_core_enable_hca(dev, vf);
		if (err) {
			mlx5_core_warn(dev, "failed to enable VF %d\n", vf - 1);
		} else {
			pr_info("enable vf %d success\n", vf - 1);
			sriov->vfs_ctx[vf - 1].enabled = 1;
		}
	}

	return 0;
}

int mlx5_sriov_cleanup(struct mlx5_core_dev *dev)
{
	struct pci_dev *pdev = dev->pdev;
	int err;

	if (!mlx5_core_is_pf(dev))
		return 0;

	err = mlx5_core_sriov_configure(pdev, MLX5_SRIOV_UNLOAD_MAGIC);
	if (err)
		return err;

	mlx5_sriov_sysfs_cleanup(dev);
	return 0;
}
