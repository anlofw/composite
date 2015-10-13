/***
 * Copyright 2011-2015 by Gabriel Parmer.  All rights reserved.
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 *
 * Author: Gabriel Parmer, gparmer@gwu.edu, 2011
 */

#ifndef PS_GLOBAL_H
#define PS_GLOBAL_H

#include <ps_plat.h>

/* 
 * Lists of free memory.  The slab freelist is all slabs that have at
 * least one free object in them.  The qsc_list is a quiescence list
 * of memory that has been freed, but might still have references to
 * it (ala parsec).
 */
struct ps_slab;
struct ps_slab_freelist {
	struct ps_slab    *list;
};

/* Memory header */
struct ps_mheader {
	ps_tsc_t           tsc_free;
	struct ps_slab    *slab;	       /* slab header ptr */
	struct ps_mheader *next;	       /* slab freelist ptr */
} PS_PACKED;

static inline struct ps_mheader *
__ps_mhead_get(void *mem)
{ return (struct ps_mheader *)((char*)mem - sizeof(struct ps_mheader)); }

static inline int
__ps_mhead_isfree(struct ps_mheader *h)
{ return h->tsc_free != 0; }

static inline void
__ps_mhead_init(struct ps_mheader *h, struct ps_slab *s)
{
	h->tsc_free = 0;
	h->slab     = s;
	h->next     = NULL;
}

static inline void
__ps_mhead_reset(struct ps_mheader *h)
{
	h->tsc_free = 0;
	h->next     = NULL;
}

/* If you don't need memory anymore, set it free! */
static inline void
__ps_mhead_setfree(struct ps_mheader *h)
{
	/* TODO: atomic w/ error out */
	h->tsc_free  = ps_tsc() | 1; /* guarantee non-zero */
}

struct ps_qsc_list {
	struct ps_mheader *head, *tail;	
};

static inline struct ps_mheader *
__ps_qsc_peek(struct ps_qsc_list *ql)
{ return ql->head; }

static inline void
__ps_qsc_enqueue(struct ps_qsc_list *ql, struct ps_mheader *n)
{
	struct ps_mheader *t;

	t = ql->tail;
	if (likely(t)) t->next  = ql->tail = n;
	else           ql->head = ql->tail = n;
}

static inline struct ps_mheader *
__ps_qsc_dequeue(struct ps_qsc_list *ql)
{
	struct ps_mheader *a = ql->head;

	if (a) {
		ql->head = a->next;
		if (unlikely(ql->tail == a)) ql->tail = NULL;
	}
	return a;
}

struct parsec;
/*
 * TODO:
 * 1. save memory by packing multiple freelists into the same
 * cache-line
 * 2. have multiple freelists (e.g. 4) for different "fullness"
 * values, so that we can in O(1) always allocate from the slab that
 * is most full, modulo the granularity of these bins.
 * 3. implement the slab to allocate the headers for the other slab.
 *
 * Note: some of these TODOs are more applicable to the
 * ps_slab_freelist.
 * 
 * Note: the padding is for two cache-lines due to the observed
 * behavior on Intel chips to aggressively prefetch an additional
 * cache-line.
 */
struct ps_mem_percore {
	struct ps_slab_freelist fl;	   /* freelist of slabs with available objects */
	struct ps_slab_freelist slabheads; /* only used if hintern == 0 */
	struct ps_qsc_list      qsc_list;  /* queue of freed, but not quiesced memory */
	struct parsec          *ps;        /* the parallel section that wraps this memory, or NULL */
	size_t                  qmemcnt;   /* # of items in the qsc_list */
	size_t                  smemcnt;   /* # of items free in the slabs */
	char padding[PS_CACHE_PAD-((sizeof(struct ps_slab_freelist)*2 + sizeof(struct ps_qsc_list) + sizeof(struct parsec *) + 2*sizeof(size_t))%PS_CACHE_PAD)];

	/* Isolate the contended cache-lines from the common-case ones. */
	struct ps_lock          lock;
	struct ps_qsc_list      remote_frees;
	char padding[PS_CACHE_PAD-(sizeof(int) + sizeof(struct ps_qsc_list))];
} PS_PACKED PS_ALIGNED;

#define PS_MEM_CREATE_DATA(name)					\
struct ps_mem_percore slab_##name##_freelist[PS_NUMCORES] PS_ALIGNED;

#endif	/* PS_GLOBAL_H */
