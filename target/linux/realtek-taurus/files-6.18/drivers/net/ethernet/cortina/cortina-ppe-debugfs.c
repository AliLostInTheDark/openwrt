// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only debugfs dump of the offloaded flows: one line per hash entry with
 * its tuple, and the key and action words as the engine reads them.
 */

#include <linux/debugfs.h>
#include <linux/in.h>
#include <linux/jiffies.h>
#include <linux/seq_file.h>

#include "cortina-ni.h"
#include "cortina-ppe.h"

static int cortina_ppe_entries_show(struct seq_file *m, void *private)
{
	struct cortina_ppe *ppe = m->private;
	u32 idx;
	int i;

	mutex_lock(&ppe->flow_lock);
	seq_printf(m, "flows %d (ds %d), pppoe session %#x, data gem %u t-cont %u\n",
		   atomic_read(&ppe->stats.installed),
		   atomic_read(&ppe->stats.ds_installed),
		   ppe->pppoe_session, ppe->data_gem, ppe->data_tcont);

	for (idx = 0; idx < PPE_ENTRIES; idx++) {
		const struct cortina_ppe_flow *e = ppe->flow_by_idx[idx];
		const __le32 *act;

		if (!e)
			continue;

		act = ppe->act_tbl + (size_t)idx * PPE_ACT_BYTES;
		if (e->tuple.ipv6)
			seq_printf(m, "%05u %s %s [%pI6c]:%u->[%pI6c]:%u",
				   idx, e->ds ? "DS" : "US",
				   e->tuple.proto == IPPROTO_TCP ? "tcp" : "udp",
				   &e->tuple.sip6, e->tuple.sport,
				   &e->tuple.dip6, e->tuple.dport);
		else
			seq_printf(m, "%05u %s %s %pI4h:%u->%pI4h:%u",
				   idx, e->ds ? "DS" : "US",
				   e->tuple.proto == IPPROTO_TCP ? "tcp" : "udp",
				   &e->tuple.sip, e->tuple.sport,
				   &e->tuple.dip, e->tuple.dport);
		if (e->pppoe_sid)
			seq_printf(m, " pppoe %#x", e->pppoe_sid);
		seq_printf(m, " idle %ums key %08x act",
			   jiffies_to_msecs(jiffies - e->last_hit),
			   ppe->key_tbl[idx]);
		for (i = 0; i < PPE_ACT_BYTES / 4; i++)
			seq_printf(m, " %08x", le32_to_cpu(act[i]));
		seq_putc(m, '\n');
	}
	mutex_unlock(&ppe->flow_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(cortina_ppe_entries);

void cortina_ppe_debugfs_init(struct cortina_ppe *ppe, struct dentry *dir)
{
	if (ppe && dir)
		debugfs_create_file("ppe_entries", 0400, dir, ppe,
				    &cortina_ppe_entries_fops);
}
