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

#include "vma/vm_area.h"
#include "vma/vma_select.h"

#define PM_INIT_CAP     4096
#define BOUNDARY_INIT   256
#define TRACE_MAX       1024
#define OFF_MAX         0x7FFFFFFFULL

#define EVENT_POLL_MASK       0xFF
#define PROGRESS_UPDATE_MASK  0x3F

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static double ns_to_ms(uint64_t ns) { return (double)ns / 1e6; }

static inline uint64_t
max_off_at(const struct fs_scan_opts *o, int depth)
{
    if (!o->max_off || o->max_off_len <= 0) return 0x1000;
    if (depth < 0) depth = 0;
    if (depth >= o->max_off_len) depth = o->max_off_len - 1;
    return o->max_off[depth];
}

static inline int
max_targets_at(const struct fs_scan_opts *o, int depth)
{
    if (!o->max_targets_per_node || o->max_targets_per_node_len <= 0)
        return 0;
    if (depth < 0) depth = 0;
    if (depth >= o->max_targets_per_node_len)
        depth = o->max_targets_per_node_len - 1;
    return o->max_targets_per_node[depth];
}

static inline uint64_t hash_u64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

struct edge_set {
    uint64_t *slots;      /* 0 = empty, else key + 1 */
    size_t    size;
    size_t    used;
};

static int es_init(struct edge_set *es, size_t size)
{
    es->slots = calloc(size, sizeof(uint64_t));
    if (!es->slots) return -1;
    es->size = size;
    es->used = 0;
    return 0;
}

static void es_free(struct edge_set *es)
{
    free(es->slots);
    es->slots = NULL;
    es->size = es->used = 0;
}

static int es_grow(struct edge_set *es)
{
    size_t ns = es->size * 2;
    uint64_t *na = calloc(ns, sizeof(uint64_t));
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
    free(es->slots);
    es->slots = na;
    es->size  = ns;
    return 0;
}

static int es_insert(struct edge_set *es, uint64_t key)
{
    if (es->size == 0) {
        if (es_init(es, 4096) != 0) return -1;
    }
    if ((es->used + 1) * 10 >= es->size * 7) {
        if (es_grow(es) != 0) return -1;
    }
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

struct pm_target {
    uintptr_t         T;
    uintptr_t         V;
    struct pm_target *next;
};

struct pm_bucket {
    uintptr_t         P;
    uint32_t          min_depth;
    struct pm_target *targets;
    int               n_targets;
};

struct parent_map {
    struct pm_bucket *b;
    size_t            cap;
    size_t            n;
};

static int pm_init(struct parent_map *pm, size_t cap)
{
    pm->b = calloc(cap, sizeof(*pm->b));
    if (!pm->b) return -1;
    pm->cap = cap;
    pm->n   = 0;
    return 0;
}

static void pm_free(struct parent_map *pm)
{
    if (!pm->b) return;
    for (size_t i = 0; i < pm->cap; i++) {
        struct pm_target *t = pm->b[i].targets;
        while (t) { struct pm_target *nx = t->next; free(t); t = nx; }
    }
    free(pm->b);
    pm->b = NULL; pm->cap = 0; pm->n = 0;
}

static int pm_grow(struct parent_map *pm)
{
    size_t nc = pm->cap * 2;
    struct pm_bucket *nb = calloc(nc, sizeof(*nb));
    if (!nb) return -1;
    size_t mask = nc - 1;
    for (size_t i = 0; i < pm->cap; i++) {
        if (!pm->b[i].P) continue;
        size_t j = hash_u64(pm->b[i].P) & mask;
        while (nb[j].P) j = (j + 1) & mask;
        nb[j] = pm->b[i];
    }
    free(pm->b);
    pm->b = nb; pm->cap = nc;
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

static int pm_put(struct parent_map *pm, struct edge_set *es,
                  uintptr_t P, uintptr_t T, uintptr_t V, int max_per_node,
                  uint32_t cur_depth, uintptr_t target)
{
    if (pm->cap == 0) {
        if (pm_init(pm, PM_INIT_CAP) != 0) return -1;
    }

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
                return pm_put(pm, es, P, T, V, max_per_node, cur_depth, target);
            }
            pm->b[i].P         = P;
            pm->b[i].min_depth = cur_depth;
            bucket = &pm->b[i];
            pm->n++;
            created = 1;
            break;
        }
        if (pm->b[i].P == P) { bucket = &pm->b[i]; break; }
        i = (i + 1) & mask;
    }

    if (!created && cur_depth < bucket->min_depth)
        bucket->min_depth = cur_depth;

    uint64_t new_delta = (T >= target) ? (uint64_t)(T - target)
                                       : (uint64_t)(target - T);

    if (max_per_node > 0 && bucket->n_targets >= max_per_node) {
        struct pm_target *worst = NULL, *worst_prev = NULL, *prev = NULL;
        uint64_t worst_delta = 0;
        for (struct pm_target *t = bucket->targets; t; t = t->next) {
            uint64_t d = (t->T >= target) ? (uint64_t)(t->T - target)
                                          : (uint64_t)(target - t->T);
            if (!worst || d > worst_delta) { worst = t; worst_prev = prev; worst_delta = d; }
            prev = t;
        }
        if (new_delta >= worst_delta) return 0;
        if (worst_prev) worst_prev->next = worst->next;
        else            bucket->targets   = worst->next;
        worst->T = T; worst->V = V;
        worst->next = bucket->targets;
        bucket->targets = worst;
        return 1;
    }

    struct pm_target *nt = malloc(sizeof(*nt));
    if (!nt) return -1;
    nt->T = T; nt->V = V;
    nt->next = bucket->targets;
    bucket->targets = nt;
    bucket->n_targets++;
    return 1;
}

