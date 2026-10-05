/***************************************************************
 *  
 * project   ____   _____  ____   ____   ____     _     _   _
 *       ___|  _ \ |_   _|| _  \ / ___| / ___|   / \   | \ | |
 *      / __| |_) |  | |  | |_) |\___ \|| |     / _ \  |  \| |
 *     | (__|  __/   | |  |  _ <  ___) || |__  / ___ \ | |\  |
 *      \___|_|      |_|  |_| \_|\____/ \____//_/   \_\|_| \_|
 *
 * Copyright (C) 2026 kaidev, <kaidevonmail@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fs_ptrscan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

#include "vma/vm_area.h"
#include "vma/vma_select.h"

#define PM_INIT_CAP     4096
#define BOUNDARY_INIT   256
#define TRACE_MAX       1024
#define OFF_MAX         0x7FFFFFFFULL

#define PM_SLAB_SHIFT   12
#define PM_SLAB_SIZE    (1u << PM_SLAB_SHIFT)
#define PM_SLAB_MASK    (PM_SLAB_SIZE - 1)

/* Basics */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t hash_u64(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33; return x;
}

static void *map_zero(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
}

static void unmap_ptr(void *p, size_t bytes)
{
    if (p && bytes > 0) munmap(p, bytes);
}

/*
 * Parameter accessor: depth is a 1-based layer index.
 *   Layer 1 = slot that directly points to target
 *   Layer N = slot that points to a Layer N-1 slot
 *
 * max_off[0] is unused; max_off[1] applies to Layer 1, and so on.
 * This matches the user-visible "Layer N" and the -k/-t indices,
 * avoiding a 0-based internal vs 1-based UI mismatch.
 */
static inline uint64_t max_off_at(const struct fs_opts *o, int depth)
{
    if (!o->max_off || o->max_off_len <= 0) return 0x1000;
    if (depth < 1) depth = 1;
    if (depth >= o->max_off_len) depth = o->max_off_len - 1;
    return o->max_off[depth];
}

static inline int max_targets_at(const struct fs_opts *o, int depth)
{
    if (!o->max_targets_per_node || o->max_targets_per_node_len <= 0)
        return 0;
    if (depth < 1) depth = 1;
    if (depth >= o->max_targets_per_node_len)
        depth = o->max_targets_per_node_len - 1;
    return o->max_targets_per_node[depth];
}

/* Anchors */

/*
 * Chain start point.
 *   P          slot address (in DATA/BSS or a user-selected segment)
 *   seg_type   segment type
 *   seg_index  segment index
 *   mod_start  module base (address -> "module + offset")
 *   module     module path (borrowed from ix->segs[], same lifetime as ix)
 */
struct anchor_rec {
    uintptr_t   P;
    uint32_t    seg_type;
    uint32_t    seg_index;
    uint64_t    mod_start;
    const char *module;
};

/* edge_set: dedup (P, T) edges */

/*
 * Open-addressing hash set.
 * Purpose: prevent the same (P, T) edge from being processed twice by pm_put.
 * Stores key+1 (0 means empty slot).
 *
 * Why dedup?
 *   During BFS, windows of multiple targets may hit the same slot P.
 *   Without dedup, pm[P] would hold duplicate target records, producing
 *   duplicate chains in ENUM and wasting memory.
 */
struct edge_set {
    uint64_t *slots;
    size_t    size;
    size_t    used;
};

static int es_init(struct edge_set *es, size_t size)
{
    es->slots = map_zero(size * sizeof(uint64_t));
    if (!es->slots) return -1;
    es->size = size;
    es->used = 0;
    return 0;
}

static void es_free(struct edge_set *es)
{
    unmap_ptr(es->slots, es->size * sizeof(uint64_t));
    es->slots = NULL;
    es->size = es->used = 0;
}

static int es_grow(struct edge_set *es)
{
    size_t ns = es->size * 2;
    uint64_t *na = map_zero(ns * sizeof(uint64_t));
    if (!na) return -1;
    size_t mask = ns - 1;
    for (size_t i = 0; i < es->size; i++) {
        uint64_t s = es->slots[i];
        if (!s) continue;
        uint64_t k = s - 1;
        size_t j = hash_u64(k) & mask;
        while (na[j]) j = (j + 1) & mask;
        na[j] = s;
    }
    unmap_ptr(es->slots, es->size * sizeof(uint64_t));
    es->slots = na;
    es->size  = ns;
    return 0;
}

static int es_insert(struct edge_set *es, uint64_t key)
{
    if (es->size == 0 && es_init(es, 4096) != 0) return -1;
    if ((es->used + 1) * 10 >= es->size * 7 && es_grow(es) != 0) return -1;
    uint64_t stored = key + 1;
    if (stored == 0) stored = 1;
    size_t mask = es->size - 1;
    size_t i = hash_u64(key) & mask;
    while (es->slots[i]) {
        if (es->slots[i] == stored) return 0;
        i = (i + 1) & mask;
    }
    es->slots[i] = stored;
    es->used++;
    return 1;
}

static inline uint64_t edge_key(uintptr_t P, uintptr_t T)
{
    return ((uint64_t)P << 16) ^ ((uint64_t)P >> 48) ^ (uint64_t)T;
}

/* parent_map: P -> several (T, V) */

/*
 * One edge: (slot P holds value V), V points to address T.
 *   T      target address the pointer points to
 *   V      value read from slot P
 *   next   next item's "global index + 1" in chain, 0 = end
 *
 * next is a uint32_t global index, not a pointer. This keeps each entry at
 * 20 bytes and all pm_targets slab-contiguous.
 */
struct __attribute__((packed)) pm_target {
    uintptr_t T;
    uintptr_t V;
    uint32_t  next;
};

/*
 * Slab: 4096 pm_target entries (80 KB) from a single mmap.
 * Each slab has a used counter.
 * Global index of a pm_target = (slab_id << PM_SLAB_SHIFT) + offset_in_slab.
 */
struct pm_target_slab {
    struct pm_target slots[PM_SLAB_SIZE];
    uint32_t         used;
};

/*
 * bucket: record for one parent slot P.
 *   head       head of target chain ("global index + 1", 0 = empty)
 *   min_depth  layer where P was first discovered (1-based, saturates at 255)
 *   n_targets  current number of kept targets (top-K pruning)
 *
 * Size: 14 bytes (packed).
 */
struct __attribute__((packed)) pm_bucket {
    uintptr_t P;
    uint32_t  head;
    uint8_t   min_depth;
    uint8_t   n_targets;
};

/*
 * Main table: open-addressing hash table + slab list.
 *   b          bucket array
 *   cap        bucket capacity (power of two)
 *   n          used buckets
 *   slabs      slab pointer array
 *   n_slabs    allocated slabs
 *   cur_slab   current slab index (locality: fill one slab before opening
 *              the next)
 */
