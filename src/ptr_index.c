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

#define _GNU_SOURCE

#include "ptr_index.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/uio.h>

#define READ_CHUNK      (8u * 1024 * 1024)
#define SEG_CHUNK_SIZE  (32u * 1024 * 1024)
#define INIT_CAP        4096
#define MAX_THREADS     64
#define RADIX_MIN       8192
#define RADIX_PAR_MIN   100000
#define RADIX_BITS  12
#define RADIX_SIZE  (1u << RADIX_BITS)
#define RADIX_MASK  (RADIX_SIZE - 1u)

#define ENT_SEG_SHIFT   48
#define ENT_SLOT_MASK  ((1ULL << ENT_SEG_SHIFT) - 1ULL)
#define ENT_SEG_MAX    ((1u << (64 - ENT_SEG_SHIFT)) - 1u)   /* 65535 */


struct idx_progress *idx_progress_create(void)
{
    struct idx_progress *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    atomic_init(&p->phase, FS_PTRSCAN_PHASE_IDLE);
    atomic_init(&p->cancel, 0);
    atomic_init(&p->pause, 0);
    atomic_init(&p->total_bytes, 0);
    atomic_init(&p->scanned_bytes, 0);
    atomic_init(&p->total_entries, 0);
    atomic_init(&p->sort_pass, 0);
    atomic_init(&p->sort_total, 0);

    pthread_mutex_init(&p->mtx, NULL);
    pthread_cond_init(&p->cond, NULL);
    return p;
}

void idx_progress_free(struct idx_progress *p)
{
    if (!p) return;
    pthread_mutex_destroy(&p->mtx);
    pthread_cond_destroy(&p->cond);
    free(p);
}

void idx_progress_pause(struct idx_progress *p)
{
    if (!p) return;
    atomic_store(&p->pause, 1);
}

void idx_progress_resume(struct idx_progress *p)
{
    if (!p) return;
    pthread_mutex_lock(&p->mtx);
    atomic_store(&p->pause, 0);
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mtx);
}

void idx_progress_cancel(struct idx_progress *p)
{
    if (!p) return;
    atomic_store(&p->cancel, 1);
    pthread_mutex_lock(&p->mtx);
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mtx);
}

void idx_progress_get(const struct idx_progress *p,
                      struct idx_progress_info *out)
{
    if (!p || !out) return;
    out->phase         = atomic_load(&p->phase);
    out->cancelled     = atomic_load(&p->cancel);
    out->paused        = atomic_load(&p->pause);
    out->total_bytes   = atomic_load(&p->total_bytes);
    out->scanned_bytes = atomic_load(&p->scanned_bytes);
    out->total_entries = atomic_load(&p->total_entries);
    out->sort_pass     = atomic_load(&p->sort_pass);
    out->sort_total    = atomic_load(&p->sort_total);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

struct scan_chunk {
    uint64_t start;
    uint64_t end;
    uint32_t seg_index;
};

struct scan_task {
    const struct idx *ix;
    pid_t             pid;
    fs_ptrscan_process_reader_t reader;
    void             *userdata;

    const struct scan_chunk *chunks;
    uint32_t          chunk_begin;
    uint32_t          chunk_end;

    struct ent       *ents;
    uint64_t          n;
    uint64_t          cap;
    uint64_t          reported_n;

    struct idx_progress *progress;

    int               result;
};

static int cmp_seg(const void *a, const void *b)
{
    const struct seg *x = a, *y = b;
    if (x->start < y->start) return -1;
    if (x->start > y->start) return  1;
    return 0;
}

static int cmp_ent(const void *a, const void *b)
{
    const struct ent *x = a, *y = b;
    if (x->v < y->v) return -1;
    if (x->v > y->v) return  1;
    return 0;
}

static int find_seg(const struct idx *ix, uint64_t v)
{
    uint32_t lo = 0, hi = ix->nseg;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ix->segs[mid].start <= v) lo = mid + 1;
        else                          hi = mid;
    }
    if (lo == 0) return -1;
    uint32_t i = lo - 1;
    if (v < ix->segs[i].end) return (int)i;
    return -1;
}