struct visited { uintptr_t *a; size_t cap; size_t n; };

static int vis_init(struct visited *v, size_t cap)
{
    v->a = calloc(cap, sizeof(uintptr_t));
    if (!v->a) return -1;
    v->cap = cap; v->n = 0; return 0;
}
static void vis_free(struct visited *v)
{
    free(v->a); v->a = NULL; v->cap = 0; v->n = 0;
}
static int vis_grow(struct visited *v)
{
    size_t nc = v->cap * 2;
    uintptr_t *na = calloc(nc, sizeof(uintptr_t));
    if (!na) return -1;
    size_t mask = nc - 1;
    for (size_t i = 0; i < v->cap; i++) {
        if (!v->a[i]) continue;
        size_t j = hash_u64(v->a[i]) & mask;
        while (na[j]) j = (j + 1) & mask;
        na[j] = v->a[i];
    }
    free(v->a); v->a = na; v->cap = nc;
    return 0;
}
static int vis_has(const struct visited *v, uintptr_t P)
{
    if (v->cap == 0) return 0;
    size_t mask = v->cap - 1;
    size_t i = hash_u64(P) & mask;
    for (;;) {
        if (!v->a[i]) return 0;
        if (v->a[i] == P) return 1;
        i = (i + 1) & mask;
    }
}
static int vis_add(struct visited *v, uintptr_t P)
{
    if (v->cap == 0 && vis_init(v, PM_INIT_CAP) != 0) return -1;
    if ((v->n + 1) * 10 >= v->cap * 7 && vis_grow(v) != 0) return -1;
    size_t mask = v->cap - 1;
    size_t i = hash_u64(P) & mask;
    while (v->a[i]) {
        if (v->a[i] == P) return 0;
        i = (i + 1) & mask;
    }
    v->a[i] = P; v->n++; return 1;
}

struct addr_vec { uintptr_t *a; uint32_t n; uint32_t cap; };
static int av_push(struct addr_vec *av, uintptr_t v)
{
    if (av->n >= av->cap) {
        uint32_t nc = av->cap ? av->cap * 2 : BOUNDARY_INIT;
        uintptr_t *na = realloc(av->a, (size_t)nc * sizeof(uintptr_t));
        if (!na) return -1;
        av->a = na; av->cap = nc;
    }
    av->a[av->n++] = v; return 0;
}
static void av_free(struct addr_vec *av)
{
    free(av->a); av->a = NULL; av->n = 0; av->cap = 0;
}

struct anchor_rec {
    uintptr_t P; uint32_t seg_type; uint32_t seg_index;
    uint64_t mod_start; const char *module;
};
struct anchor_vec { struct anchor_rec *a; uint32_t n; uint32_t cap; };
static int ar_push(struct anchor_vec *ar, const struct anchor_rec *r)
{
    if (ar->n >= ar->cap) {
        uint32_t nc = ar->cap ? ar->cap * 2 : 64;
        struct anchor_rec *na = realloc(ar->a, (size_t)nc * sizeof(*na));
        if (!na) return -1;
        ar->a = na; ar->cap = nc;
    }
    ar->a[ar->n++] = *r; return 0;
}
static void ar_free(struct anchor_vec *ar)
{
    free(ar->a); ar->a = NULL; ar->n = 0; ar->cap = 0;
}