struct parent_map {
    struct pm_bucket *b;
    size_t            cap;
    size_t            n;

    struct pm_target_slab **slabs;
    size_t            n_slabs;
    size_t            slabs_cap;
    uint32_t          cur_slab;
};

/* Allocate a pm_target index; returns "global index + 1" (0 = failure). */
static uint32_t pm_target_alloc(struct parent_map *pm)
{
    /* Fast path: current slab still has room. */
    if (pm->n_slabs > 0 && pm->cur_slab < pm->n_slabs) {
        struct pm_target_slab *s = pm->slabs[pm->cur_slab];
        if (s->used < PM_SLAB_SIZE) {
            uint32_t idx = (pm->cur_slab << PM_SLAB_SHIFT) + s->used;
            s->used++;
            return idx + 1;
        }
    }

    /* Need a new slab: grow the slab pointer array first. */
    if (pm->n_slabs == pm->slabs_cap) {
        size_t nc = pm->slabs_cap ? pm->slabs_cap * 2 : 16;
        struct pm_target_slab **ns = map_zero(nc * sizeof(*ns));
        if (!ns) return 0;
        if (pm->slabs) {
            memcpy(ns, pm->slabs, pm->n_slabs * sizeof(*ns));
            unmap_ptr(pm->slabs, pm->slabs_cap * sizeof(*ns));
        }
        pm->slabs = ns;
        pm->slabs_cap = nc;
    }

    struct pm_target_slab *s = map_zero(sizeof(*s));
    if (!s) return 0;
    s->used = 0;
    pm->slabs[pm->n_slabs++] = s;
    pm->cur_slab = (uint32_t)(pm->n_slabs - 1);

    uint32_t idx = (pm->cur_slab << PM_SLAB_SHIFT) + s->used;
    s->used++;
    return idx + 1;
}

/* Translate "global index + 1" to a pm_target pointer. */
static inline struct pm_target *pm_target_get(const struct parent_map *pm,
                                              uint32_t head_plus_1)
{
    if (head_plus_1 == 0) return NULL;
    uint32_t idx = head_plus_1 - 1;
    struct pm_target_slab *s = pm->slabs[idx >> PM_SLAB_SHIFT];
    return &s->slots[idx & PM_SLAB_MASK];
}

static int pm_init(struct parent_map *pm, size_t cap)
{
    memset(pm, 0, sizeof(*pm));
    pm->b = map_zero(cap * sizeof(*pm->b));
    if (!pm->b) return -1;
    pm->cap = cap;
    return 0;
}

static void pm_free(struct parent_map *pm)
{
    if (pm->b) unmap_ptr(pm->b, pm->cap * sizeof(*pm->b));
    pm->b = NULL;
    for (size_t i = 0; i < pm->n_slabs; i++)
        unmap_ptr(pm->slabs[i], sizeof(struct pm_target_slab));
    if (pm->slabs)
        unmap_ptr(pm->slabs, pm->slabs_cap * sizeof(*pm->slabs));
    pm->slabs = NULL;
    pm->n_slabs = pm->slabs_cap = 0;
    pm->cap = pm->n = 0;
}

static int pm_grow(struct parent_map *pm)
{
    size_t nc = pm->cap * 2;
    struct pm_bucket *nb = map_zero(nc * sizeof(*nb));
    if (!nb) return -1;
    size_t mask = nc - 1;
    for (size_t i = 0; i < pm->cap; i++) {
        if (!pm->b[i].P) continue;
        size_t j = hash_u64(pm->b[i].P) & mask;
        while (nb[j].P) j = (j + 1) & mask;
        nb[j] = pm->b[i];
    }
    unmap_ptr(pm->b, pm->cap * sizeof(*pm->b));
    pm->b = nb;
    pm->cap = nc;
    return 0;
}

static struct pm_bucket *pm_find(const struct parent_map *pm, uintptr_t P)
{
    if (pm->cap == 0) return NULL;
    size_t mask = pm->cap - 1;
    size_t i = hash_u64(P) & mask;
    for (;;) {
        if (!pm->b[i].P) return NULL;
        if (pm->b[i].P == P) return &pm->b[i];
        i = (i + 1) & mask;
    }
}

/*
 * Insert one edge (P, T, V).
 *
 * Returns:
 *   1 = new edge inserted
 *   0 = duplicate or dropped by top-K pruning
 *  -1 = OOM
 *
 * Key points:
 *   1. edge_set dedup: same (P, T) is written only once.
 *
 *   2. top-K pruning (when max_per_node > 0):
 *        If bucket[P] is full, compute new_delta = |T - target|.
 *        Find the worst (largest distance) entry in the bucket.
 *        new_delta >= worst_delta -> drop the new edge (it is farther).
 *        Otherwise replace the worst entry, reusing its slot to save slab
 *        space.
 *
 *      This is the OOM guard: parent_map memory is bounded by
 *        unique_parents * K * 20 bytes, independent of BFS depth.
 *
 *   3. cur_depth is a 1-based layer index, used to fill min_depth
 *      (cycle pruning during ENUM).
 */
static int pm_put(struct parent_map *pm, struct edge_set *es,
                  uintptr_t P, uintptr_t T, uintptr_t V, int max_per_node,
                  uint32_t cur_depth, uintptr_t target)
{
    if (pm->cap == 0) return -1;

    uint64_t ekey = edge_key(P, T);
    int is_new_edge = es_insert(es, ekey);
    if (is_new_edge < 0) return -1;
    if (is_new_edge == 0) return 0;

    size_t mask = pm->cap - 1;
    size_t i = hash_u64(P) & mask;
    struct pm_bucket *bucket = NULL;
    int created = 0;

    for (;;) {
        if (!pm->b[i].P) {
            if ((pm->n + 1) * 10 >= pm->cap * 7) {
                if (pm_grow(pm) != 0) return -1;
                /* Retry after grow (recursion at most once). */
                return pm_put(pm, es, P, T, V, max_per_node, cur_depth, target);
            }
            pm->b[i].P         = P;
            pm->b[i].head      = 0;
            pm->b[i].min_depth = (uint8_t)(cur_depth > 255 ? 255 : cur_depth);
            pm->b[i].n_targets = 0;
            bucket = &pm->b[i];
            pm->n++;
            created = 1;
            break;
        }
        if (pm->b[i].P == P) { bucket = &pm->b[i]; break; }
        i = (i + 1) & mask;
    }

    /* Update min_depth: layer where P was first seen. */
    uint8_t depth8 = (uint8_t)(cur_depth > 255 ? 255 : cur_depth);
    if (!created && depth8 < bucket->min_depth)
        bucket->min_depth = depth8;

    uint64_t new_delta = (T >= target) ? (uint64_t)(T - target)
                                       : (uint64_t)(target - T);

    /* top-K full: find the farthest and replace. */
    if (max_per_node > 0 && bucket->n_targets >= max_per_node) {
        uint32_t prev = 0, cur = bucket->head;
        uint32_t worst_prev = 0, worst_cur = 0;
        uint64_t worst_delta = 0;

        while (cur) {
            struct pm_target *t = pm_target_get(pm, cur);
            uint64_t d = (t->T >= target) ? (uint64_t)(t->T - target)
                                          : (uint64_t)(target - t->T);
            if (d >= worst_delta) {
                worst_delta = d;
                worst_prev = prev;
                worst_cur = cur;
            }
            prev = cur;
            cur = t->next;
        }

        if (new_delta >= worst_delta) return 0;

        /* Detach worst. */
        struct pm_target *wt = pm_target_get(pm, worst_cur);
        if (worst_prev == 0) {
            bucket->head = wt->next;
        } else {
            struct pm_target *wp = pm_target_get(pm, worst_prev);
            wp->next = wt->next;
        }
        /* Reuse worst's slot, head insert. */
        wt->T = T;
        wt->V = V;
        wt->next = bucket->head;
        bucket->head = worst_cur;
        return 1;
    }

    /* Not full: head insert. */
    uint32_t idx = pm_target_alloc(pm);
    if (idx == 0) return -1;
    struct pm_target *nt = pm_target_get(pm, idx);
    nt->T = T;
    nt->V = V;
    nt->next = bucket->head;
    bucket->head = idx;
    bucket->n_targets++;
    return 1;
}

