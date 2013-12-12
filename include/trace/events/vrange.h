#undef TRACE_SYSTEM
#define TRACE_SYSTEM vrange

#if !defined(_TRACE_VRANGE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_VRANGE_H

#include <linux/tracepoint.h>
#include <linux/vrange.h>

TRACE_EVENT(shrink_vrange,
	TP_PROTO(struct lruvec *lruvec, struct scan_control *sc,
									unsigned long nr_reclaimed),
	TP_ARGS(lruvec, sc, nr_reclaimed),

	TP_STRUCT__entry(
		__field(int, zone_id)
		__field(int, priority )
		__field(unsigned long, nr_to_reclaim )
		__field(unsigned long, nr_reclaimed )
	),

	TP_fast_assign(
		__entry->zone_id = zone_idx(lruvec_zone(lruvec));
		__entry->priority = sc->priority;
		__entry->nr_to_reclaim = sc->nr_to_reclaim;
		__entry->nr_reclaimed = nr_reclaimed;
	),

	TP_printk("zone_id=%d priority=%d nr_to_reclaim=%lu nr_reclaimed=%lu",
						__entry->zone_id, __entry->priority,
						__entry->nr_to_reclaim, __entry->nr_reclaimed)
);

TRACE_EVENT(discard_vrange,
	TP_PROTO(struct vrange *range, unsigned int nr_discard),
	TP_ARGS(range, nr_discard),

	TP_STRUCT__entry(
		__field(unsigned long, size)
		__field(unsigned int, nr_discard)
	),

	TP_fast_assign(
		__entry->size = (range->node.last + 1 - range->node.start)
						>> PAGE_SHIFT;
		__entry->nr_discard = nr_discard;
	),

	TP_printk("scan=%lu nr_discard=%u", __entry->size, __entry->nr_discard)
);

#endif /* _TRACE_VRANGE_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
