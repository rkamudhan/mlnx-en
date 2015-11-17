#ifndef _COMPAT_LINUX_SKBUFF_H
#define _COMPAT_LINUX_SKBUFF_H

#include_next <linux/skbuff.h>

#ifndef HAVE_SKB_PULL_INLINE
static inline unsigned char *skb_pull_inline(struct sk_buff *skb, unsigned int len)
{
	return unlikely(len > skb->len) ? NULL : __skb_pull(skb, len);
}
#endif /* HAVE_SKB_PULL_INLINE */

#ifndef SKB_TRUESIZE
#define SKB_TRUESIZE(X) ((X) +						\
			SKB_DATA_ALIGN(sizeof(struct sk_buff)) +	\
			SKB_DATA_ALIGN(sizeof(struct skb_shared_info)))
#endif

#endif /* _COMPAT_LINUX_SKBUFF_H */