struct scan_ctx {
    const struct idx          *ix;
    const struct fs_scan_opts *opts;
    struct pc_list            *cl;
    int                        total_chains;
    struct parent_map          pm;
    struct edge_set            es;
    struct visited             vis;
    struct addr_vec           *next;
    struct anchor_vec          anchors;
    uintptr_t                  target;
    int                        aborted;
    int                        oom;
    uint64_t                   paths_visited;
    uint64_t                   edges_added;
    uint64_t                   edges_dup;

    struct fs_scan_progress   *pg;
    uint64_t                   event_counter;
    uint64_t                   progress_counter;
    uint64_t                   t_start;
    int                        cur_depth;
    uint32_t                   cur_layer_in;
    uint32_t                   cur_layer_out;
    uint64_t                   cur_hits_n;
};

static void fs_scan_progress_set(struct fs_scan_progress *p,
                          const struct fs_scan_progress_info *v)
{
    if (!p || !v) return;
    pthread_mutex_lock(&p->mtx);
    p->info = *v;
    pthread_mutex_unlock(&p->mtx);
}

static void fs_scan_progress_fail(struct fs_scan_progress *p)
{
    if (!p) return;
    pthread_mutex_lock(&p->mtx);
    p->info.phase = FS_SCAN_PHASE_FAILED;
    pthread_mutex_unlock(&p->mtx);
    atomic_store(&p->finished, 1);
}

static void fs_scan_progress_finish_ok(struct fs_scan_progress *p,
                                double total_ms, int cancelled)
{
    if (!p) return;
    pthread_mutex_lock(&p->mtx);
    p->info.phase      = FS_SCAN_PHASE_DONE;
    p->info.elapsed_ms = total_ms;
    p->perf.total_ms   = total_ms;
    p->perf.cancelled  = cancelled;
    pthread_mutex_unlock(&p->mtx);
    atomic_store(&p->finished, 1);
}

static void fs_scan_progress_finish_failed(struct fs_scan_progress *p,
                                    double total_ms)
{
    if (!p) return;
    pthread_mutex_lock(&p->mtx);
    p->info.phase      = FS_SCAN_PHASE_FAILED;
    p->info.elapsed_ms = total_ms;
    p->perf.total_ms   = total_ms;
    pthread_mutex_unlock(&p->mtx);
    atomic_store(&p->finished, 1);
}

static inline int event_tick(struct scan_ctx *ctx)
{
    if ((++ctx->event_counter & EVENT_POLL_MASK) != 0)
        return 0;

    const struct fs_scan_opts *opts = ctx->opts;
    if ((opts->cancel && atomic_load_explicit(opts->cancel,
                                              memory_order_relaxed)) ||
        (ctx->pg && atomic_load_explicit(&ctx->pg->cancelled,
                                         memory_order_relaxed))) {
        ctx->aborted = 1;
        return 1;
    }

    if (ctx->pg) {
        if ((++ctx->progress_counter & PROGRESS_UPDATE_MASK) == 0) {
            struct fs_scan_progress_info v = {
                .phase         = FS_SCAN_PHASE_BFS,
                .depth         = ctx->cur_depth,
                .layer_in      = ctx->cur_layer_in,
                .layer_out     = ctx->cur_layer_out,
                .layer_hits    = ctx->cur_hits_n,
                .total_pm      = ctx->pm.n,
                .total_edges   = ctx->edges_added,
                .total_dup     = ctx->edges_dup,
                .total_anchors = ctx->anchors.n,
                .total_chains  = (uint64_t)ctx->total_chains,
                .elapsed_ms    = ns_to_ms(now_ns() - ctx->t_start),
            };
            fs_scan_progress_set(ctx->pg, &v);
        }
    }
    return 0;
}

static const struct seg *find_seg_by_addr(const struct idx *ix, uintptr_t P)
{
    uint32_t lo = 0, hi = ix->nseg;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ix->segs[mid].start <= P) lo = mid + 1;
        else                          hi = mid;
    }
    if (lo == 0) return NULL;
    uint32_t i = lo - 1;
    if (P >= ix->segs[i].end) return NULL;
    return &ix->segs[i];
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