static int ent_push(struct scan_task *t, uint64_t v, uint64_t meta)
{
    if (t->n >= t->cap) {
        uint64_t nc = t->cap ? t->cap * 2 : INIT_CAP;
        if (nc <= t->cap) return -1;
        struct ent *ne = realloc(t->ents, (size_t)nc * sizeof(*ne));
        if (!ne) return -1;
        t->ents = ne;
        t->cap  = nc;
    }
    t->ents[t->n].v    = v;
    t->ents[t->n].meta = meta;
    t->n++;
    return 0;
}

static int radix_passes(uint64_t vmax)
{
    int passes = 0;
    uint64_t v = vmax;
    do {
        passes++;
        v >>= RADIX_BITS;
    } while (v);
    return passes < 1 ? 1 : passes;
}

static void radix_sort_serial(struct ent *e, uint64_t n, int passes,
                              struct idx_progress *progress)
{
    struct ent *tmp = malloc((size_t)n * sizeof(*tmp));
    if (!tmp) {
        qsort(e, (size_t)n, sizeof(*e), cmp_ent);
        if (progress) atomic_fetch_add(&progress->sort_pass, passes);
        return;
    }

    uint64_t *count = calloc(RADIX_SIZE, sizeof(uint64_t));
    if (!count) {
        free(tmp);
        qsort(e, (size_t)n, sizeof(*e), cmp_ent);
        if (progress) atomic_fetch_add(&progress->sort_pass, passes);
        return;
    }

    struct ent *src = e;
    struct ent *dst = tmp;

    for (int pass = 0; pass < passes; pass++) {
        int shift = pass * RADIX_BITS;
        memset(count, 0, RADIX_SIZE * sizeof(uint64_t));

        for (uint64_t i = 0; i < n; i++)
            count[(src[i].v >> shift) & RADIX_MASK]++;

        uint64_t sum = 0;
        for (uint32_t b = 0; b < RADIX_SIZE; b++) {
            uint64_t c = count[b];
            count[b] = sum;
            sum += c;
        }

        for (uint64_t i = 0; i < n; i++) {
            uint32_t b = (uint32_t)((src[i].v >> shift) & RADIX_MASK);
            dst[count[b]++] = src[i];
        }

        struct ent *swap = src;
        src = dst;
        dst = swap;

        if (progress)
            atomic_fetch_add(&progress->sort_pass, 1);
    }

    if (passes & 1)
        memcpy(e, tmp, (size_t)n * sizeof(*e));

    free(count);
    free(tmp);
}

struct radix_ctx {
    struct ent       *base_a;
    struct ent       *base_b;
    uint64_t          n;
    int               nthreads;
    int               tid;
    int               passes;
    uint64_t         *local_count;
    uint64_t         *my_offset;
    pthread_barrier_t *barrier;
    struct idx_progress *progress;

    /* start protocol : every thread waits for the start signal */
    pthread_mutex_t  *start_mtx;
    pthread_cond_t   *start_cond;
    int              *start_flag;   /* 0=wait, 1=go, -1=abort */
};

static void *radix_worker(void *arg)
{
    struct radix_ctx *ctx = arg;

    /* wait for start signal */
    pthread_mutex_lock(ctx->start_mtx);
    while (*ctx->start_flag == 0)
        pthread_cond_wait(ctx->start_cond, ctx->start_mtx);
    int flag = *ctx->start_flag;
    pthread_mutex_unlock(ctx->start_mtx);

    if (flag < 0)
        return NULL;

    int      tid      = ctx->tid;
    int      nthreads = ctx->nthreads;
    uint64_t n        = ctx->n;
    int      passes   = ctx->passes;

    uint64_t begin = n * (uint64_t)tid       / nthreads;
    uint64_t end   = n * (uint64_t)(tid + 1) / nthreads;

    for (int pass = 0; pass < passes; pass++) {
        int shift = pass * RADIX_BITS;
        struct ent *src = (pass & 1) ? ctx->base_b : ctx->base_a;
        struct ent *dst = (pass & 1) ? ctx->base_a : ctx->base_b;

        uint64_t *lc = ctx->local_count + (size_t)tid * RADIX_SIZE;
        memset(lc, 0, RADIX_SIZE * sizeof(uint64_t));

        for (uint64_t i = begin; i < end; i++)
            lc[(src[i].v >> shift) & RADIX_MASK]++;

        pthread_barrier_wait(ctx->barrier);

        uint64_t *off = ctx->my_offset + (size_t)tid * RADIX_SIZE;
        uint64_t pos = 0;
        for (uint32_t b = 0; b < RADIX_SIZE; b++) {
            uint64_t my_start = pos;
            for (int t = 0; t < tid; t++)
                my_start += ctx->local_count[(size_t)t * RADIX_SIZE + b];
            off[b] = my_start;

            for (int t = 0; t < nthreads; t++)
                pos += ctx->local_count[(size_t)t * RADIX_SIZE + b];
        }

        for (uint64_t i = begin; i < end; i++) {
            uint32_t b = (uint32_t)((src[i].v >> shift) & RADIX_MASK);
            dst[off[b]++] = src[i];
        }

        pthread_barrier_wait(ctx->barrier);

        if (ctx->tid == 0 && ctx->progress)
            atomic_fetch_add(&ctx->progress->sort_pass, 1);
    }

    return NULL;
}