/* visited: per-segment bitmap */

/*
 * One bit per 8 bytes (one pointer slot). All segments share one bitmap.
 *
 * Slot j of segment i (j = (addr - start) / 8) maps to
 *   bit (j % 8) of bitmap[seg_offset[i] + j / 8].
 *
 * Compared with a traditional hash set:
 *   - 8 bytes per node -> 1 bit, 64x memory reduction
 *   - O(1) lookup / insert
 *   - no rehash, no collisions
 *
 * Cost: the address range must be relatively dense, otherwise the bitmap
 * wastes space. Our use case (process heap) is dense.
 */
struct visited {
    const struct idx *ix;
    uint8_t          *bits;
    uint64_t         *seg_offset;
    uint64_t          total_bits;
    size_t            total_bytes;
};

static int vis_init(struct visited *v, const struct idx *ix)
{
    memset(v, 0, sizeof(*v));
    v->ix = ix;

    v->seg_offset = map_zero(ix->nseg * sizeof(uint64_t));
    if (!v->seg_offset) return -1;

    uint64_t off = 0;
    for (uint32_t i = 0; i < ix->nseg; i++) {
        v->seg_offset[i] = off;
        uint64_t sz = ix->segs[i].end - ix->segs[i].start;
        off += sz / 8;
    }
    v->total_bits = off;
    v->total_bytes = (off + 7) / 8;
    if (v->total_bytes == 0) v->total_bytes = 1;

    v->bits = map_zero(v->total_bytes);
    return v->bits ? 0 : -1;
}

static void vis_free(struct visited *v)
{
    unmap_ptr(v->bits, v->total_bytes);
    unmap_ptr(v->seg_offset,
              v->ix ? v->ix->nseg * sizeof(uint64_t) : 0);
    v->bits = NULL;
    v->seg_offset = NULL;
}

/* Binary search for the segment containing P; returns its index, -1 on failure. */
static int find_seg_idx(const struct idx *ix, uintptr_t P)
{
    uint32_t lo = 0, hi = ix->nseg;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ix->segs[mid].start <= P) lo = mid + 1;
        else                          hi = mid;
    }
    if (lo == 0) return -1;
    uint32_t i = lo - 1;
    if (P >= ix->segs[i].end) return -1;
    return (int)i;
}

/* Return the global bit index for address P; UINT64_MAX on failure. */
static inline uint64_t vis_bit_of(const struct visited *v, uintptr_t P)
{
    int si = find_seg_idx(v->ix, P);
    if (si < 0) return UINT64_MAX;
    uint64_t slot = (P - v->ix->segs[si].start) >> 3;
    uint64_t bit = v->seg_offset[si] + slot;
    if (bit >= v->total_bits) return UINT64_MAX;
    return bit;
}

static inline int vis_has(const struct visited *v, uintptr_t P)
{
    uint64_t bit = vis_bit_of(v, P);
    if (bit == UINT64_MAX) return 0;
    return (v->bits[bit >> 3] >> (bit & 7)) & 1;
}

/* Returns 1 = first insert, 0 = already present. */
static inline int vis_add(struct visited *v, uintptr_t P)
{
    uint64_t bit = vis_bit_of(v, P);
    if (bit == UINT64_MAX) return 0;
    uint8_t mask = (uint8_t)(1u << (bit & 7));
    uint8_t *byte = &v->bits[bit >> 3];
    if (*byte & mask) return 0;
    *byte |= mask;
    return 1;
}

/* addr_vec: BFS layer boundary */

/*
 * Node set for one layer.
 *   a   node address array
 *   d   "source delta" for each node (offset used by the previous layer),
 *       -1 for the first layer
 *
 * The d array feeds the progress histogram ("prev -> cur"). It does not
 * affect BFS correctness.
 */
struct addr_vec {
    uintptr_t *a;
    int32_t   *d;
    uint32_t   n;
    uint32_t   cap;
};

static int av_push(struct addr_vec *av, uintptr_t v, int32_t d)
{
    if (av->n >= av->cap) {
        uint32_t nc = av->cap ? av->cap * 2 : BOUNDARY_INIT;
        uintptr_t *na = realloc(av->a, (size_t)nc * sizeof(uintptr_t));
        if (!na) return -1;
        int32_t *nd = realloc(av->d, (size_t)nc * sizeof(int32_t));
        if (!nd) return -1;
        av->a = na; av->d = nd; av->cap = nc;
    }
    av->a[av->n] = v;
    av->d[av->n] = d;
    av->n++;
    return 0;
}

static void av_free(struct addr_vec *av)
{
    free(av->a); free(av->d);
    av->a = NULL; av->d = NULL;
    av->n = 0; av->cap = 0;
}

/* anchor_vec */

struct anchor_vec {
    struct anchor_rec *a;
    uint32_t           n;
    uint32_t           cap;
};

static int ar_push(struct anchor_vec *ar, const struct anchor_rec *r)
{
    if (ar->n >= ar->cap) {
        uint32_t nc = ar->cap ? ar->cap * 2 : 64;
        struct anchor_rec *na = realloc(ar->a, (size_t)nc * sizeof(*na));
        if (!na) return -1;
        ar->a = na; ar->cap = nc;
    }
    ar->a[ar->n++] = *r;
    return 0;
}

static void ar_free(struct anchor_vec *ar)
{
    free(ar->a); ar->a = NULL; ar->n = 0; ar->cap = 0;
}

