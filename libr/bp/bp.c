/* radare2 - LGPL - Copyright 2009-2014 - pancake */

#include <r_bp.h>
#include "../config.h"

R_LIB_VERSION (r_bp);

static struct r_bp_plugin_t *bp_static_plugins[] = 
	{ R_BP_STATIC_PLUGINS };

static void r_bp_item_free (RBreakpointItem *b) {
	free (b->bbytes);
	free (b->obytes);
	free (b);
}

R_API RBreakpoint *r_bp_new() {
	RBreakpointPlugin *static_plugin;
	RBreakpoint *bp = R_NEW (RBreakpoint);
	if (bp) {
		int i;
		bp->cur = NULL;
		bp->nbps = 0;
		bp->trace_bp = R_FALSE;
		bp->stepcont = R_BP_CONT_NORMAL;
		bp->breakpoint = NULL;
		bp->endian = 0; // little by default
		bp->traces = r_bp_traptrace_new ();
		bp->printf = (PrintfCallback)printf;
		bp->bps = r_list_newf ((RListFree)r_bp_item_free);
		bp->plugins = r_list_newf ((RListFree)free);
		for (i=0; bp_static_plugins[i]; i++) {
			static_plugin = R_NEW (RBreakpointPlugin);
			memcpy (static_plugin, bp_static_plugins[i],
				sizeof (RBreakpointPlugin));
			r_bp_plugin_add (bp, static_plugin);
		}
		memset (&bp->iob, 0, sizeof (bp->iob));
	}
	return bp;
}

R_API RBreakpoint *r_bp_free(RBreakpoint *bp) {
	r_list_free (bp->bps);
	r_list_free (bp->plugins);
	free (bp);
	return NULL;
}

R_API int r_bp_get_bytes(RBreakpoint *bp, ut8 *buf, int len, int endian, int idx) {
	int i;
	struct r_bp_arch_t *b;
	if (bp->cur) {
		// find matching size breakpoint
		for (i=0; i<bp->cur->nbps; i++) {
			b = &bp->cur->bps[i];
			if (bp->cur->bps[i].length == len) {
				memcpy (buf, b->bytes, b->length);
				return b->length;
			}
		}
		// TODO: this must be reworked to work better
		/* if not found try to pad with the first one */
		b = &bp->cur->bps[0];
		if (len % b->length) {
			eprintf ("No matching bpsize\n");
			return 0;
		}
		for (i=0;i<len;i++) {
			memcpy (buf+i, b->bytes, b->length);
		}
		return b->length;
	}
	return 0;
}

R_API RBreakpointItem *r_bp_get(RBreakpoint *bp, ut64 addr) {
	RListIter *iter;
	RBreakpointItem *b;
	r_list_foreach(bp->bps, iter, b)
		if (b->addr == addr)
			return b;
	return NULL;
}

R_API RBreakpointItem *r_bp_at_addr(RBreakpoint *bp, ut64 addr, int rwx) {
	RBreakpointItem *b;
	RListIter *iter;
	r_list_foreach (bp->bps, iter, b) {
		// eprintf ("---ataddr--- 0x%08"PFMT64x" %d %d %x\n", b->addr, b->size, b->recoil, b->rwx);
		//Check addr within range and provided rwx matches (or null)
		if (addr>=b->addr && addr<=(b->addr+b->size) && (!rwx || rwx&b->rwx))
			return b;
	}
	return NULL;
}

R_API struct r_bp_item_t *r_bp_enable(RBreakpoint *bp, ut64 addr, int set) {
	RListIter *iter;
	RBreakpointItem *b;
	r_list_foreach(bp->bps, iter, b) {
		if (addr >= b->addr && addr <= b->addr+b->size) {
			b->enabled = set;
			return b;
		}
	}
	return NULL;
}

R_API int r_bp_stepy_continuation(RBreakpoint *bp) {
	// TODO: implement
	return bp->stepcont;
}