static int radix_sort_parallel(struct ent *e, uint64_t n, int passes,
                               int nthreads, struct idx_progress *progress)
{
    if (nthreads < 2 || n < RADIX_PAR_MIN)
        return -1;

    struct ent *tmp = malloc((size_t)n * sizeof(*tmp));
    if (!tmp) return -1;

    uint64_t *local_count = calloc((size_t)nthreads * RADIX_SIZE, sizeof(uint64_t));
    uint64_t *my_offset   = malloc((size_t)nthreads * RADIX_SIZE * sizeof(uint64_t));
    if (!local_count || !my_offset) {
        free(tmp); free(local_count); free(my_offset);
        return -1;
    }

    pthread_barrier_t barrier;
    if (pthread_barrier_init(&barrier, NULL, (unsigned)nthreads) != 0) {
        free(tmp); free(local_count); free(my_offset);
        return -1;
    }

    pthread_mutex_t start_mtx;
    pthread_cond_t  start_cond;
    pthread_mutex_init(&start_mtx, NULL);
    pthread_cond_init(&start_cond, NULL);
    int start_flag = 0;

    struct radix_ctx *ctxs = malloc((size_t)nthreads * sizeof(*ctxs));
    pthread_t        *tids = malloc((size_t)nthreads * sizeof(pthread_t));
    if (!ctxs || !tids) {
        pthread_barrier_destroy(&barrier);
        pthread_mutex_destroy(&start_mtx);
        pthread_cond_destroy(&start_cond);
        free(tmp); free(local_count); free(my_offset);
        free(ctxs); free(tids);
        return -1;
    }

    for (int t = 0; t < nthreads; t++) {
        ctxs[t].base_a      = e;
        ctxs[t].base_b      = tmp;
        ctxs[t].n           = n;
        ctxs[t].nthreads    = nthreads;
        ctxs[t].tid         = t;
        ctxs[t].passes      = passes;
        ctxs[t].local_count = local_count;
        ctxs[t].my_offset   = my_offset;
        ctxs[t].barrier     = &barrier;
        ctxs[t].progress    = progress;
        ctxs[t].start_mtx   = &start_mtx;
        ctxs[t].start_cond  = &start_cond;
        ctxs[t].start_flag  = &start_flag;
    }

    int started = 0;
    for (int t = 0; t < nthreads; t++) {
        if (pthread_create(&tids[t], NULL, radix_worker, &ctxs[t]) != 0)
            break;
        started++;
    }

    pthread_mutex_lock(&start_mtx);

    if (started < nthreads) {
        start_flag = -1;
        pthread_cond_broadcast(&start_cond);
        pthread_mutex_unlock(&start_mtx);

        for (int t = 0; t < started; t++)
            pthread_join(tids[t], NULL);

        pthread_barrier_destroy(&barrier);
        pthread_mutex_destroy(&start_mtx);
        pthread_cond_destroy(&start_cond);
        free(ctxs); free(tids);
        free(local_count); free(my_offset);
        free(tmp);
        return -1;
    }

    start_flag = 1;
    pthread_cond_broadcast(&start_cond);
    pthread_mutex_unlock(&start_mtx);

    for (int t = 0; t < nthreads; t++)
        pthread_join(tids[t], NULL);

    pthread_barrier_destroy(&barrier);
    pthread_mutex_destroy(&start_mtx);
    pthread_cond_destroy(&start_cond);

    if (passes & 1)
        memcpy(e, tmp, (size_t)n * sizeof(*e));

    free(ctxs);
    free(tids);
    free(local_count);
    free(my_offset);
    free(tmp);
    return 0;
}