/* Histogram */

static inline uint32_t slot_hash(int32_t prev, int32_t delta)
{
    uint64_t k = ((uint64_t)(uint32_t)prev << 32) | (uint32_t)delta;
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL; k ^= k >> 33;
    return (uint32_t)k;
}

static int hist_layer_reserve(struct fs_hist_layer *L, int need)
{
    if (need <= L->cap) return 0;
    int nc = L->cap ? L->cap * 2 : 64;
    while (nc < need) nc *= 2;
    struct fs_hist_slot *ns = realloc(L->slots, (size_t)nc * sizeof(*ns));
    if (!ns) return -1;
    L->slots = ns; L->cap = nc;
    return 0;
}

static int hist_index_rebuild(struct fs_hist_layer *L, int new_cap)
{
    int *ni = malloc((size_t)new_cap * sizeof(int));
    if (!ni) return -1;
    for (int i = 0; i < new_cap; i++) ni[i] = -1;

    uint32_t mask = (uint32_t)(new_cap - 1);
    for (int i = 0; i < L->n; i++) {
        uint32_t h = slot_hash(L->slots[i].prev, L->slots[i].delta) & mask;
        while (ni[h] >= 0) h = (h + 1) & mask;
        ni[h] = i;
    }
    free(L->index);
    L->index = ni; L->index_cap = new_cap;
    return 0;
}

/*
 * Layer begin: clear the layer's histogram.
 * Does not free the slots array (reused next time).
 * If the layer's slots were already moved out by snapshot_one, slots is
 * NULL here and will be reallocated on demand. Safe either way.
 */
void fs_progress_hist_begin(struct fs_progress *p,
                            int depth, uint64_t boundary_in)
{
    if (!p || depth < 1 || depth > FS_HIST_MAX_LAYERS) return;
    pthread_mutex_lock(&p->hist_mtx);
    struct fs_hist_layer *L = &p->hist_layers[depth - 1];
    L->n = 0;
    L->boundary_in = boundary_in;
    L->total_hits = 0;
    pthread_mutex_unlock(&p->hist_mtx);

    int prev = atomic_load_explicit(&p->hist_max_depth, memory_order_relaxed);
    while (depth > prev) {
        if (atomic_compare_exchange_weak_explicit(
                &p->hist_max_depth, &prev, depth,
                memory_order_relaxed, memory_order_relaxed))
            break;
    }
}

/*
 * Merge locally accumulated (prev, delta, hits, anc) into the shared layer.
 * Takes the lock once; cost is amortized over FS_HIST_FLUSH_BATCH hits.
 */
static int hist_flush(struct fs_progress *pg, int depth,
                      const struct fs_hist_slot *local, int local_n)
{
    if (!pg || local_n <= 0) return 0;
    if (depth < 1 || depth > FS_HIST_MAX_LAYERS) return 0;

    pthread_mutex_lock(&pg->hist_mtx);
    struct fs_hist_layer *L = &pg->hist_layers[depth - 1];

    if (!L->index || (L->n + local_n) * 10 >= L->index_cap * 7) {
        int nc = L->index_cap ? L->index_cap : 256;
        while ((L->n + local_n) * 10 >= nc * 7) nc *= 2;
        if (hist_index_rebuild(L, nc) != 0) {
            pthread_mutex_unlock(&pg->hist_mtx);
            return -1;
        }
    }

    uint32_t mask = (uint32_t)(L->index_cap - 1);
    for (int i = 0; i < local_n; i++) {
        uint32_t h = slot_hash(local[i].prev, local[i].delta) & mask;
        int id = -1;
        for (;;) {
            int cur = L->index[h];
            if (cur < 0) break;
            if (L->slots[cur].prev  == local[i].prev &&
                L->slots[cur].delta == local[i].delta) { id = cur; break; }
            h = (h + 1) & mask;
        }
        if (id >= 0) {
            L->slots[id].hits += local[i].hits;
            L->slots[id].anc  += local[i].anc;
            continue;
        }
        if (hist_layer_reserve(L, L->n + 1) != 0) {
            pthread_mutex_unlock(&pg->hist_mtx);
            return -1;
        }
        L->slots[L->n] = local[i];
        L->index[h] = L->n;
        L->n++;
    }
    pthread_mutex_unlock(&pg->hist_mtx);
    return 0;
}

/*
 * Snapshot of one layer.
 *
 * Key point: ownership transfer.
 *
 * Old design: malloc a copy for out, src is unchanged. Problem: src->slots
 * were never freed and accumulated over time. 8 layers x 260k entries x 28
 * bytes ~= 58 MB resident, growing linearly with layer count.
 *
 * New design: hand src->slots directly to out.
 *   - src->slots = NULL, n = 0, cap = 0
 *   - src->index is freed immediately
 *   - out owns slots; caller frees it after use
 *
 * Call convention:
 *   - called once per layer (when the layer is frozen)
 *   - repeated calls return empty data (slots = NULL, n = 0)
 *
 * Effect: progress holds only the currently running layer's slots (~7 MB);
 * all frozen layers' slots are held by the caller and released shortly.
 */
int fs_progress_hist_snapshot_one(struct fs_progress *p,
                                  int layer_1based,
                                  struct fs_hist_layer *out)
{
    if (!p || !out) return -1;
    if (layer_1based < 1 || layer_1based > FS_HIST_MAX_LAYERS) return -1;

    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&p->hist_mtx);

    struct fs_hist_layer *src = &p->hist_layers[layer_1based - 1];

    out->slots       = src->slots;
    out->n           = src->n;
    out->cap         = src->cap;
    out->boundary_in = src->boundary_in;
    out->total_hits  = 0;
    for (int k = 0; k < src->n; k++)
        out->total_hits += src->slots[k].hits;

    src->slots = NULL;
    src->n     = 0;
    src->cap   = 0;
    free(src->index);
    src->index     = NULL;
    src->index_cap = 0;

    pthread_mutex_unlock(&p->hist_mtx);
    return 0;
}

static int cmp_slot_hits_desc(const void *a, const void *b)
{
    const struct fs_hist_slot *x = a, *y = b;
    if (x->hits != y->hits) return (x->hits < y->hits) ? 1 : -1;
    return 0;
}

void fs_progress_hist_sort(struct fs_hist_layer *L)
{
    if (!L || L->n <= 1) return;
    qsort(L->slots, (size_t)L->n, sizeof(*L->slots), cmp_slot_hits_desc);
}

void fs_progress_hist_release(struct fs_hist_layer *layers, int n)
{
    if (!layers) return;
    for (int i = 0; i < n; i++) {
        free(layers[i].slots);
        layers[i].slots = NULL;
        layers[i].n = 0;
        layers[i].cap = 0;
    }
}

/* scan_ctx */

struct scan_ctx {
    const struct idx      *ix;
    const struct fs_opts  *opts;
    struct fs_progress    *pg;

