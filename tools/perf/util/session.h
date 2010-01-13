#ifndef __PERF_SESSION_H
#define __PERF_SESSION_H

#include "event.h"
#include "header.h"
#include "thread.h"
#include <linux/rbtree.h>
#include "../../../include/linux/perf_event.h"

struct ip_callchain;
struct thread;
struct symbol;

struct perf_session {
	struct perf_header	header;
	unsigned long		size;
	unsigned long		mmap_window;
	struct map_groups	kmaps;
	struct rb_root		threads;
	struct thread		*last_match;
	struct map		*vmlinux_maps[MAP__NR_TYPES];
	struct events_stats	events_stats;
	unsigned long		event_total[PERF_RECORD_MAX];
	unsigned long		unknown_events;
	struct rb_root		hists;
	u64			sample_type;
	struct {
		const char	*name;
		u64		addr;
	}			ref_reloc_sym;
	int			fd;
	int			cwdlen;
	char			*cwd;
	char filename[0];
};

typedef int (*event_op)(event_t *self, struct perf_session *session);

struct perf_event_ops {
	event_op sample,
		 mmap,
		 comm,
		 fork,
		 exit,
		 lost,
		 read,
		 throttle,
		 unthrottle;
};

struct perf_session *perf_session__new(const char *filename, int mode, bool force);
void perf_session__delete(struct perf_session *self);

int perf_session__process_events(struct perf_session *self,
				 struct perf_event_ops *event_ops);

struct symbol **perf_session__resolve_callchain(struct perf_session *self,
						struct thread *thread,
						struct ip_callchain *chain,
						struct symbol **parent);

bool perf_session__has_traces(struct perf_session *self, const char *msg);

int perf_header__read_build_ids(int input, u64 offset, u64 file_size);

int perf_session__set_kallsyms_ref_reloc_sym(struct perf_session *self,
					     const char *symbol_name,
					     u64 addr);
void perf_session__reloc_vmlinux_maps(struct perf_session *self,
				      u64 unrelocated_addr);

#endif /* __PERF_SESSION_H */