static int is_anchor(const struct idx *ix, const struct fs_scan_opts *opts,
                     uintptr_t P, struct fs_anchor_info *info)
{
    const struct seg *s = find_seg_by_addr(ix, P);
    if (!s || !s->pathname) return 0;

    if (opts->anchors && opts->anchor_count > 0) {
        if (!seg_match_select(s, opts->anchors, opts->anchor_count)) return 0;
    } else {
        if (s->type != VMA_TYPE_DATA && s->type != VMA_TYPE_BSS) return 0;
        if (s->pathname[0] == '[') return 0;
    }

    uint64_t mod_start = s->mod_start;
    if (mod_start == 0 || mod_start > s->start) mod_start = s->start;
    info->seg_type  = s->type;
    info->seg_index = s->index;
    info->mod_start = mod_start;
    info->module    = s->pathname;
    return 1;
}

static int pc_list_append_chain(struct scan_ctx *ctx,
                                const int32_t *offsets, int count,
                                const char *filename,
                                uint8_t seg_type, uint8_t seg_index)
{
    if (!offsets || count <= 0) return -1;
    struct pc_list *node = ctx->cl;
    while (node) {
        int name_match = (node->filename == NULL && filename[0] == '\0') ||
                         (node->filename && strcmp(node->filename, filename) == 0);
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
        node->seg_type    = seg_type;
        node->seg_index   = seg_index;
        node->chain_count = 0;
        node->chains      = NULL;
        node->next        = ctx->cl;
        ctx->cl           = node;
    }
    struct oc_block *b = node->chains, *last = NULL;
    while (b && atomic_load(&b->used) >= OC_LIST_CAPACITY) { last = b; b = b->next; }
    if (!b) {
        b = calloc(1, sizeof(*b));
        if (!b) return -1;
        atomic_init(&b->used, 0);
        b->next = NULL;
        if (last) last->next = b; else node->chains = b;
    }
    int slot = atomic_load(&b->used);
    struct offset_chain *oc = &b->oc[slot];
    oc->offsets.count  = count;
    oc->offsets.offset = malloc((size_t)count * sizeof(int32_t));
    if (!oc->offsets.offset) return -1;
    memcpy(oc->offsets.offset, offsets, (size_t)count * sizeof(int32_t));
    b->oc_cost[slot] = 0;
    atomic_store(&b->used, slot + 1);
    node->chain_count++;
    return 0;
}

static int emit_chain(struct scan_ctx *ctx,
                      const uintptr_t *path, const uintptr_t *values,
                      int path_len, const struct fs_anchor_info *info)
{
    if (path_len < 1 || path_len > TRACE_MAX) return -1;
    const struct fs_scan_opts *opts = ctx->opts;
    if (opts->min_depth > 0 && path_len < opts->min_depth) return -1;

    int32_t offs[TRACE_MAX];
    uint64_t mod_base = info->mod_start;
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
    const char *filename = info->module ? info->module : "";
    if (pc_list_append_chain(ctx, offs, path_len, filename,
                             (uint8_t)info->seg_type,
                             (uint8_t)info->seg_index) != 0) return -1;
    ctx->total_chains++;
    return 0;
}

static int64_t filter_hits_topk(struct idx_hit *hits, int64_t nh, uint32_t K,
                                uintptr_t target)
{
    if (K == 0 || nh <= 1) return nh;

    struct idx_hit *out = malloc((size_t)nh * sizeof(*out));
    if (!out) return nh;

    int64_t w = 0;
    int64_t i = 0;
    while (i < nh) {
        uint32_t t = hits[i].target_index;
        int64_t j = i;
        while (j < nh && hits[j].target_index == t) j++;

        int64_t seg_n = j - i;
        int64_t picked = 0;
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
        for (int64_t k = i; k < j; k++) hits[k].target_index = t;
        i = j;
    }

    memcpy(hits, out, (size_t)w * sizeof(*out));
    free(out);
    return w;
}

static void explore(struct scan_ctx *ctx,
                    uintptr_t *path, uintptr_t *values, int path_len,
                    const struct fs_anchor_info *info);

