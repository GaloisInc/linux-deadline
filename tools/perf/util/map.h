#ifndef __PERF_MAP_H
#define __PERF_MAP_H

#include <linux/compiler.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/types.h>

enum map_type {
	MAP__FUNCTION = 0,
	MAP__VARIABLE,
};

#define MAP__NR_TYPES (MAP__VARIABLE + 1)

struct dso;

struct map {
	union {
		struct rb_node	rb_node;
		struct list_head node;
	};
	u64			start;
	u64			end;
	enum map_type		type;
	u64			pgoff;
	u64			(*map_ip)(struct map *, u64);
	u64			(*unmap_ip)(struct map *, u64);
	struct dso		*dso;
};

static inline u64 map__map_ip(struct map *map, u64 ip)
{
	return ip - map->start + map->pgoff;
}

static inline u64 map__unmap_ip(struct map *map, u64 ip)
{
	return ip + map->start - map->pgoff;
}

static inline u64 identity__map_ip(struct map *map __used, u64 ip)
{
	return ip;
}

struct symbol;
struct mmap_event;

typedef int (*symbol_filter_t)(struct map *map, struct symbol *sym);

void map__init(struct map *self, enum map_type type,
	       u64 start, u64 end, u64 pgoff, struct dso *dso);
struct map *map__new(struct mmap_event *event, enum map_type,
		     char *cwd, int cwdlen);
void map__delete(struct map *self);
struct map *map__clone(struct map *self);
int map__overlap(struct map *l, struct map *r);
size_t map__fprintf(struct map *self, FILE *fp);

struct perf_session;

int map__load(struct map *self, struct perf_session *session,
	      symbol_filter_t filter);
struct symbol *map__find_symbol(struct map *self, struct perf_session *session,
				u64 addr, symbol_filter_t filter);
struct symbol *map__find_symbol_by_name(struct map *self, const char *name,
					struct perf_session *session,
					symbol_filter_t filter);
void map__fixup_start(struct map *self);
void map__fixup_end(struct map *self);

#endif /* __PERF_MAP_H */
