/* Public domain. */

#ifndef _LINUX_MMZONE_H
#define _LINUX_MMZONE_H

#include <linux/mm_types.h>
#include <linux/nodemask.h>

#define MAX_ORDER	11
#define NR_PAGE_ORDERS	(MAX_ORDER + 1)
#define pfn_to_nid(x)	0

#endif
