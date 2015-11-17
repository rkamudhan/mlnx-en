#ifndef LINUX_4_0_COMPAT_H
#define LINUX_4_0_COMPAT_H

#include <linux/version.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0))
#include <linux/string.h>
#include <linux/uaccess.h>

static inline const char *kstrdup_const(const char *s, gfp_t gfp)
{
	return kstrdup(s, gfp);
}

static inline void kfree_const(const void *x)
{
	kfree(x);
}
#endif /* (LINUX_VERSION_CODE < KERNEL_VERSION(4,0,0)) */

#endif /* LINUX_4_0_COMPAT_H */