static void explore_bucket(struct scan_ctx *ctx,
                           const struct pm_bucket *bucket,
                           uintptr_t *path, uintptr_t *values, int path_len,
                           const struct fs_anchor_info *info)
{
    uint32_t cur_depth = bucket->min_depth;

    for (struct pm_target *t = bucket->targets; t; t = t->next) {
        if (ctx->aborted) return;

        if (event_tick(ctx)) return;

        if (ctx->opts->max_chains > 0 &&
            ctx->total_chains >= ctx->opts->max_chains) {
            ctx->aborted = 1;
            return;
        }

        if (t->T != ctx->target) {
            struct pm_bucket *tb = pm_find(&ctx->pm, t->T);
            if (!tb) continue;
            if (tb->min_depth >= cur_depth) continue;
        }

        values[path_len - 1] = t->V;
        path[path_len]       = t->T;
        ctx->paths_visited++;

        if (t->T == ctx->target) {
            emit_chain(ctx, path, values, path_len + 1, info);
            continue;
        }
        if (path_len + 1 >= TRACE_MAX) continue;
        explore(ctx, path, values, path_len + 1, info);
    }
}

static void explore(struct scan_ctx *ctx,
                    uintptr_t *path, uintptr_t *values, int path_len,
                    const struct fs_anchor_info *info)
{
    if (ctx->aborted) return;
    uintptr_t last = path[path_len - 1];
    struct pm_bucket *bucket = pm_find(&ctx->pm, last);
    if (!bucket) return;
    explore_bucket(ctx, bucket, path, values, path_len, info);
}

static void enumerate_anchor(struct scan_ctx *ctx, const struct anchor_rec *ar)
{
    uintptr_t path[TRACE_MAX];
    uintptr_t values[TRACE_MAX];
    path[0] = ar->P;

    struct fs_anchor_info info = {
        .seg_type  = ar->seg_type,
        .seg_index = ar->seg_index,
        .mod_start = ar->mod_start,
        .module    = ar->module,
    };

    if (ar->P == ctx->target) {
        emit_chain(ctx, path, values, 1, &info);
        return;
    }
    struct pm_bucket *bucket = pm_find(&ctx->pm, ar->P);
    if (!bucket) return;
    explore_bucket(ctx, bucket, path, values, 1, &info);
}

static void process_hit(struct scan_ctx *ctx,
                        uintptr_t P, uintptr_t V, uintptr_t T,
                        uint32_t cur_depth)
{
    const struct fs_scan_opts *opts = ctx->opts;
    if (ctx->aborted) return;
    if (opts->max_chains > 0 && ctx->total_chains >= opts->max_chains) {
        ctx->aborted = 1;
        return;
    }
    if (V > T) return;
    if (P == T) return;
    if (P == ctx->target) return;

    int is_new_node = !vis_has(&ctx->vis, P);
    int is_new = pm_put(&ctx->pm, &ctx->es, P, T, V,
                        max_targets_at(opts, cur_depth), cur_depth, ctx->target);
    if (is_new < 0) { ctx->aborted = 1; ctx->oom = 1; return; }
    if (is_new == 0) { ctx->edges_dup++; return; }
    ctx->edges_added++;

    if (is_new_node) {
        vis_add(&ctx->vis, P);
        if (av_push(ctx->next, P) != 0) { ctx->aborted = 1; ctx->oom = 1; return; }

        struct fs_anchor_info info = {0};
        if (is_anchor(ctx->ix, opts, P, &info)) {
            struct anchor_rec rec = {
                .P = P, .seg_type = info.seg_type, .seg_index = info.seg_index,
                .mod_start = info.mod_start, .module = info.module,
            };
            if (ar_push(&ctx->anchors, &rec) != 0) { ctx->aborted = 1; ctx->oom = 1; }
        }
    }
}
static inline int
tail_layer_offsets(const struct fs_scan_opts *o, int depth,
                   const int32_t **out_offs, int *out_n)
{
    if (!o->tail_flat || !o->tail_starts || o->tail_layer_count <= 0)
        return 0;
    if (depth < 0 || depth >= o->tail_layer_count)
        return 0;
    int s = o->tail_starts[depth];
    int e = o->tail_starts[depth + 1];
    if (e <= s) return 0;
    *out_offs = o->tail_flat + s;
    *out_n    = e - s;
    return 1;
}

struct pc_list *fs_ptrscan(const struct idx *ix, const struct fs_scan_opts *opts)
{
    errno = 0;
    if (!ix || !opts || opts->target == 0) { errno = EINVAL; return NULL; }

    struct fs_scan_progress *pg = opts->progress;