/* TODO: detect overlapping of breakpoints */
static RBreakpointItem *r_bp_add(RBreakpoint *bp, const ut8 *obytes, ut64 addr, int size, int hw, int rwx) {
	int ret;
	RBreakpointItem *b;
	if (r_bp_at_addr (bp, addr, rwx)) {
		eprintf ("Breakpoint already set at this address.\n");
		return NULL;
	}
	b = R_NEW (RBreakpointItem);
	b->pids[0] = 0; /* for any pid */
	b->addr = addr;
	b->data = NULL;
	b->size = size;
	b->enabled = R_TRUE;
	b->hw = hw;
	b->trace = 0;

	if (hw) {
		b->bbytes = NULL;
		b->obytes = NULL;
		b->recoil = 0;
	} else {
		b->bbytes = malloc (size+16);
		if (obytes) {
			b->obytes = malloc (size);
			memcpy (b->obytes, obytes, size);
		} else b->obytes = NULL;
		/* XXX: endian .. use bp->endian */
		// XXX for hw breakpoints there are no bytes
		ret = r_bp_get_bytes (bp, b->bbytes, size, 0, 0);
		if (ret == 0) {
			eprintf ("Cannot get breakpoint bytes. No r_bp_use()?\n");
			r_bp_item_free (b);
			return NULL;
		}
		b->recoil = ret;
	}

	bp->nbps++;
	r_list_append (bp->bps, b);
	return b;
}

R_API int r_bp_add_fault(RBreakpoint *bp, ut64 addr, int size, int rwx) {
	// TODO
	return R_FALSE;
}

R_API struct r_bp_item_t *r_bp_add_sw(RBreakpoint *bp, ut64 addr, int size, int rwx) {
	RBreakpointItem *item;
	ut8 *bytes;
	if (size<1)
		size = 1;
	bytes = malloc (size);
	if (bytes == NULL)
		return NULL;
	if (bp->iob.read_at)
		bp->iob.read_at (bp->iob.io, addr, bytes, size);
	else memset (bytes, 0, size);
	item = r_bp_add (bp, bytes, addr, size, R_BP_TYPE_SW, rwx);
	free (bytes);
	return item;
}

R_API struct r_bp_item_t *r_bp_add_hw(RBreakpoint *bp, ut64 addr, int size, int rwx) {
	return r_bp_add (bp, NULL, addr, size, R_BP_TYPE_HW, rwx);
}

R_API int r_bp_del_all(RBreakpoint *bp) {
	if (r_list_empty (bp->bps))
		return R_FALSE;
	r_list_purge (bp->bps);
	return R_TRUE;
}

R_API int r_bp_del(RBreakpoint *bp, ut64 addr) {
	RListIter *iter;
	RBreakpointItem *b;
	/* No _safe loop necessary because we return immediately after the delete. */
	r_list_foreach (bp->bps, iter, b) {
		if (b->addr == addr) {
			r_list_delete (bp->bps, iter);
			return R_TRUE;
		}
	}
	return R_FALSE;
}

// TODO: rename or drop?
// TODO: use a r_bp_item instead of address
// TODO: we can just drop it.. its just b->trace = R_TRUE or so..
R_API int r_bp_set_trace(RBreakpoint *bp, ut64 addr, int set) {
	RListIter *iter;
	RBreakpointItem *b;
	r_list_foreach (bp->bps, iter, b) {
		if (addr >= b->addr && addr <= b->addr+b->size) {
			b->trace = set;
			return R_TRUE;
		}
	}
	return R_FALSE;
}

#if 0
// TODO: rename or remove
R_API int r_bp_set_trace_bp(RBreakpoint *bp, ut64 addr, int set)
{
	bp->trace_all = set;
	bp->trace_bp = addr;
	return R_TRUE;
}
#endif

// TODO: deprecate
R_API int r_bp_list(RBreakpoint *bp, int rad) {
	int n = 0;
	RBreakpointItem *b;
	RListIter *iter;
	//eprintf ("Breakpoint list:\n");
	r_list_foreach (bp->bps, iter, b) {
		bp->printf ("0x%08"PFMT64x" - 0x%08"PFMT64x" %d %c%c%c %s %s %s cmd=\"%s\"\n",
			b->addr, b->addr+b->size, b->size,
			(b->rwx & R_BP_PROT_READ)? 'r': '-',
			(b->rwx & R_BP_PROT_WRITE)? 'w': '-',
			(b->rwx & R_BP_PROT_EXEC)? 'x': '-',
			b->hw? "hw": "sw",
			b->trace? "trace": "break",
			b->enabled? "enabled": "disabled",
			b->data? b->data: "");
		/* TODO: Show list of pids and trace points, conditionals */
		n++;
	}
	return n;
}