    struct pc_list        *cl;
    int                    total_chains;

    struct parent_map      pm;
    struct edge_set        es;
    struct visited         vis;
    struct anchor_vec      anchors;
    struct addr_vec       *next;

    uintptr_t              target;
    int                    aborted;
    int                    oom;
    uint64_t               edges_added;
    uint64_t               edges_dup;
};

static inline int check_cancel(struct scan_ctx *ctx)
{
    if (ctx->aborted) return 1;
    if (ctx->pg &&
        atomic_load_explicit(&ctx->pg->cancelled, memory_order_relaxed)) {
        ctx->aborted = 1;
        return 1;
    }
    return 0;
}

/* Simplified segment lookup (reuses find_seg_idx). */
static const struct seg *find_seg_by_addr(const struct idx *ix, uintptr_t P)
{
    int i = find_seg_idx(ix, P);
    return (i < 0) ? NULL : &ix->segs[i];
}

static int seg_match_select(const struct seg *s,
                            struct vma_select * const *sel, int count)
{
    if (!s || !sel || count <= 0) return 0;
    for (int i = 0; i < count; i++) {
        struct vma_select *v = sel[i];
        if (!v) continue;
        if (v->module && v->module[0]) {
            if (!s->pathname || !strstr(s->pathname, v->module)) continue;
        }
        if (v->seg_type >= 0 && (int)s->type != v->seg_type) continue;
        if (v->index >= 0 && (int)s->index != v->index) continue;
        return 1;
    }
    return 0;
}

/*
 * Decide whether P is an anchor (a valid chain start).
 *
 * Default rules (opts->anchors empty):
 *   - segment type is DATA or BSS
 *   - pathname does not start with '[' (skip anonymous mappings)
 *
 * With a user selector, match by selector (module / seg_type / index / perm).
 *
 * On hit, fill rec and normalize the module base: if seg->mod_start is
 * invalid, fall back to seg->start so emit_chain produces offsets[0] = 0.
 */
static int is_anchor(const struct idx *ix, const struct fs_opts *opts,
                     uintptr_t P, struct anchor_rec *rec)
{
    const struct seg *s = find_seg_by_addr(ix, P);
    if (!s || !s->pathname) return 0;
    if (opts->anchors && opts->anchor_count > 0) {
        if (!seg_match_select(s, opts->anchors, opts->anchor_count))
            return 0;
    } else {
        if (s->type != VMA_TYPE_DATA && s->type != VMA_TYPE_BSS) return 0;
        if (s->pathname[0] == '[') return 0;
    }
    uint64_t mod_start = s->mod_start;
    if (mod_start == 0 || mod_start > s->start) mod_start = s->start;
    rec->P = P;
    rec->seg_type = s->type;
    rec->seg_index = s->index;
    rec->mod_start = mod_start;
    rec->module = s->pathname;
    return 1;
}

/*
 * Append an offset sequence to the result chain list.
 * Group by (filename, seg_type, seg_index); same group shares one pc_list
 * node. Within a group, store chains in oc_block chunks (4096 entries each).
 */
static int pc_list_append_chain(struct scan_ctx *ctx,
                                const int32_t *offsets, int count,
                                const char *filename,
                                uint8_t seg_type, uint8_t seg_index)
{
    if (!offsets || count <= 0) return -1;
    struct pc_list *node = ctx->cl;
    while (node) {
        int name_match = (node->filename == NULL && filename[0] == '\0') ||
                         (node->filename &&
                          strcmp(node->filename, filename) == 0);
        if (name_match && node->seg_type == seg_type &&
            node->seg_index == seg_index) break;
        node = node->next;
    }
    if (!node) {
        node = calloc(1, sizeof(*node));
        if (!node) return -1;
        if (filename[0]) {
            node->filename = strdup(filename);
            if (!node->filename) { free(node); return -1; }
        }
        node->seg_type = seg_type;
        node->seg_index = seg_index;
        node->chain_count = 0;
        node->chains = NULL;
        node->next = ctx->cl;
        ctx->cl = node;
    }
    struct oc_block *b = node->chains, *last = NULL;
    while (b && atomic_load(&b->used) >= OC_LIST_CAPACITY) {
        last = b; b = b->next;
    }
    if (!b) {
        b = calloc(1, sizeof(*b));
        if (!b) return -1;
        atomic_init(&b->used, 0);
        b->next = NULL;
        if (last) last->next = b; else node->chains = b;
    }
    int slot = atomic_load(&b->used);
    struct offset_chain *oc = &b->oc[slot];
    oc->offsets.count = count;
    oc->offsets.offset = malloc((size_t)count * sizeof(int32_t));
    if (!oc->offsets.offset) return -1;
    memcpy(oc->offsets.offset, offsets, (size_t)count * sizeof(int32_t));
    b->oc_cost[slot] = 0;
    atomic_store(&b->used, slot + 1);
    node->chain_count++;
    return 0;
}

/*
 * Serialize a path into an offset chain and append it to the result.
 *
 * Input:
 *   path[0..path_len-1]   address sequence from anchor to target
 *   values[i]             value of path[i] stored in path[i-1]
 *
 * Output offsets:
 *   offsets[0]  = path[0] - mod_start   (relative to module base)
 *   offsets[i]  = path[i] - values[i-1] (relative to previous pointer value)
 *
 * Why: starting from the module base, read offsets[0] to reach path[0],
 * then path[0] + offsets[1] to reach path[1], and so on to target.
 */
static int emit_chain(struct scan_ctx *ctx,
                      const uintptr_t *path, const uintptr_t *values,
                      int path_len, const struct anchor_rec *ar)
{
    if (path_len < 1 || path_len > TRACE_MAX) return -1;
    const struct fs_opts *opts = ctx->opts;
    if (opts->min_depth > 0 && path_len < opts->min_depth) return -1;

    int32_t offs[TRACE_MAX];
    uint64_t mod_base = ar->mod_start;
    if (mod_base == 0 || mod_base > path[0]) return -1;

    uint64_t d0 = path[0] - mod_base;
    if (d0 > OFF_MAX) return -1;
    offs[0] = (int32_t)d0;

    for (int i = 1; i < path_len; i++) {
        if (path[i] < values[i - 1]) return -1;
        uint64_t d = path[i] - values[i - 1];
        if (d > OFF_MAX) return -1;
        offs[i] = (int32_t)d;
    }
    const char *filename = ar->module ? ar->module : "";
    if (pc_list_append_chain(ctx, offs, path_len, filename,
                             (uint8_t)ar->seg_type,
                             (uint8_t)ar->seg_index) != 0) return -1;
    ctx->total_chains++;
    if (ctx->pg)
        atomic_fetch_add_explicit(&ctx->pg->total_chains, 1,
                                  memory_order_relaxed);
    return 0;
}