    if (pg) {
        struct fs_scan_progress_info v = {0};
        v.phase = FS_SCAN_PHASE_BFS;
        fs_scan_progress_set(pg, &v);
    }

    struct scan_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ix = ix; ctx.opts = opts; ctx.target = opts->target;
    ctx.pg = pg;
    ctx.t_start = now_ns();

    if (pm_init(&ctx.pm, PM_INIT_CAP) != 0) {
        if (pg) fs_scan_progress_fail(pg);
        errno = ENOMEM; return NULL;
    }
    if (es_init(&ctx.es, 4096) != 0) {
        pm_free(&ctx.pm);
        if (pg) fs_scan_progress_fail(pg);
        errno = ENOMEM; return NULL;
    }
    if (vis_init(&ctx.vis, PM_INIT_CAP) != 0) {
        es_free(&ctx.es); pm_free(&ctx.pm);
        if (pg) fs_scan_progress_fail(pg);
        errno = ENOMEM; return NULL;
    }

    struct addr_vec boundary = {0}, next = {0};
    uint64_t *targets = NULL;
    uint32_t  targets_cap = 0;

    {
        struct fs_anchor_info info = {0};
        if (is_anchor(ix, opts, opts->target, &info)) {
            struct anchor_rec rec = {
                .P = opts->target, .seg_type = info.seg_type,
                .seg_index = info.seg_index, .mod_start = info.mod_start,
                .module = info.module,
            };
            if (ar_push(&ctx.anchors, &rec) != 0) { errno = ENOMEM; goto fail; }
        }
    }

    if (av_push(&boundary, opts->target) != 0) { errno = ENOMEM; goto fail; }

    int max_d = (opts->max_depth > 0) ? opts->max_depth : INT_MAX;