static void radix_sort_entries(struct ent *e, uint64_t n, uint64_t vmax,
                               int nthreads, struct idx_progress *progress)
{
    if (n < 2) {
        if (progress) {
            atomic_store(&progress->sort_total, 1);
            atomic_store(&progress->sort_pass, 1);
        }
        return;
    }

    if (n < RADIX_MIN) {
        if (progress) {
            atomic_store(&progress->sort_total, 1);
            atomic_store(&progress->sort_pass, 0);
        }
        qsort(e, (size_t)n, sizeof(*e), cmp_ent);
        if (progress)
            atomic_store(&progress->sort_pass, 1);
        return;
    }

    int passes = radix_passes(vmax);

    if (progress) {
        atomic_store(&progress->sort_total, passes);
        atomic_store(&progress->sort_pass, 0);
    }

    if (radix_sort_parallel(e, n, passes, nthreads, progress) == 0)
        return;

    if (progress)
        atomic_store(&progress->sort_pass, 0);

    radix_sort_serial(e, n, passes, progress);
}

static int scan_task_run(struct scan_task *t)
{
    uint8_t *buf = malloc(READ_CHUNK);
    if (!buf) return -1;

    const uint64_t vmin   = t->ix->vmin;
    const uint64_t vrange = t->ix->vmax - t->ix->vmin;

    for (uint32_t c = t->chunk_begin; c < t->chunk_end; c++) {
        const struct scan_chunk *ch = &t->chunks[c];
        const uint64_t end      = ch->end;
        const uint32_t sid      = ch->seg_index;
        const uint64_t seg_base = t->ix->segs[sid].start;

        uint64_t pos = ch->start;

        while (pos < end) {
            if (t->progress) {
                if (atomic_load(&t->progress->cancel)) { free(buf); return -2; }
                if (atomic_load(&t->progress->pause)) {
                    pthread_mutex_lock(&t->progress->mtx);
                    while (atomic_load(&t->progress->pause) &&
                           !atomic_load(&t->progress->cancel))
                        pthread_cond_wait(&t->progress->cond, &t->progress->mtx);
                    pthread_mutex_unlock(&t->progress->mtx);
                    if (atomic_load(&t->progress->cancel)) { free(buf); return -2; }
                }
            }

            size_t want = (size_t)(end - pos);
            if (want > READ_CHUNK) want = READ_CHUNK;

            struct iovec local  = { .iov_base = buf,                    .iov_len = want };
            struct iovec remote = { .iov_base = (void *)(uintptr_t)pos, .iov_len = want };

            ssize_t r = t->reader(t->pid, &local, 1, &remote, 1, t->userdata);

            if (r <= 0) {
                uint64_t next = (pos + 4095) & ~(uint64_t)4095;
                if (next <= pos) next = pos + 4096;
                uint64_t stop = (next < end) ? next : end;

                if (t->progress)
                    atomic_fetch_add(&t->progress->scanned_bytes, stop - pos);

                if (next >= end) break;
                pos = next;
                continue;
            }

            size_t valid  = (size_t)r;
            size_t nslots = valid >> 3;
            const uint64_t *slots = (const uint64_t *)buf;

            for (size_t i = 0; i < nslots; i++) {
                uint64_t v = slots[i];
                if ((uint64_t)(v - vmin) > vrange) continue;
                if (find_seg(t->ix, v) < 0)        continue;

                uint64_t byte_off = (uint64_t)(pos + (i << 3) - seg_base);
                uint64_t meta = ((uint64_t)sid << ENT_SEG_SHIFT)
                              | ((byte_off >> 3) & ENT_SLOT_MASK);

                if (ent_push(t, v, meta) != 0) { free(buf); return -1; }
            }

            if (t->progress) {
                atomic_fetch_add(&t->progress->scanned_bytes, valid);
                uint64_t delta = t->n - t->reported_n;
                if (delta) {
                    atomic_fetch_add(&t->progress->total_entries, delta);
                    t->reported_n = t->n;
                }
            }

            if (valid < want) {
                uint64_t next = (pos + valid + 4095) & ~(uint64_t)4095;
                if (next <= pos) next = pos + 4096;
                uint64_t stop = (next < end) ? next : end;

                if (t->progress)
                    atomic_fetch_add(&t->progress->scanned_bytes,
                                     stop - (pos + valid));

                if (next >= end) break;
                pos = next;
            } else {
                pos += valid & ~(size_t)7;
            }
        }
    }

    free(buf);
    return 0;
}