/*
 * For each target_index group, keep only the K entries with the smallest
 * |value - target|. hits is already sorted by target_index (idx_scan output).
 * In-place compact; returns the new count.
 *
 * Trick: 0xFFFFFFFF marks "selected" entries to avoid re-picking the same
 * one; target_index is restored afterwards.
 */
static int64_t filter_hits_topk(struct idx_hit *hits, int64_t nh, uint32_t K,
                                uintptr_t target)
{
    if (K == 0 || nh <= 1) return nh;
    struct idx_hit *out = malloc((size_t)nh * sizeof(*out));
    if (!out) return nh;
    int64_t w = 0, i = 0;
    while (i < nh) {
        uint32_t t = hits[i].target_index;
        int64_t j = i;
        while (j < nh && hits[j].target_index == t) j++;
        int64_t seg_n = j - i, picked = 0;
        uint32_t mark = 0xFFFFFFFFu;
        while (picked < (int64_t)K && picked < seg_n) {
            int64_t best = -1;
            uint64_t best_d = UINT64_MAX;
            for (int64_t k = i; k < j; k++) {
                if (hits[k].target_index == mark) continue;
                uint64_t v = hits[k].value;
                uint64_t d = (v >= target) ? (v - target) : (target - v);
                if (d < best_d) { best_d = d; best = k; }
            }
            if (best < 0) break;
            out[w++] = hits[best];
            hits[best].target_index = mark;
            picked++;
        }
        for (int64_t k = i; k < j; k++) hits[k].target_index = mark == t ? t : t;
        i = j;
    }
    memcpy(hits, out, (size_t)w * sizeof(*out));
    free(out);
    return w;
}

/* Forward declaration. */
static void explore(struct scan_ctx *ctx,
                    uintptr_t *path, uintptr_t *values, int path_len,
                    const struct anchor_rec *ar);

/*
 * Expand all out-edges from bucket[P].
 *
 * Pruning rule:
 *   If the next edge's T has min_depth >= current bucket's min_depth,
 *   traversal would go deeper; prune it (cycle prevention).
 *   This makes ENUM's search direction monotonically decreasing
 *   (deep to shallow). Each recursion decreases the layer number, so:
 *     - no cycles
 *     - recursion depth <= 255
 */
static void explore_bucket(struct scan_ctx *ctx,
                           const struct pm_bucket *bucket,
                           uintptr_t *path, uintptr_t *values, int path_len,
                           const struct anchor_rec *ar)
{
    uint32_t cur_depth = bucket->min_depth;
    uint32_t idx = bucket->head;

    while (idx) {
        if (ctx->aborted) return;
        if (check_cancel(ctx)) return;
        if (ctx->opts->max_chains > 0 &&
            ctx->total_chains >= ctx->opts->max_chains) {
            ctx->aborted = 1; return;
        }

        struct pm_target *t = pm_target_get(&ctx->pm, idx);

        if (t->T != ctx->target) {
            struct pm_bucket *tb = pm_find(&ctx->pm, t->T);
            if (!tb) { idx = t->next; continue; }
            if (tb->min_depth >= cur_depth) { idx = t->next; continue; }
        }

        values[path_len - 1] = t->V;
        path[path_len] = t->T;

        if (t->T == ctx->target) {
            emit_chain(ctx, path, values, path_len + 1, ar);
        } else if (path_len + 1 < TRACE_MAX) {
            explore(ctx, path, values, path_len + 1, ar);
        }

        idx = t->next;
    }
}

static void explore(struct scan_ctx *ctx,
                    uintptr_t *path, uintptr_t *values, int path_len,
                    const struct anchor_rec *ar)
{
    if (ctx->aborted) return;
    uintptr_t last = path[path_len - 1];
    struct pm_bucket *bucket = pm_find(&ctx->pm, last);
    if (!bucket) return;
    explore_bucket(ctx, bucket, path, values, path_len, ar);
}

static void enumerate_anchor(struct scan_ctx *ctx, const struct anchor_rec *ar)
{
    uintptr_t path[TRACE_MAX];
    uintptr_t values[TRACE_MAX];
    path[0] = ar->P;
    if (ar->P == ctx->target) {
        emit_chain(ctx, path, values, 1, ar);
        return;
    }
    struct pm_bucket *bucket = pm_find(&ctx->pm, ar->P);
    if (!bucket) return;
    explore_bucket(ctx, bucket, path, values, 1, ar);
}

/*
 * Process one BFS hit (P, V, T).
 *
 * Steps:
 *   1. basic filtering
 *   2. node dedup: has P been seen before?
 *   3. pm_put: edge dedup + write into parent_map + top-K pruning
 *   4. if P is new:
 *        - add to visited
 *        - push to next-layer boundary (with src_delta for the histogram)
 *        - test anchor; if yes, add to ctx->anchors
 */
static void process_hit(struct scan_ctx *ctx,
                        uintptr_t P, uintptr_t V, uintptr_t T,
                        uint32_t cur_depth)
{
    const struct fs_opts *opts = ctx->opts;
    if (ctx->aborted) return;
    if (opts->max_chains > 0 && ctx->total_chains >= opts->max_chains) {
        ctx->aborted = 1; return;
    }
    if (V > T) return;
    if (P == T) return;
    if (P == ctx->target) return;

    int is_new_node = !vis_has(&ctx->vis, P);
    int is_new = pm_put(&ctx->pm, &ctx->es, P, T, V,
                        max_targets_at(opts, cur_depth), cur_depth,
                        ctx->target);
    if (is_new < 0) { ctx->aborted = 1; ctx->oom = 1; return; }
    if (is_new == 0) { ctx->edges_dup++; return; }
    ctx->edges_added++;

    if (is_new_node) {
        vis_add(&ctx->vis, P);
        int32_t src_delta = (int32_t)(T - V);
        if (av_push(ctx->next, P, src_delta) != 0) {
            ctx->aborted = 1; ctx->oom = 1; return;
        }
        struct anchor_rec rec = {0};
        if (is_anchor(ctx->ix, opts, P, &rec)) {
            if (ar_push(&ctx->anchors, &rec) != 0) {
                ctx->aborted = 1; ctx->oom = 1;
            }
        }
    }
}

/*
 * Get the tail whitelist slice for layer_1based.
 *   layer_1based = 1  -> tail_starts[0]..tail_starts[1]
 *   layer_1based = 2  -> tail_starts[1]..tail_starts[2]
 * Returns 1 = this layer has constraints, 0 = unconstrained.
 */
static inline int
tail_layer_offsets(const struct fs_opts *o, int layer_1based,
                   const int32_t **out_offs, int *out_n)
{
    if (!o->tail_flat || !o->tail_starts || o->tail_layer_count <= 0)
        return 0;
    if (layer_1based < 1 || layer_1based > o->tail_layer_count)
        return 0;
    int s = o->tail_starts[layer_1based - 1];
    int e = o->tail_starts[layer_1based];
    if (e <= s) return 0;
    *out_offs = o->tail_flat + s;
    *out_n = e - s;
    return 1;
}

