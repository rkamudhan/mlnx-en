#ifndef _LINUX_PCI_H
#define _LINUX_PCI_H

#include_next <linux/pci.h>
#include <linux/compat-2.6.h>

#ifndef HAVE_PCI_PHYSFN
static inline struct pci_dev *pci_physfn(struct pci_dev *dev)
{
#ifdef CONFIG_PCI_IOV
	if (dev->is_virtfn)
		dev = dev->physfn;
#endif
	return dev;
}
#endif /* HAVE_PCI_PHYSFN */

#ifndef HAVE_PCI_NUM_VF
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 18))
int pci_num_vf(struct pci_dev *pdev);
#else
static inline int pci_num_vf(struct pci_dev *pdev)
{
	return 0;
}
#endif
#endif

#ifndef HAVE_PCI_VFS_ASSIGNED
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 18))
int pci_vfs_assigned(struct pci_dev *pdev);
#else
static inline int pci_vfs_assigned(struct pci_dev *pdev)
{
	return 0;
}
#endif
#endif

#ifndef HAVE_PCI_SRIOV_GET_TOTALVFS
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 18))
int pci_sriov_get_totalvfs(struct pci_dev *pdev);
#else
static inline int pci_sriov_get_totalvfs(struct pci_dev *pdev)
{
	return 0;
}
#endif
#endif

#endif /* _LINUX_PCI_H */