static void *task_thread(void *arg)
{
    struct scan_task *t = arg;
    t->result = scan_task_run(t);
    return NULL;
}

struct merge_arg {
    struct ent       *dst;
    const struct ent *src;
    uint64_t          n;
};

static void *merge_worker(void *arg)
{
    struct merge_arg *m = arg;
    if (m->n > 0)
        memcpy(m->dst, m->src, (size_t)m->n * sizeof(struct ent));
    return NULL;
}

struct idx *idx_build(pid_t pid, const struct vm_area *vma,
                      fs_ptrscan_process_reader_t reader, void *userdata,
                      int jobs, struct idx_progress *progress)
{
    if (!vma || !reader) return NULL;

    if (progress)
        atomic_store(&progress->phase, FS_PTRSCAN_PHASE_SCAN);

    uint64_t t_total_begin = now_ns();

    struct idx *ix = calloc(1, sizeof(*ix));
    if (!ix) { if (progress) atomic_store(&progress->phase, FS_PTRSCAN_PHASE_FAILED); return NULL; }

    /* 1. count segments */
    uint32_t nseg = 0;
    for (const struct vm_area *v = vma; v; v = v->next) {
        if (v->end <= v->start) continue;
        nseg++;
    }
    if (nseg == 0) {
        if (progress) atomic_store(&progress->phase, FS_PTRSCAN_PHASE_FAILED);
        free(ix);
        return NULL;
    }

    if (nseg > ENT_SEG_MAX) {
        if (progress) atomic_store(&progress->phase, FS_PTRSCAN_PHASE_FAILED);
        free(ix);
        return NULL;
    }

    ix->segs = calloc(nseg, sizeof(*ix->segs));
    if (!ix->segs) {
        if (progress) atomic_store(&progress->phase, FS_PTRSCAN_PHASE_FAILED);
        free(ix);
        return NULL;
    }

    /* 2. copy segments */
    uint64_t t_seg_begin = now_ns();

    uint32_t n = 0;
    uint64_t bytes_total = 0;
    for (const struct vm_area *v = vma; v; v = v->next) {
        if (v->end <= v->start) continue;

        struct seg *s = &ix->segs[n];
        s->start = v->start;
        s->end   = v->end;
        s->type  = v->seg_type;
        s->index = v->seg_index;
        s->mod_start = v->module ? v->module->start : 0;

        if (v->pathname) {
            s->pathname = strdup(v->pathname);
            if (!s->pathname) goto fail_segs;
        }
        bytes_total += v->end - v->start;
        n++;
    }
    ix->nseg = n;

    uint64_t t_seg_copy_end = now_ns();

    /* 3. sort segments */
    qsort(ix->segs, n, sizeof(*ix->segs), cmp_seg);

    uint64_t t_seg_sort_end = now_ns();

    /* 4. vmin / vmax */
    ix->vmin = ix->segs[0].start;
    ix->vmax = ix->segs[0].end - 1;
    for (uint32_t i = 1; i < n; i++) {
        if (ix->segs[i].end - 1 > ix->vmax)
            ix->vmax = ix->segs[i].end - 1;
    }

    /* 5. expand to chunks */
    uint64_t total_chunks = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t sz = ix->segs[i].end - ix->segs[i].start;
        total_chunks += (sz + SEG_CHUNK_SIZE - 1) / SEG_CHUNK_SIZE;
    }
    if (total_chunks == 0) { ix->perf.jobs_used = 0; return ix; }
    if (total_chunks > UINT32_MAX) goto fail_segs;

    struct scan_chunk *chunks = malloc((size_t)total_chunks * sizeof(*chunks));
    if (!chunks) goto fail_segs;

    uint32_t ci = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t seg_start = ix->segs[i].start;
        uint64_t sz        = ix->segs[i].end - seg_start;

        for (uint64_t off = 0; off < sz; off += SEG_CHUNK_SIZE) {
            uint64_t cend = off + SEG_CHUNK_SIZE;
            if (cend > sz) cend = sz;
            chunks[ci].start     = seg_start + off;
            chunks[ci].end       = seg_start + cend;
            chunks[ci].seg_index = i;
            ci++;
        }
    }

    /* 6. decide thread count */
    if (jobs <= 0) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        jobs = (ncpu > 0) ? (int)ncpu : 1;
    }
    if (jobs > MAX_THREADS) jobs = MAX_THREADS;
    if ((uint32_t)jobs > ci) jobs = (int)ci;
    if (jobs < 1)            jobs = 1;

    ix->perf.jobs_used = jobs;

    /* initialize progress */
    if (progress) {
        atomic_store(&progress->total_bytes,   bytes_total);
        atomic_store(&progress->scanned_bytes, 0);
        atomic_store(&progress->total_entries, 0);
        atomic_store(&progress->phase,         FS_PTRSCAN_PHASE_SCAN);
    }

    struct scan_task *tasks = calloc((size_t)jobs, sizeof(*tasks));
    if (!tasks) { free(chunks); goto fail_segs; }

    for (int t = 0; t < jobs; t++) {
        tasks[t].ix          = ix;
        tasks[t].pid         = pid;
        tasks[t].reader      = reader;
        tasks[t].userdata    = userdata;
        tasks[t].chunks      = chunks;
        tasks[t].chunk_begin = (uint32_t)((uint64_t)ci *  t      / jobs);
        tasks[t].chunk_end   = (uint32_t)((uint64_t)ci * (t + 1) / jobs);
        tasks[t].progress    = progress;
    }

    /* 7. scan */
    uint64_t t_scan_begin = now_ns();

    if (jobs == 1) {
        tasks[0].chunk_begin = 0;
        tasks[0].chunk_end   = ci;
        tasks[0].result      = scan_task_run(&tasks[0]);
    } else {
        pthread_t *tids = malloc((size_t)jobs * sizeof(pthread_t));
        if (!tids) goto fail_tasks_chunks;

        int started = 0;
        for (int t = 0; t < jobs; t++) {
            if (pthread_create(&tids[t], NULL, task_thread, &tasks[t]) != 0)
                break;
            started++;
        }
        for (int t = 0; t < started; t++)
            pthread_join(tids[t], NULL);
        free(tids);

        if (started < jobs) goto fail_tasks_chunks;
    }

    for (int t = 0; t < jobs; t++)
        if (tasks[t].result != 0) goto fail_tasks_chunks;

    uint64_t t_scan_end = now_ns();

    if (progress)
        atomic_store(&progress->phase, FS_PTRSCAN_PHASE_MERGE);

    /* 8. parallel merge */
    uint64_t t_merge_begin = now_ns();

    uint64_t total = 0;
    for (int t = 0; t < jobs; t++)
        total += tasks[t].n;

    if (total > 0) {
        ix->e = malloc((size_t)total * sizeof(*ix->e));
        if (!ix->e) goto fail_tasks_chunks;

        uint64_t offsets[MAX_THREADS];
        uint64_t pos = 0;
        for (int t = 0; t < jobs; t++) {
            offsets[t] = pos;
            pos += tasks[t].n;
        }

        if (jobs == 1) {
            if (tasks[0].n > 0)
                memcpy(ix->e, tasks[0].ents,
                       (size_t)tasks[0].n * sizeof(*ix->e));
        } else {
            struct merge_arg margs[MAX_THREADS];
            pthread_t        mtids[MAX_THREADS];

            for (int t = 0; t < jobs; t++) {
                margs[t].dst = ix->e + offsets[t];
                margs[t].src = tasks[t].ents;
                margs[t].n   = tasks[t].n;
            }

            int mstarted = 0;
            for (int t = 0; t < jobs; t++) {
                if (pthread_create(&mtids[t], NULL, merge_worker, &margs[t]) != 0)
                    break;
                mstarted++;
            }

            for (int t = mstarted; t < jobs; t++) {
                if (margs[t].n > 0)
                    memcpy(margs[t].dst, margs[t].src,
                           (size_t)margs[t].n * sizeof(*ix->e));
            }

            for (int t = 0; t < mstarted; t++)
                pthread_join(mtids[t], NULL);
        }

        ix->n   = total;
        ix->cap = total;
    }

    for (int t = 0; t < jobs; t++) free(tasks[t].ents);
    free(tasks);
    free(chunks);

    uint64_t t_merge_end = now_ns();

    if (progress)
        atomic_store(&progress->phase, FS_PTRSCAN_PHASE_SORT);

    /* 9. radix sort */
    uint64_t t_radix_begin = now_ns();

    if (ix->n > 1)
        radix_sort_entries(ix->e, ix->n, ix->vmax, jobs, progress);

    uint64_t t_radix_end = now_ns();

    /* 10. perf counters */
    ix->perf.seg_copy_ns   = t_seg_copy_end   - t_seg_begin;
    ix->perf.seg_sort_ns   = t_seg_sort_end   - t_seg_copy_end;
    ix->perf.scan_ns       = t_scan_end       - t_scan_begin;
    ix->perf.merge_ns      = t_merge_end      - t_merge_begin;
    ix->perf.radix_sort_ns = t_radix_end      - t_radix_begin;
    ix->perf.total_ns      = t_radix_end      - t_total_begin;
    ix->perf.bytes_scanned = bytes_total;
    ix->perf.entries       = ix->n;

    if (progress)
        atomic_store(&progress->phase, FS_PTRSCAN_PHASE_DONE);

    return ix;