/* Main flow */

int fs_ptrscan(const struct idx *ix,
               const struct fs_opts *opts,
               struct fs_progress *progress,
               struct fs_result **result)
{
    if (!result) return FS_SCAN_FAILED;
    *result = NULL;
    if (!ix || !opts || opts->target == 0)
        return FS_SCAN_FAILED;

    struct fs_result *res = calloc(1, sizeof(*res));
    if (!res) return FS_SCAN_FAILED;

    uint64_t t_start = now_ns();
    atomic_store(&res->perf.start_ns,     t_start);
    atomic_store(&res->perf.bfs_start_ns, t_start);

    if (progress) {
        atomic_store(&progress->phase,    FS_SCAN_PHASE_BFS);
        atomic_store(&progress->start_ms, t_start / 1000000ULL);
    }

    struct scan_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ix = ix; ctx.opts = opts; ctx.pg = progress;
    ctx.target = opts->target;

    /*
     * parent_map preallocation: estimate an upper bound from idx entry count.
     * idx->n / 16 is empirical -- not every pointer becomes a parent node.
     * Round up to a power of two to fit hash mask arithmetic.
     */
    size_t n_hint = ix->n / 16;
    if (n_hint < PM_INIT_CAP) n_hint = PM_INIT_CAP;
    size_t cap = 1;
    while (cap < n_hint) cap <<= 1;

    int pm_ok = 0, es_ok = 0, vis_ok = 0;
    if (pm_init(&ctx.pm, cap)  == 0) pm_ok  = 1; else goto fail_init;
    if (es_init(&ctx.es, 4096) == 0) es_ok  = 1; else goto fail_init;
    if (vis_init(&ctx.vis, ix) == 0) vis_ok = 1; else goto fail_init;

    struct addr_vec boundary = {0}, next = {0};
    uint64_t *targets = NULL;
    uint32_t  targets_cap = 0;

    struct fs_hist_slot *local = NULL;
    int local_n = 0, local_cap = 0, local_pending = 0;

    /* target itself may be an anchor. */
    {
        struct anchor_rec rec = {0};
        if (is_anchor(ix, opts, opts->target, &rec)) {
            if (ar_push(&ctx.anchors, &rec) != 0) goto fail_run;
        }
    }
    if (av_push(&boundary, opts->target, -1) != 0) goto fail_run;

    /* max_depth: 0 = unlimited; otherwise Layer 1..max_d. */
    int max_d = (opts->max_depth > 0) ? opts->max_depth : INT_MAX;

    /*
     * BFS main loop (1-based layer number).
     *
     * Per layer:
     *   1. idx_scan: hit set for all boundary nodes
     *   2. tail whitelist filter (in-place compact)
     *   3. per-T top-K (in-place compact)
     *   4. process_hit: write pm / vis / anchors / next boundary
     *   5. accumulate layer histogram locally, flush in batches
     *   6. boundary = next
     */
    for (int depth = 1; depth <= max_d && boundary.n > 0; depth++) {
        if (check_cancel(&ctx)) break;

        uint32_t b_in = boundary.n;
        uint64_t cur_max_off = max_off_at(opts, depth);
        int cur_max_targets  = max_targets_at(opts, depth);

        /* Compute per-T lower bounds for idx_scan binary search. */
        if (boundary.n > targets_cap) {
            uint32_t nc = targets_cap ? targets_cap * 2 : BOUNDARY_INIT;
            while (nc < boundary.n) nc *= 2;
            uint64_t *nt = realloc(targets, (size_t)nc * sizeof(uint64_t));
            if (!nt) { ctx.oom = 1; break; }
            targets = nt; targets_cap = nc;
        }
        for (uint32_t i = 0; i < boundary.n; i++) {
            uintptr_t T = boundary.a[i];
            targets[i] = (T >= cur_max_off) ? (uint64_t)(T - cur_max_off) : 0;
        }

        /* Batch window scan: hits for all boundary nodes. */
        struct idx_hit *hits = NULL;
        int64_t nh = idx_scan(ix, targets, boundary.n, cur_max_off, &hits);
        if (nh < 0) { ctx.oom = 1; break; }

        /* tail whitelist filter (in-place compact). */
        {
            const int32_t *layer = NULL;
            int layer_n = 0;
            if (nh > 0 &&
                tail_layer_offsets(opts, depth, &layer, &layer_n)) {
                int64_t w = 0;
                for (int64_t j = 0; j < nh; j++) {
                    uint32_t ti = hits[j].target_index;
                    if (ti >= boundary.n) continue;
                    uintptr_t T = boundary.a[ti];
                    uintptr_t V = hits[j].value;
                    if (T < V) continue;
                    uint64_t delta = (uint64_t)(T - V);
                    int match = 0;
                    for (int k = 0; k < layer_n; k++) {
                        if (delta == (uint64_t)layer[k]) { match = 1; break; }
                    }
                    if (match) hits[w++] = hits[j];
                }
                nh = w;
            }
        }

        /* Per-T top-K pruning. */
        if (cur_max_targets > 0 && nh > 0)
            nh = filter_hits_topk(hits, nh,
                                  (uint32_t)cur_max_targets, ctx.target);

        next.n = 0;
        ctx.next = &next;

        if (progress) {
            atomic_store(&progress->depth,      depth);
            atomic_store(&progress->layer_in,   b_in);
            atomic_store(&progress->layer_out,  0);
            atomic_store(&progress->layer_hits, (uint64_t)nh);
            int prev_max = atomic_load(&progress->max_depth_reached);
            if (depth > prev_max)
                atomic_store(&progress->max_depth_reached, depth);
        }

        if (progress) fs_progress_hist_begin(progress, depth, b_in);
        local_n = 0; local_pending = 0;

        for (int64_t j = 0; j < nh; j++) {
            if (check_cancel(&ctx)) break;

            if (progress) {
                atomic_store(&progress->total_pm,      ctx.pm.n);
                atomic_store(&progress->total_edges,   ctx.edges_added);
                atomic_store(&progress->total_dup,     ctx.edges_dup);
                atomic_store(&progress->total_anchors, ctx.anchors.n);
                atomic_store(&progress->layer_out,     next.n);
            }

            uint32_t ti = hits[j].target_index;
            uintptr_t T = boundary.a[ti];
            uintptr_t V = hits[j].value;

            /* Local histogram accumulation (lock-free), flushed in batches. */
            if (progress && V <= T) {
                int32_t cur_d  = (int32_t)(T - V);
                int32_t prev_d = boundary.d ? boundary.d[ti] : -1;

                struct anchor_rec tmp = {0};
                int is_anc = is_anchor(ix, opts, hits[j].source, &tmp);

                int found = 0;
                for (int k = 0; k < local_n; k++) {
                    if (local[k].prev == prev_d && local[k].delta == cur_d) {
                        local[k].hits++;
                        if (is_anc) local[k].anc++;
                        found = 1; break;
                    }
                }
                if (!found) {
                    if (local_n >= local_cap) {
                        int nc = local_cap ? local_cap * 2 : 64;
                        struct fs_hist_slot *nl = realloc(local,
                                            (size_t)nc * sizeof(*nl));
                        if (nl) { local = nl; local_cap = nc; }
                    }
                    if (local_n < local_cap) {
                        local[local_n].prev = prev_d;
                        local[local_n].delta = cur_d;
                        local[local_n].hits = 1;
                        local[local_n].anc = is_anc ? 1 : 0;
                        local_n++;
                    }
                }
                if (++local_pending >= FS_HIST_FLUSH_BATCH) {
                    hist_flush(progress, depth, local, local_n);
                    local_n = 0; local_pending = 0;
                }
            }

            process_hit(&ctx, hits[j].source, hits[j].value, T,
                        (uint32_t)depth);
            if (ctx.aborted) break;
        }

        if (progress && local_n > 0)
            hist_flush(progress, depth, local, local_n);

        free(hits);

        /* Swap boundary and next. */
        struct addr_vec tmp = boundary;
        boundary = next;
        next = tmp;
    }