    for (int depth = 0; depth < max_d && boundary.n > 0; depth++) {
        if (ctx.aborted) break;

        if ((opts->cancel && atomic_load(opts->cancel)) ||
            (pg && atomic_load(&pg->cancelled))) {
            ctx.aborted = 1;
            break;
        }

        uint64_t t_layer_0 = now_ns();
        uint32_t b_in = boundary.n;
        uint64_t cur_max_off = max_off_at(opts, depth + 1);
        int      cur_max_targets = max_targets_at(opts, depth + 1);

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

        uint64_t t_idx0 = now_ns();
        struct idx_hit *hits = NULL;
        int64_t nh = idx_scan(ix, targets, boundary.n, cur_max_off, &hits);
        uint64_t t_idx1 = now_ns();

        if (nh < 0) { ctx.oom = 1; break; }
        uint64_t hits_raw = (uint64_t)nh;

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
                        if (delta == (uint64_t)layer[k]) {
                            match = 1;
                            break;
                        }
                    }
                    if (match) hits[w++] = hits[j];
                }
                nh = w;
            }
        }

        if (cur_max_targets > 0 && nh > 0) {
            nh = filter_hits_topk(hits, nh,
                                  (uint32_t)cur_max_targets, ctx.target);
        }
        uint64_t hits_n = (uint64_t)nh;

        next.n = 0;
        ctx.next = &next;

        ctx.cur_depth    = depth + 1;
        ctx.cur_layer_in = b_in;
        ctx.cur_layer_out = boundary.n;
        ctx.cur_hits_n   = hits_n;

        uint64_t t_proc0 = now_ns();
        for (int64_t j = 0; j < nh; j++) {
            if (event_tick(&ctx)) break;

            uintptr_t T = boundary.a[hits[j].target_index];
            process_hit(&ctx, hits[j].source, hits[j].value, T,
                        (uint32_t)(depth + 1));
            if (ctx.aborted) break;
        }
        uint64_t t_proc1 = now_ns();
        free(hits);

        struct addr_vec tmp = boundary; boundary = next; next = tmp;
        uint64_t t_layer_1 = now_ns();

        if (pg) {
            struct fs_scan_progress_info v = {
                .phase         = FS_SCAN_PHASE_BFS,
                .depth         = depth + 1,
                .layer_in      = b_in,
                .layer_out     = boundary.n,
                .layer_hits    = hits_n,
                .total_pm      = ctx.pm.n,
                .total_edges   = ctx.edges_added,
                .total_dup     = ctx.edges_dup,
                .total_anchors = ctx.anchors.n,
                .total_chains  = (uint64_t)ctx.total_chains,
                .elapsed_ms    = ns_to_ms(t_layer_1 - ctx.t_start),
            };
            fs_scan_progress_set(pg, &v);
        }

        (void)hits_raw;
        (void)t_idx0; (void)t_idx1; (void)t_proc0; (void)t_proc1;
        (void)t_layer_0;
    }

    free(targets);

    if (pg && !ctx.aborted) {
        struct fs_scan_progress_info v = {0};
        v.phase         = FS_SCAN_PHASE_ENUM;
        v.enum_total    = (int)ctx.anchors.n;
        v.total_pm      = ctx.pm.n;
        v.total_edges   = ctx.edges_added;
        v.total_dup     = ctx.edges_dup;
        v.total_anchors = ctx.anchors.n;
        v.total_chains  = (uint64_t)ctx.total_chains;
        v.elapsed_ms    = ns_to_ms(now_ns() - ctx.t_start);
        fs_scan_progress_set(pg, &v);
    }

    if (ctx.anchors.n > 0 && !ctx.aborted) {
        for (uint32_t i = 0; i < ctx.anchors.n; i++) {
            if (ctx.aborted) break;

            if ((opts->cancel && atomic_load(opts->cancel)) ||
                (pg && atomic_load(&pg->cancelled))) {
                ctx.aborted = 1;
                break;
            }

            uint64_t before   = ctx.total_chains;
            uint64_t p_before = ctx.paths_visited;

            enumerate_anchor(&ctx, &ctx.anchors.a[i]);

            if (pg) {
                struct fs_scan_progress_info v = {
                    .phase         = FS_SCAN_PHASE_ENUM,
                    .enum_index    = (int)(i + 1),
                    .enum_total    = (int)ctx.anchors.n,
                    .enum_paths    = ctx.paths_visited - p_before,
                    .enum_chains   = (uint64_t)(ctx.total_chains - before),
                    .total_pm      = ctx.pm.n,
                    .total_edges   = ctx.edges_added,
                    .total_dup     = ctx.edges_dup,
                    .total_anchors = ctx.anchors.n,
                    .total_chains  = (uint64_t)ctx.total_chains,
                    .elapsed_ms    = ns_to_ms(now_ns() - ctx.t_start),
                };
                fs_scan_progress_set(pg, &v);
            }
        }
    }

    double total_ms = ns_to_ms(now_ns() - ctx.t_start);

    ar_free(&ctx.anchors);
    av_free(&boundary);
    av_free(&next);
    vis_free(&ctx.vis);
    es_free(&ctx.es);
    pm_free(&ctx.pm);

    if (ctx.oom) {
        if (pg) fs_scan_progress_finish_failed(pg, total_ms);
        free_pc_list(ctx.cl);
        errno = ENOMEM;
        return NULL;
    }

    if (pg) fs_scan_progress_finish_ok(pg, total_ms, ctx.aborted);
    return ctx.cl;

fail:
    free(targets);
    ar_free(&ctx.anchors);
    av_free(&boundary);
    av_free(&next);
    vis_free(&ctx.vis);
    es_free(&ctx.es);
    pm_free(&ctx.pm);
    free_pc_list(ctx.cl);

    if (pg) fs_scan_progress_finish_failed(pg, ns_to_ms(now_ns() - ctx.t_start));
    errno = ENOMEM;
    return NULL;
}

struct fs_scan_progress *fs_scan_progress_create(void)
{
    struct fs_scan_progress *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;

    if (pthread_mutex_init(&p->mtx, NULL) != 0) {
        free(p);
        return NULL;
    }

    memset(&p->info, 0, sizeof(p->info));
    memset(&p->perf, 0, sizeof(p->perf));

    atomic_init(&p->cancelled, 0);
    atomic_init(&p->finished,  0);

    return p;
}

void fs_scan_progress_free(struct fs_scan_progress *p)
{
    if (!p)
        return;
    pthread_mutex_destroy(&p->mtx);
    free(p);
}

void fs_scan_progress_get(const struct fs_scan_progress *p,
                     struct fs_scan_progress_info *out)
{
    if (!p || !out)
        return;

    pthread_mutex_t *m = (pthread_mutex_t *)&p->mtx;

    pthread_mutex_lock(m);
    *out = p->info;
    pthread_mutex_unlock(m);
}

void fs_scan_perf_get(const struct fs_scan_progress *p,
                 struct fs_scan_perf *out)
{
    if (!p || !out)
        return;