fail_tasks_chunks:
    if (progress) atomic_store(&progress->phase, FS_PTRSCAN_PHASE_FAILED);
    for (int t = 0; t < jobs; t++) free(tasks[t].ents);
    free(tasks);
    free(chunks);

fail_segs:
    if (progress) atomic_store(&progress->phase, FS_PTRSCAN_PHASE_FAILED);
    for (uint32_t j = 0; j < n; j++)
        free(ix->segs[j].pathname);
    free(ix->segs);
    free(ix->e);
    free(ix);
    return NULL;
}

void idx_free(struct idx *ix)
{
    if (!ix) return;

    if (ix->segs) {
        for (uint32_t i = 0; i < ix->nseg; i++)
            free(ix->segs[i].pathname);
        free(ix->segs);
    }
    free(ix->e);
    free(ix);
}

int64_t idx_scan(const struct idx *ix,
                 const uint64_t *targets, uint32_t n,
                 uint64_t delta,
                 struct idx_hit **out)
{
    if (out) *out = NULL;

    if (!ix || !targets || n == 0 || !out) return 0;
    if (ix->n == 0) return 0;

    uint64_t total = 0;
    for (uint32_t t = 0; t < n; t++) {
        uint64_t lo = targets[t];
        uint64_t hi = (delta > UINT64_MAX - lo) ? UINT64_MAX : lo + delta;

        uint64_t L = 0, R = ix->n;
        while (L < R) {
            uint64_t m = L + (R - L) / 2;
            if (ix->e[m].v < lo) L = m + 1; else R = m;
        }
        for (uint64_t i = L; i < ix->n && ix->e[i].v <= hi; i++) {
    uint64_t v = ix->e[i].v;
    if (v & 7ULL) continue;
    if (find_seg(ix, v) < 0) continue;
    total++;
}
    }
    if (total == 0) return 0;

