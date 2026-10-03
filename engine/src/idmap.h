/* Map from a 32-bit id (session id) to a small index: open addressing with
 * linear probing and backward-shift deletion (no tombstones). The table has
 * at least twice as many slots as entries, so probes always end.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CG_IDMAP_H
#define CG_IDMAP_H

#include <stdint.h>
#include <stdlib.h>

struct cg_idmap_slot {
	uint32_t id;
	int32_t idx; /* < 0: empty */
};

struct cg_idmap {
	struct cg_idmap_slot *slot;
	uint32_t mask;
};

static inline uint32_t cg_idmap_home(const struct cg_idmap *m, uint32_t id)
{
	return (id * 2654435761u) & m->mask;
}

/* Room for max_entries ids. Returns 0 or -1. */
static inline int cg_idmap_init(struct cg_idmap *m, uint32_t max_entries)
{
	uint32_t cap = 2;

	m->mask = 0;
	while (cap < 2 * max_entries)
		cap <<= 1;
	m->slot = malloc(sizeof(*m->slot) * cap);
	if (!m->slot)
		return -1;
	for (uint32_t i = 0; i < cap; i++)
		m->slot[i].idx = -1;
	m->mask = cap - 1;
	return 0;
}

static inline void cg_idmap_free(struct cg_idmap *m)
{
	free(m->slot);
	m->slot = NULL;
}

static inline int32_t cg_idmap_get(const struct cg_idmap *m, uint32_t id)
{
	for (uint32_t i = cg_idmap_home(m, id);; i = (i + 1) & m->mask) {
		if (m->slot[i].idx < 0)
			return -1;
		if (m->slot[i].id == id)
			return m->slot[i].idx;
	}
}

/* id must not be present and the table must not be full. */
static inline void cg_idmap_put(struct cg_idmap *m, uint32_t id, int32_t idx)
{
	uint32_t i = cg_idmap_home(m, id);

	while (m->slot[i].idx >= 0)
		i = (i + 1) & m->mask;
	m->slot[i].id = id;
	m->slot[i].idx = idx;
}

static inline void cg_idmap_del(struct cg_idmap *m, uint32_t id)
{
	uint32_t i = cg_idmap_home(m, id), j;

	while (m->slot[i].idx >= 0 && m->slot[i].id != id)
		i = (i + 1) & m->mask;
	if (m->slot[i].idx < 0)
		return;
	for (j = (i + 1) & m->mask; m->slot[j].idx >= 0; j = (j + 1) & m->mask) {
		uint32_t home = cg_idmap_home(m, m->slot[j].id);

		/* Move j into the hole at i unless its home lies in (i, j]. */
		if (((j - home) & m->mask) >= ((j - i) & m->mask)) {
			m->slot[i] = m->slot[j];
			i = j;
		}
	}
	m->slot[i].idx = -1;
}

#endif