    pthread_mutex_t *m = (pthread_mutex_t *)&p->mtx;

    pthread_mutex_lock(m);
    *out = p->perf;
    pthread_mutex_unlock(m);
}

void fs_scan_progress_cancel(struct fs_scan_progress *p)
{
    if (!p)
        return;
    atomic_store(&p->cancelled, 1);
}

int fs_scan_progress_cancelled(const struct fs_scan_progress *p)
{
    if (!p)
        return 0;
    return atomic_load(&p->cancelled);
}

struct fs_scan_opts *fs_scan_opts_create(void)
{
    struct fs_scan_opts *o = calloc(1, sizeof(*o));
    if (!o) return NULL;

    o->target                   = 0;
    o->max_depth                = 0;
    o->max_chains               = 0;
    o->min_depth                = 0;

    o->tail_flat                = NULL;
    o->tail_starts              = NULL;
    o->tail_layer_count         = 0;
    o->max_off                  = NULL;
    o->max_off_len              = 0;
    o->max_targets_per_node     = NULL;
    o->max_targets_per_node_len = 0;

    o->anchors                  = NULL;
    o->anchor_count             = 0;

    o->cancel                   = NULL;
    o->progress                 = NULL;

    return o;
}

void fs_scan_opts_free(struct fs_scan_opts *opts)
{
    if (!opts) return;

    free((void *)opts->tail_flat);
    free((void *)opts->tail_starts);
    free((void *)opts->max_off);
    free((void *)opts->max_targets_per_node);
    free(opts->anchors);

    free(opts);
}

static void *dup_array(const void *src, size_t elem, int n)
{
    if (n <= 0 || !src) return NULL;
    void *p = malloc((size_t)n * elem);
    if (!p) return NULL;
    memcpy(p, src, (size_t)n * elem);
    return p;
}

int fs_scan_opts_set_max_off(struct fs_scan_opts *o,
                             const uint64_t *v, int n)
{
    if (!o) { errno = EINVAL; return -1; }

    uint64_t *na = dup_array(v, sizeof(*na), n);
    if (n > 0 && !na) { errno = ENOMEM; return -1; }

    free((void *)o->max_off);
    o->max_off     = na;
    o->max_off_len = (n > 0) ? n : 0;
    return 0;
}

int fs_scan_opts_set_max_targets_per_node(struct fs_scan_opts *o,
                                          const int *v, int n)
{
    if (!o) { errno = EINVAL; return -1; }

    int *na = dup_array(v, sizeof(*na), n);
    if (n > 0 && !na) { errno = ENOMEM; return -1; }

    free((void *)o->max_targets_per_node);
    o->max_targets_per_node     = na;
    o->max_targets_per_node_len = (n > 0) ? n : 0;
    return 0;
}

int fs_scan_opts_set_anchors(struct fs_scan_opts *o,
                             struct vma_select * const *v, int n)
{
    if (!o) { errno = EINVAL; return -1; }

    struct vma_select **na = dup_array(v, sizeof(*na), n);
    if (n > 0 && !na) { errno = ENOMEM; return -1; }

    free(o->anchors);
    o->anchors      = na;
    o->anchor_count = (n > 0) ? n : 0;
    return 0;
}

int fs_scan_opts_set_tail_layers(struct fs_scan_opts *o,
                                 const int32_t *flat, int flat_n,
                                 const int *starts, int layer_count)
{
    if (!o || flat_n < 0 || layer_count < 0) {
        errno = EINVAL;
        return -1;
    }
    if (layer_count > 0 && (!flat || !starts)) {
        errno = EINVAL;
        return -1;
    }

    int32_t *nf = NULL;
    int     *ns = NULL;

    if (flat_n > 0) {
        nf = malloc((size_t)flat_n * sizeof(int32_t));
        if (!nf) { errno = ENOMEM; return -1; }
        memcpy(nf, flat, (size_t)flat_n * sizeof(int32_t));
    }

    if (layer_count > 0) {
        ns = malloc((size_t)(layer_count + 1) * sizeof(int));
        if (!ns) {
            free(nf);
            errno = ENOMEM;
            return -1;
        }
        memcpy(ns, starts, (size_t)(layer_count + 1) * sizeof(int));
    }

    free((void *)o->tail_flat);
    free((void *)o->tail_starts);

    o->tail_flat        = nf;
    o->tail_starts      = ns;
    o->tail_layer_count = layer_count;

    return 0;
}