    free(targets); free(local); targets = NULL;

    /*
     * BFS done -> free BFS-only structures not needed by ENUM.
     *   edge_set   up to ~1.5 GB in a d=5 -k 0 scenario
     *   visited    ~30 MB
     *   boundary   a few MB
     *   next       a few MB
     *
     * ENUM only needs parent_map + anchors + chains. Freeing early brings
     * ENUM peak memory from ~3.1 GB down to ~1.6 GB.
     */
    es_free(&ctx.es);
    vis_free(&ctx.vis);
    av_free(&boundary);
    av_free(&next);
    ctx.next = NULL;

    atomic_store(&res->perf.bfs_end_ns, now_ns());
    if (progress) atomic_store(&progress->phase, FS_SCAN_PHASE_ENUM);
    atomic_store(&res->perf.enum_start_ns, now_ns());

    /*
     * ENUM phase.
     *
     * For each anchor, walk parent_map backwards to target, enumerating
     * all paths. BFS already built the full graph, so each anchor's
     * downstream is complete.
     *
     * Memory: read-only pm; no new allocations (except chains).
     * Time:   O(total paths) = O(chains).
     */
    if (!ctx.aborted && !ctx.oom && ctx.anchors.n > 0) {
        if (progress) {
            atomic_store(&progress->enum_total, (int)ctx.anchors.n);
            atomic_store(&progress->enum_index, 0);
        }
        for (uint32_t i = 0; i < ctx.anchors.n; i++) {
            if (check_cancel(&ctx)) break;
            if (progress) {
                atomic_store(&progress->enum_index,    (int)(i + 1));
                atomic_store(&progress->total_pm,      ctx.pm.n);
                atomic_store(&progress->total_edges,   ctx.edges_added);
                atomic_store(&progress->total_dup,     ctx.edges_dup);
                atomic_store(&progress->total_anchors, ctx.anchors.n);
            }
            enumerate_anchor(&ctx, &ctx.anchors.a[i]);
        }
    }

    atomic_store(&res->perf.enum_end_ns, now_ns());
    atomic_store(&res->perf.end_ns, now_ns());
    uint64_t s = atomic_load(&res->perf.start_ns);
    uint64_t e = atomic_load(&res->perf.end_ns);
    res->perf.total_ms = (double)(e - s) / 1e6;

    /* Final release: only pm and anchors remain. */
    ar_free(&ctx.anchors);
    pm_free(&ctx.pm);

    if (ctx.oom) {
        if (progress) atomic_store(&progress->phase, FS_SCAN_PHASE_FAILED);
        free_pc_list(ctx.cl); free(res);
        return FS_SCAN_FAILED;
    }
    res->chains = ctx.cl;
    *result = res;
    if (progress) atomic_store(&progress->phase, FS_SCAN_PHASE_DONE);
    return ctx.aborted ? FS_SCAN_CANCELLED : FS_SCAN_FINISH;

fail_run:
    free(targets); free(local);
    ar_free(&ctx.anchors);
    av_free(&boundary); av_free(&next);
    vis_free(&ctx.vis); es_free(&ctx.es); pm_free(&ctx.pm);
    free_pc_list(ctx.cl); free(res);
    if (progress) atomic_store(&progress->phase, FS_SCAN_PHASE_FAILED);
    return FS_SCAN_FAILED;

fail_init:
    if (pm_ok)  pm_free(&ctx.pm);
    if (es_ok)  es_free(&ctx.es);
    if (vis_ok) vis_free(&ctx.vis);
    free(res);
    if (progress) atomic_store(&progress->phase, FS_SCAN_PHASE_FAILED);
    return FS_SCAN_FAILED;
}

/* Lifecycle */

void free_fs_result(struct fs_result **result)
{
    if (!result || !*result) return;
    free_pc_list((*result)->chains);
    free(*result);
    *result = NULL;
}

struct fs_progress *create_fs_progress(void)
{
    struct fs_progress *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    if (pthread_mutex_init(&p->hist_mtx, NULL) != 0) { free(p); return NULL; }

    atomic_init(&p->phase,             FS_SCAN_PHASE_IDLE);
    atomic_init(&p->start_ms,          0);
    atomic_init(&p->depth,             0);
    atomic_init(&p->max_depth_reached, 0);
    atomic_init(&p->layer_in,          0);
    atomic_init(&p->layer_out,         0);
    atomic_init(&p->layer_hits,        0);
    atomic_init(&p->total_pm,          0);
    atomic_init(&p->total_edges,       0);
    atomic_init(&p->total_dup,         0);
    atomic_init(&p->total_anchors,     0);
    atomic_init(&p->total_chains,      0);
    atomic_init(&p->enum_index,        0);
    atomic_init(&p->enum_total,        0);
    atomic_init(&p->cancelled,         0);
    atomic_init(&p->hist_max_depth,    0);
    memset(p->hist_layers, 0, sizeof(p->hist_layers));
    return p;
}

void free_fs_progress(struct fs_progress *p)
{
    if (!p) return;
    for (int i = 0; i < FS_HIST_MAX_LAYERS; i++) {
        free(p->hist_layers[i].slots);
        free(p->hist_layers[i].index);
    }
    pthread_mutex_destroy(&p->hist_mtx);
    free(p);
}

void fs_progress_cancel(struct fs_progress *p)
{
    if (p) atomic_store_explicit(&p->cancelled, 1, memory_order_relaxed);
}