    struct idx_hit *hits = malloc((size_t)total * sizeof(*hits));
    if (!hits) return -1;

    uint64_t k = 0;
    for (uint32_t t = 0; t < n; t++) {
        uint64_t lo = targets[t];
        uint64_t hi = (delta > UINT64_MAX - lo) ? UINT64_MAX : lo + delta;

        uint64_t L = 0, R = ix->n;
        while (L < R) {
            uint64_t m = L + (R - L) / 2;
            if (ix->e[m].v < lo) L = m + 1; else R = m;
        }
        for (uint64_t i = L; i < ix->n && ix->e[i].v <= hi; i++) {
    const struct ent *e = &ix->e[i];
    uint64_t v = e->v;

    if (v & 7ULL) continue;

    if (find_seg(ix, v) < 0) continue;

    uint32_t seg  = (uint32_t)(e->meta >> ENT_SEG_SHIFT);
    uint64_t slot = e->meta & ENT_SLOT_MASK;

    hits[k].target_index = t;
    hits[k].value        = v;
    hits[k].source       = ix->segs[seg].start + (slot << 3);
    k++;
}
    }

    *out = hits;
    return (int64_t)k;
}

uint64_t idx_count(const struct idx *ix)     { return ix ? ix->n    : 0; }
uint32_t idx_seg_count(const struct idx *ix) { return ix ? ix->nseg : 0; }

int idx_find_seg(const struct idx *ix, uintptr_t value)
{
    if (!ix) return -1;
    return find_seg(ix, value);
}