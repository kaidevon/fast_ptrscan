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

#include "ptr_index.h"
#include "fs_ptrscan.h"
#include "callback.h"
#include "pc_list.h"
#include "format/idx.h"
#include "format/pcf.h"
#include "format/txt.h"
#include "vma/vm_area.h"
#include "vma/vma_select.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>
#include <limits.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <sys/uio.h>
#include <sys/types.h>

#define VERSION "1.0.0-rc.1"

#define BLUE       "\x1b[34m"
#define BLUE_BOLD  "\x1b[1m\x1b[34m"
#define BOLD       "\x1b[1m"
#define RED        "\x1b[31m"
#define GREEN      "\x1b[32m"
#define YELLOW     "\x1b[33m"
#define RED_BOLD   "\x1b[1m\x1b[31m"
#define RESET      "\x1b[0m"

static int use_color = 1;

#define C_BLUE       (use_color ? BLUE     : "")
#define C_BLUE_BOLD  (use_color ? BLUE_BOLD: "")
#define C_BOLD       (use_color ? BOLD     : "")
#define C_RED        (use_color ? RED      : "")
#define C_GREEN      (use_color ? GREEN    : "")
#define C_YELLOW     (use_color ? YELLOW   : "")
#define C_RED_BOLD   (use_color ? RED_BOLD : "")
#define C_RESET      (use_color ? RESET    : "")

#define OPT_ARR_LEN     1024
#define TAIL_FLAT_MAX   512
#define TAIL_LAYERS_MAX 64

#define ANALYZE_MAX_FILES 16
#define ANALYZE_MAX_TAIL  16
#define ANALYZE_KEY_MAX   2048

/* Progress frame. */
#define FRAME_TOP_PER_LAYER  3
#define FRAME_POLL_US        100000
#define SMOOTH_ALPHA         0.25

/* SIGINT */

static struct fs_progress  *g_progress     = NULL;
static struct idx_progress *g_idx_progress = NULL;

static void on_sigint(int sig)
{
    (void)sig;
    if (g_progress)
        atomic_store_explicit(&g_progress->cancelled, 1,
                              memory_order_relaxed);
    if (g_idx_progress)
        atomic_store_explicit(&g_idx_progress->cancel, 1,
                              memory_order_relaxed);
}

static void install_sigint(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL +
           (uint64_t)ts.tv_nsec / 1000000ULL;
}

static int detect_color(void)
{
    if (getenv("NO_COLOR")) return 0;
    if (getenv("CLICOLOR_FORCE")) return 1;
    const char *term = getenv("TERM");
    if (term && strcmp(term, "dumb") == 0) return 0;
    return isatty(STDOUT_FILENO);
}

/* Smoothing */

struct smooth_state {
    double in, out, hits, pm, anc, chains;
    int    init;
    int    last_depth;
};

static void smooth_update(struct smooth_state *s, int depth,
                          uint64_t in, uint64_t out, uint64_t hits,
                          uint64_t pm, uint64_t anc, uint64_t chains)
{
    if (!s->init || depth != s->last_depth) {
        s->in = (double)in;
        s->out = (double)out;
        s->hits = (double)hits;
        s->pm = (double)pm;
        s->anc = (double)anc;
        s->chains = (double)chains;
        s->init = 1;
        s->last_depth = depth;
        return;
    }
    s->in     += ((double)in     - s->in)     * SMOOTH_ALPHA;
    s->out    += ((double)out    - s->out)    * SMOOTH_ALPHA;
    s->hits   += ((double)hits   - s->hits)   * SMOOTH_ALPHA;
    s->pm     += ((double)pm     - s->pm)     * SMOOTH_ALPHA;
    s->anc    += ((double)anc    - s->anc)    * SMOOTH_ALPHA;
    s->chains += ((double)chains - s->chains) * SMOOTH_ALPHA;
}

static uint64_t smooth_u64(double v)
{
    return (v < 0.0) ? 0 : (uint64_t)(v + 0.5);
}

/* Argument parsing */

static void fill_max_off(uint64_t *arr, uint64_t off)
{
    for (int i = 0; i < OPT_ARR_LEN; i++) arr[i] = off;
}

static int parse_int_arg(const char *s, int *out)
{
    if (!s || !*s) return -1;
    char *end; errno = 0;
    long v = strtol(s, &end, 0);
    if (end == s || *end != '\0' || errno == ERANGE) return -1;
    if (v < INT_MIN || v > INT_MAX) return -1;
    *out = (int)v; return 0;
}

static int parse_u64_arg(const char *s, uint64_t *out)
{
    if (!s || !*s) return -1;
    char *end; errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (end == s || *end != '\0' || errno == ERANGE) return -1;
    *out = (uint64_t)v; return 0;
}

static int parse_k_list(const char *s, int *arr)
{
    for (int i = 0; i < OPT_ARR_LEN; i++) arr[i] = 0;
    if (!s || !*s) return 0;
    int i = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (i >= OPT_ARR_LEN) return -1;
        char *end = NULL; errno = 0;
        long v = strtol(p, &end, 0);
        if (end == p || errno == ERANGE || v < 0 || v > INT_MAX)
            return -1;
        arr[i++] = (int)v;
        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') { p++; continue; }
        if (*p == '\0') break;
        return -1;
    }
    if (i > 0 && i < OPT_ARR_LEN) {
        int last = arr[i - 1];
        while (i < OPT_ARR_LEN) arr[i++] = last;
    }
    return 0;
}

static int parse_tail_layer(const char *s,
                            int32_t *flat, int flat_cap, int *flat_n,
                            int *starts, int starts_cap, int *layer_count)
{
    if (!s || !*s) return -1;
    if (*layer_count >= starts_cap - 1) return -1;
    int s_idx = *flat_n;
    int32_t *dst = flat + s_idx;
    int cap = flat_cap - s_idx;
    if (cap <= 0) return -1;
    int n = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *end = NULL; errno = 0;
        long v = strtol(p, &end, 0);
        if (end == p || errno == ERANGE || v < INT32_MIN || v > INT32_MAX)
            return -1;
        if (n >= cap) return -1;
        dst[n++] = (int32_t)v;
        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ',') { p++; continue; }
        if (*p == '\0') break;
        return -1;
    }
    if (n == 0) return -1;
    *flat_n = s_idx + n;
    (*layer_count)++;
    starts[*layer_count] = *flat_n;
    return 0;
}

/* scan progress thread */

struct progress_monitor {
    struct fs_progress *pg;
    pthread_t           tid;
    _Atomic int         stop;
    FILE               *log_fp;
};

static const char *phase_name(int p)
{
    switch (p) {
    case FS_SCAN_PHASE_IDLE:   return "idle";
    case FS_SCAN_PHASE_BFS:    return "bfs";
    case FS_SCAN_PHASE_ENUM:   return "enum";
    case FS_SCAN_PHASE_DONE:   return "done";
    case FS_SCAN_PHASE_FAILED: return "failed";
    }
    return "?";
}

static void print_slot_line(FILE *fp, const struct fs_hist_slot *h)
{
    char path[64];
    if (h->prev < 0)
        snprintf(path, sizeof(path), "0x%x", (unsigned)h->delta);
    else
        snprintf(path, sizeof(path), "0x%x -> 0x%x",
                 (unsigned)h->prev, (unsigned)h->delta);

    if (fp == stderr) {
        const char *c = (h->anc > 0) ? C_GREEN : C_YELLOW;
        if (h->anc > 0)
            fprintf(fp, "  %s%-24s%s %10lu    %sanc=%lu%s\n",
                    c, path, C_RESET,
                    (unsigned long)h->hits,
                    C_GREEN, (unsigned long)h->anc, C_RESET);
        else
            fprintf(fp, "  %s%-24s%s %10lu\n",
                    c, path, C_RESET, (unsigned long)h->hits);
    } else {
        if (h->anc > 0)
            fprintf(fp, "  %-24s %10lu    anc=%lu\n",
                    path, (unsigned long)h->hits,
                    (unsigned long)h->anc);
        else
            fprintf(fp, "  %-24s %10lu\n",
                    path, (unsigned long)h->hits);
    }
}

static void print_layer_stderr(int depth, const struct fs_hist_layer *L)
{
    uint64_t anc_sum = 0;
    for (int k = 0; k < L->n; k++) anc_sum += L->slots[k].anc;

    fprintf(stderr,
            "\n%sLayer %d%s  boundary_in=%lu  deltas=%d  "
            "total_hits=%lu  anchors=%lu\n",
            C_BLUE_BOLD, depth, C_RESET,
            (unsigned long)L->boundary_in, L->n,
            (unsigned long)L->total_hits,
            (unsigned long)anc_sum);

    int show = L->n < FRAME_TOP_PER_LAYER ? L->n : FRAME_TOP_PER_LAYER;
    for (int k = 0; k < show; k++)
        print_slot_line(stderr, &L->slots[k]);

    if (L->n > show) {
        uint64_t rest = 0;
        for (int k = show; k < L->n; k++) rest += L->slots[k].hits;
        fprintf(stderr, "  %s... (%d more, hits=%lu)%s\n",
                C_YELLOW, L->n - show,
                (unsigned long)rest, C_RESET);
    }
}

static void print_layer_log(FILE *log, int depth,
                            const struct fs_hist_layer *L)
{
    if (!log) return;
    uint64_t anc_sum = 0;
    for (int k = 0; k < L->n; k++) anc_sum += L->slots[k].anc;

    fprintf(log, "Layer %d  boundary_in=%lu  deltas=%d  "
                 "total_hits=%lu  anchors=%lu\n",
            depth,
            (unsigned long)L->boundary_in, L->n,
            (unsigned long)L->total_hits,
            (unsigned long)anc_sum);

    for (int k = 0; k < L->n; k++)
        print_slot_line(log, &L->slots[k]);
    fprintf(log, "\n");
    fflush(log);
}

static int flush_layer(struct fs_progress *pg, FILE *log_fp, int d)
{
    struct fs_hist_layer one;
    if (fs_progress_hist_snapshot_one(pg, d, &one) != 0) return -1;

    fs_progress_hist_sort(&one);
    print_layer_stderr(d, &one);
    print_layer_log(log_fp, d, &one);
    fs_progress_hist_release(&one, 1);
    return 0;
}

static void *progress_thread(void *arg)
{
    struct progress_monitor *pm = arg;
    int last_printed = 0;
    struct smooth_state sm = {0};
    FILE *log_fp = pm->log_fp;

    while (!atomic_load(&pm->stop)) {
        struct fs_progress *pg = pm->pg;

        int phase     = atomic_load_explicit(&pg->phase, memory_order_relaxed);
        int cur_depth = atomic_load_explicit(&pg->depth, memory_order_relaxed);
        int hist_max  = atomic_load_explicit(&pg->hist_max_depth,
                                             memory_order_relaxed);

        int done = (phase == FS_SCAN_PHASE_BFS) ? (cur_depth - 1) : hist_max;
        if (done < 0) done = 0;

        while (last_printed < done) {
            fprintf(stderr, "\r\x1b[2K");
            int d = last_printed + 1;
            flush_layer(pg, log_fp, d);
            last_printed = d;
        }

        uint64_t in     = atomic_load_explicit(&pg->layer_in,     memory_order_relaxed);
        uint64_t out    = atomic_load_explicit(&pg->layer_out,    memory_order_relaxed);
        uint64_t hits   = atomic_load_explicit(&pg->layer_hits,   memory_order_relaxed);
        uint64_t pmv    = atomic_load_explicit(&pg->total_pm,     memory_order_relaxed);
        uint64_t anc    = atomic_load_explicit(&pg->total_anchors,memory_order_relaxed);
        uint64_t chains = atomic_load_explicit(&pg->total_chains, memory_order_relaxed);
        uint64_t start  = atomic_load_explicit(&pg->start_ms,     memory_order_relaxed);
        int enum_i = atomic_load_explicit(&pg->enum_index, memory_order_relaxed);
        int enum_t = atomic_load_explicit(&pg->enum_total, memory_order_relaxed);

        uint64_t s_in, s_out, s_hits, s_pm, s_anc, s_chains;
        if (phase == FS_SCAN_PHASE_DONE || phase == FS_SCAN_PHASE_FAILED) {
            s_in = in; s_out = out; s_hits = hits;
            s_pm = pmv; s_anc = anc; s_chains = chains;
        } else {
            smooth_update(&sm, cur_depth, in, out, hits, pmv, anc, chains);
            s_in     = smooth_u64(sm.in);
            s_out    = smooth_u64(sm.out);
            s_hits   = smooth_u64(sm.hits);
            s_pm     = smooth_u64(sm.pm);
            s_anc    = smooth_u64(sm.anc);
            s_chains = smooth_u64(sm.chains);
        }

        uint64_t now = now_ms();
        double el = (start > 0) ? (double)(now - start) / 1000.0 : 0.0;

        if (phase == FS_SCAN_PHASE_ENUM) {
            double pct = (enum_t > 0) ? (100.0 * enum_i / enum_t) : 0.0;
            fprintf(stderr,
                    "\r%s[%-6s]%s d=%-2d  in=%-8lu  out=%-8lu  "
                    "hits=%-10lu  pm=%-9lu  anc=%-6lu  "
                    "enum=%d/%d (%.1f%%)  chains=%-8lu  %.1fs",
                    C_BLUE_BOLD, phase_name(phase), C_RESET,
                    cur_depth, s_in, s_out, s_hits, s_pm, s_anc,
                    enum_i, enum_t, pct,
                    s_chains, el);
        } else {
            fprintf(stderr,
                    "\r%s[%-6s]%s d=%-2d  in=%-8lu  out=%-8lu  "
                    "hits=%-10lu  pm=%-9lu  anc=%-6lu  chains=%-8lu  %.1fs",
                    C_BLUE_BOLD, phase_name(phase), C_RESET,
                    cur_depth, s_in, s_out, s_hits, s_pm, s_anc,
                    s_chains, el);
        }
        fflush(stderr);

        usleep(FRAME_POLL_US);
    }

    /* Wrap up. */
    struct fs_progress *pg = pm->pg;
    int phase     = atomic_load_explicit(&pg->phase, memory_order_relaxed);
    int cur_depth = atomic_load_explicit(&pg->depth, memory_order_relaxed);
    int hist_max  = atomic_load_explicit(&pg->hist_max_depth,
                                         memory_order_relaxed);

    int done = (phase == FS_SCAN_PHASE_BFS) ? (cur_depth - 1) : hist_max;
    if (done < 0) done = 0;

    while (last_printed < done) {
        fprintf(stderr, "\r\x1b[2K");
        int d = last_printed + 1;
        flush_layer(pg, log_fp, d);
        last_printed = d;
    }

    uint64_t in     = atomic_load_explicit(&pg->layer_in,     memory_order_relaxed);
    uint64_t out    = atomic_load_explicit(&pg->layer_out,    memory_order_relaxed);
    uint64_t hits   = atomic_load_explicit(&pg->layer_hits,   memory_order_relaxed);
    uint64_t pmv    = atomic_load_explicit(&pg->total_pm,     memory_order_relaxed);
    uint64_t anc    = atomic_load_explicit(&pg->total_anchors,memory_order_relaxed);
    uint64_t chains = atomic_load_explicit(&pg->total_chains, memory_order_relaxed);
    uint64_t start  = atomic_load_explicit(&pg->start_ms,     memory_order_relaxed);
    int enum_i = atomic_load_explicit(&pg->enum_index, memory_order_relaxed);
    int enum_t = atomic_load_explicit(&pg->enum_total, memory_order_relaxed);

    uint64_t now = now_ms();
    double el = (start > 0) ? (double)(now - start) / 1000.0 : 0.0;

    if (phase == FS_SCAN_PHASE_ENUM || phase == FS_SCAN_PHASE_DONE) {
        double pct = (enum_t > 0) ? (100.0 * enum_i / enum_t) : 0.0;
        fprintf(stderr,
                "\r\x1b[2K%s[%-6s]%s d=%-2d  in=%-8lu  out=%-8lu  "
                "hits=%-10lu  pm=%-9lu  anc=%-6lu  "
                "enum=%d/%d (%.1f%%)  chains=%-8lu  %.1fs\n",
                C_BLUE_BOLD, phase_name(phase), C_RESET,
                cur_depth, in, out, hits, pmv, anc,
                enum_i, enum_t, pct, chains, el);
    } else {
        fprintf(stderr,
                "\r\x1b[2K%s[%-6s]%s d=%-2d  in=%-8lu  out=%-8lu  "
                "hits=%-10lu  pm=%-9lu  anc=%-6lu  chains=%-8lu  %.1fs\n",
                C_BLUE_BOLD, phase_name(phase), C_RESET,
                cur_depth, in, out, hits, pmv, anc, chains, el);
    }

    if (log_fp) {
        fprintf(log_fp, "### FINAL ###\n");
        fprintf(log_fp, "phase=%s  depth=%d  enum=%d/%d  "
                        "pm=%lu  anc=%lu  chains=%lu\n",
                phase_name(phase), cur_depth, enum_i, enum_t,
                (unsigned long)pmv, (unsigned long)anc,
                (unsigned long)chains);
        fflush(log_fp);
    }

    fflush(stderr);
    return NULL;
}

/* index progress thread */

struct idx_progress_monitor {
    struct idx_progress *pg;
    pthread_t            tid;
    _Atomic int          stop;
    uint64_t             start_ms;
};

static const char *idx_phase_name(int p)
{
    switch (p) {
    case FS_PTRSCAN_PHASE_IDLE:   return "idle";
    case FS_PTRSCAN_PHASE_SCAN:   return "scan";
    case FS_PTRSCAN_PHASE_MERGE:  return "merge";
    case FS_PTRSCAN_PHASE_SORT:   return "sort";
    case FS_PTRSCAN_PHASE_DONE:   return "done";
    case FS_PTRSCAN_PHASE_FAILED: return "failed";
    }
    return "?";
}

static void *idx_progress_thread(void *arg)
{
    struct idx_progress_monitor *pm = arg;

    while (!atomic_load(&pm->stop)) {
        struct idx_progress_info cur;
        idx_progress_get(pm->pg, &cur);

        uint64_t now = now_ms();
        double   el  = (double)(now - pm->start_ms) / 1000.0;

        double pct = 0.0;
        if (cur.total_bytes > 0)
            pct = 100.0 * (double)cur.scanned_bytes
                        / (double)cur.total_bytes;

        if (cur.phase == FS_PTRSCAN_PHASE_SCAN) {
            fprintf(stderr,
                    "\r%s[idx  ]%s scan    "
                    "bytes=%5.1fM/%-5.1fM (%5.1f%%)  "
                    "entries=%-9lu  %.1fs",
                    C_BLUE_BOLD, C_RESET,
                    (double)cur.scanned_bytes / (1024.0 * 1024.0),
                    (double)cur.total_bytes   / (1024.0 * 1024.0),
                    pct,
                    (unsigned long)cur.total_entries,
                    el);
        } else if (cur.phase == FS_PTRSCAN_PHASE_MERGE) {
            fprintf(stderr,
                    "\r%s[idx  ]%s merge   entries=%-9lu  %.1fs",
                    C_BLUE_BOLD, C_RESET,
                    (unsigned long)cur.total_entries, el);
        } else if (cur.phase == FS_PTRSCAN_PHASE_SORT) {
            fprintf(stderr,
                    "\r%s[idx  ]%s sort    pass=%d/%d  entries=%-9lu  %.1fs",
                    C_BLUE_BOLD, C_RESET,
                    cur.sort_pass, cur.sort_total,
                    (unsigned long)cur.total_entries, el);
        } else {
            fprintf(stderr,
                    "\r%s[idx  ]%s %-6s  entries=%-9lu  %.1fs",
                    C_BLUE_BOLD, C_RESET,
                    idx_phase_name(cur.phase),
                    (unsigned long)cur.total_entries, el);
        }
        fflush(stderr);

        usleep(FRAME_POLL_US);
    }

    struct idx_progress_info cur;
    idx_progress_get(pm->pg, &cur);

    uint64_t now = now_ms();
    double   el  = (double)(now - pm->start_ms) / 1000.0;

    fprintf(stderr,
            "\r\x1b[2K%s[idx  ]%s %-6s  entries=%-9lu  %.1fs\n",
            C_BLUE_BOLD, C_RESET,
            idx_phase_name(cur.phase),
            (unsigned long)cur.total_entries, el);

    return NULL;
}

/* reader / vma */

static ssize_t default_reader(pid_t pid,
                              const struct iovec *local_iov,
                              unsigned long liovcnt,
                              const struct iovec *remote_iov,
                              unsigned long riovcnt,
                              void *userdata)
{
    (void)userdata;
    return process_vm_readv(pid, local_iov, liovcnt,
                            remote_iov, riovcnt, 0);
}

static struct vm_area *load_vma(pid_t pid)
{
    struct vm_area *vma = NULL;
    int rc = parse_maps(pid, &vma);
    if (rc != 0) {
        fprintf(stderr, "%serror:%s cannot parse maps for pid %d: %s\n",
                C_RED_BOLD, C_RESET, (int)pid,
                strerror(rc < 0 ? -rc : rc));
        return NULL;
    }
    (void)vma_elf(vma);
    return vma;
}

/* Summary */

static void print_summary(const struct fs_result *result,
                          const struct fs_progress *pg)
{
    uint64_t bs = atomic_load(&result->perf.bfs_start_ns);
    uint64_t be = atomic_load(&result->perf.bfs_end_ns);
    uint64_t es = atomic_load(&result->perf.enum_start_ns);
    uint64_t ee = atomic_load(&result->perf.enum_end_ns);

    double bfs_ms  = (be > bs) ? (double)(be - bs) / 1e6 : 0.0;
    double enum_ms = (ee > es) ? (double)(ee - es) / 1e6 : 0.0;

    int      n_groups = 0;
    uint64_t n_chains = 0;
    int      min_len = INT_MAX, max_len = 0;
    uint64_t sum_len = 0;

    for (const struct pc_list *p = result->chains; p; p = p->next) {
        n_groups++;
        for (struct oc_block *b = p->chains; b; b = b->next) {
            int used = atomic_load(&b->used);
            for (int i = 0; i < used; i++) {
                int L = b->oc[i].offsets.count;
                n_chains++;
                sum_len += (uint64_t)L;
                if (L < min_len) min_len = L;
                if (L > max_len) max_len = L;
            }
        }
    }
    if (n_chains == 0) min_len = 0;

    int      depth_reached = atomic_load(&pg->max_depth_reached);
    uint64_t total_pm      = atomic_load(&pg->total_pm);
    uint64_t total_edges   = atomic_load(&pg->total_edges);
    uint64_t total_dup     = atomic_load(&pg->total_dup);
    uint64_t total_anchors = atomic_load(&pg->total_anchors);

    fprintf(stderr, "%sSummary:%s\n", C_BOLD, C_RESET);
    fprintf(stderr, "  time      bfs %.3fs   enum %.3fs\n",
            bfs_ms / 1000.0, enum_ms / 1000.0);
    fprintf(stderr, "  depth     %d\n", depth_reached);
    fprintf(stderr, "  pm        %lu buckets   edges %lu +%lu dup\n",
            (unsigned long)total_pm,
            (unsigned long)total_edges,
            (unsigned long)total_dup);
    fprintf(stderr, "  anchors   %lu\n", (unsigned long)total_anchors);
    fprintf(stderr, "  chains    %lu in %d group%s\n",
            (unsigned long)n_chains, n_groups,
            n_groups == 1 ? "" : "s");
    if (n_chains > 0)
        fprintf(stderr, "  length    %d ~ %d   avg %.1f\n",
                min_len, max_len, (double)sum_len / (double)n_chains);
    fprintf(stderr, "\n");
}

/* scan */

static int run_scan_and_output(const struct idx *ix, uintptr_t target,
                               int depth, int min_depth, int max_chains,
                               const int *mt, uint64_t offset,
                               const int32_t *tail_flat, int tail_flat_n,
                               const int *tail_starts, int tail_layer_count,
                               const char *out_file, FILE *log_fp)
{
    uint64_t off[OPT_ARR_LEN];
    fill_max_off(off, offset);

    struct fs_opts opts;
    memset(&opts, 0, sizeof(opts));
    opts.target                   = target;
    opts.max_depth                = depth;
    opts.min_depth                = min_depth;
    opts.max_chains               = max_chains;
    opts.max_off                  = off;
    opts.max_off_len              = OPT_ARR_LEN;
    opts.max_targets_per_node     = mt;
    opts.max_targets_per_node_len = OPT_ARR_LEN;

    if (tail_layer_count > 0 && tail_flat_n > 0) {
        opts.tail_flat        = tail_flat;
        opts.tail_starts      = tail_starts;
        opts.tail_layer_count = tail_layer_count;
    }

    struct fs_progress *pg = create_fs_progress();
    if (!pg) {
        fprintf(stderr, "%serror:%s cannot create progress\n",
                C_RED_BOLD, C_RESET);
        return 1;
    }

    g_progress = pg;

    if (log_fp) {
        fprintf(log_fp, "# ptrscan log\n");
        fprintf(log_fp, "# target=0x%lx\n", (unsigned long)target);
        fprintf(log_fp, "# depth=%d  min_depth=%d  max_chains=%d  offset=0x%lx\n",
                depth, min_depth, max_chains, (unsigned long)offset);
        fprintf(log_fp, "#\n\n");
        fflush(log_fp);
    }

    struct progress_monitor pm;
    memset(&pm, 0, sizeof(pm));
    int have_pg = 0;
    pm.pg = pg;
    pm.log_fp = log_fp;
    atomic_init(&pm.stop, 0);
    if (pthread_create(&pm.tid, NULL, progress_thread, &pm) == 0)
        have_pg = 1;
    else
        fprintf(stderr, "%swarn:%s cannot start progress thread\n",
                C_YELLOW, C_RESET);

    struct fs_result *result = NULL;
    int rc = fs_ptrscan(ix, &opts, pg, &result);

    if (have_pg) {
        atomic_store(&pm.stop, 1);
        pthread_join(pm.tid, NULL);
    }

    g_progress = NULL;

    if (rc == FS_SCAN_FAILED) {
        fprintf(stderr, "\n%serror:%s scan failed\n", C_RED_BOLD, C_RESET);
        free_fs_progress(pg);
        if (result) free_fs_result(&result);
        return 1;
    }
    if (rc == FS_SCAN_CANCELLED) {
        fprintf(stderr, "\n%scancelled%s\n", C_YELLOW, C_RESET);
        free_fs_progress(pg);
        if (result) free_fs_result(&result);
        return 1;
    }

    if (!result || !result->chains) {
        printf("%s(no chains found)%s\n", C_YELLOW, C_RESET);
        free_fs_progress(pg);
        if (result) free_fs_result(&result);
        return 0;
    }

    print_summary(result, pg);
    free_fs_progress(pg);

    int out_rc = 0;
    if (out_file) {
        FILE *fp = fopen(out_file, "wb");
        if (!fp) {
            fprintf(stderr, "%serror:%s cannot open '%s': %s\n",
                    C_RED_BOLD, C_RESET, out_file, strerror(errno));
            out_rc = 1;
        } else {
            const char *ext = strrchr(out_file, '.');
            int sv;
            if (ext && strcasecmp(ext, ".pcf") == 0)
                sv = pcf_write_pc_list_everything(fp, result->chains);
            else
                sv = txt_save(fp, result->chains);
            fclose(fp);
            if (sv != 0) {
                fprintf(stderr, "%serror:%s save failed\n",
                        C_RED_BOLD, C_RESET);
                out_rc = 1;
            } else {
                printf("%swrote:%s %s\n", C_GREEN, C_RESET, out_file);
            }
        }
    } else {
        txt_save(stdout, result->chains);
    }
    free_fs_result(&result);
    return out_rc;
}

/* usage */

static void usage(void)
{
    printf("Fast in-process pointer-chain scanner\n\n");
    printf("%sUsage:%s ptrscan [COMMAND] [ARGS]\n\n", C_BOLD, C_RESET);
    printf("%sCommands:%s\n", C_BOLD, C_RESET);

#define CMD_ROW(name, desc) \
    printf("  %s%-14s%s%s\n", C_GREEN, name, C_RESET, desc)

    CMD_ROW("index",   "Build a pointer index from a running process and save to IDX");
    CMD_ROW("scan",    "Build index on the fly (or load from IDX) and scan pointer chains");
    CMD_ROW("analyze", "Intersect chains/tails across multiple scan outputs");
    CMD_ROW("help",    "Print this message or the help of the given subcommand(s)");

#undef CMD_ROW

    printf("\n%sOptions:%s\n", C_BOLD, C_RESET);
    printf("  %s%-14s%s%s\n", C_GREEN, "-h, --help",    C_RESET, "Print help");
    printf("  %s%-14s%s%s\n", C_GREEN, "-V, --version", C_RESET, "Print version");
}

/* index */

static int cmd_index(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "%serror:%s missing <PID>\n",
                C_RED_BOLD, C_RESET);
        return 2;
    }
    pid_t pid = (pid_t)atoi(argv[1]);
    const char *out_file = NULL;
    int jobs = 0;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] && a[1] != '-' && a[2]) {
            if      (a[1] == 'j') { if (parse_int_arg(a + 2, &jobs) == 0) continue; }
            else if (a[1] == 'o') { out_file = a + 2; continue; }
        }
        if ((!strcmp(a, "-o") || !strcmp(a, "--output")) && i + 1 < argc)
            out_file = argv[++i];
        else if ((!strcmp(a, "-j") || !strcmp(a, "--jobs")) && i + 1 < argc) {
            if (parse_int_arg(argv[++i], &jobs) != 0) {
                fprintf(stderr, "%serror:%s invalid -j value\n",
                        C_RED_BOLD, C_RESET);
                return 2;
            }
        } else {
            fprintf(stderr, "%serror:%s unknown option '%s'\n",
                    C_RED_BOLD, C_RESET, a);
            return 2;
        }
    }
    if (pid <= 0) {
        fprintf(stderr, "%serror:%s invalid pid '%s'\n",
                C_RED_BOLD, C_RESET, argv[1]);
        return 2;
    }

    struct vm_area *vma = load_vma(pid);
    if (!vma) return 1;

    struct idx_progress *ipg = idx_progress_create();
    if (!ipg) {
        fprintf(stderr, "%serror:%s cannot create progress\n",
                C_RED_BOLD, C_RESET);
        free_vm_area(vma);
        return 1;
    }

    g_idx_progress = ipg;

    struct idx_progress_monitor pm;
    memset(&pm, 0, sizeof(pm));
    pm.pg        = ipg;
    pm.start_ms  = now_ms();
    atomic_init(&pm.stop, 0);

    int have_pg = 0;
    if (pthread_create(&pm.tid, NULL, idx_progress_thread, &pm) == 0)
        have_pg = 1;
    else
        fprintf(stderr, "%swarn:%s cannot start progress thread\n",
                C_YELLOW, C_RESET);

    struct idx *ix = idx_build(pid, vma, default_reader, NULL, jobs, ipg);
    free_vm_area(vma);

    if (have_pg) {
        atomic_store(&pm.stop, 1);
        pthread_join(pm.tid, NULL);
    }

    g_idx_progress = NULL;
    idx_progress_free(ipg);

    if (!ix) {
        fprintf(stderr, "%serror:%s index build failed\n",
                C_RED_BOLD, C_RESET);
        return 1;
    }

    printf("%ssegments:%s %u\n", C_GREEN, C_RESET, idx_seg_count(ix));
    printf("%sentries:%s  %lu\n", C_GREEN, C_RESET,
           (unsigned long)idx_count(ix));

    int rc = 0;
    if (out_file) {
        FILE *fp = fopen(out_file, "wb");
        if (!fp) {
            fprintf(stderr, "%serror:%s cannot open '%s': %s\n",
                    C_RED_BOLD, C_RESET, out_file, strerror(errno));
            rc = 1;
        } else {
            int sv = idx_save(fp, ix);
            fclose(fp);
            if (sv != IDX_OK) {
                fprintf(stderr, "%serror:%s save failed: %s\n",
                        C_RED_BOLD, C_RESET, idx_strerror(sv));
                rc = 1;
            } else {
                printf("%swrote:%s    %s\n", C_GREEN, C_RESET, out_file);
            }
        }
    }
    idx_free(ix);
    return rc;
}

/* scan args */

struct scan_args {
    pid_t    pid;
    int      depth;
    int      min_depth;
    int      max_chains;
    int      jobs;
    int      k_list[OPT_ARR_LEN];
    uint64_t offset;
    const char *out_file;
    const char *idx_file;
    const char *log_file;
    int32_t  tail_flat[TAIL_FLAT_MAX];
    int      tail_starts[TAIL_LAYERS_MAX + 1];
    int      tail_flat_n;
    int      tail_layer_count;
};

static int parse_scan_args(int argc, char **argv, int start_i,
                           int allow_jobs, struct scan_args *out)
{
    out->pid        = 0;
    out->depth      = 6;
    out->min_depth  = 0;
    out->max_chains = 0;
    out->jobs       = 0;
    out->offset     = 0x1000;
    out->out_file   = NULL;
    out->idx_file   = NULL;
    out->log_file   = NULL;
    for (int j = 0; j < OPT_ARR_LEN; j++) out->k_list[j] = 3;

    out->tail_flat_n      = 0;
    out->tail_layer_count = 0;
    out->tail_starts[0]   = 0;

    int i = start_i;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] && a[1] != '-' && a[2]) {
            const char *v = a + 2;
            switch (a[1]) {
            case 'd': if (parse_int_arg(v, &out->depth)      == 0) continue; break;
            case 'm': if (parse_int_arg(v, &out->min_depth)  == 0) continue; break;
            case 'n': if (parse_int_arg(v, &out->max_chains) == 0) continue; break;
            case 'j':
                if (allow_jobs && parse_int_arg(v, &out->jobs) == 0) continue;
                break;
            case 'k': if (parse_k_list(v, out->k_list)       == 0) continue; break;
            case 'O': if (parse_u64_arg(v, &out->offset)     == 0) continue; break;
            case 'o': out->out_file = v; continue;
            case 'i': out->idx_file = v; continue;
            case 'L': out->log_file = v; continue;
            case 'p': {
                int pid_v;
                if (parse_int_arg(v, &pid_v) == 0) {
                    out->pid = (pid_t)pid_v;
                    continue;
                }
                break;
            }
            case 't':
                if (parse_tail_layer(v, out->tail_flat, TAIL_FLAT_MAX,
                                     &out->tail_flat_n,
                                     out->tail_starts, TAIL_LAYERS_MAX + 1,
                                     &out->tail_layer_count) == 0)
                    continue;
                break;
            default: break;
            }
        }
        if      ((!strcmp(a, "-d") || !strcmp(a, "--depth"))      && i + 1 < argc)
            { if (parse_int_arg(argv[++i], &out->depth) != 0) goto bad; }
        else if ((!strcmp(a, "-m") || !strcmp(a, "--min-depth"))  && i + 1 < argc)
            { if (parse_int_arg(argv[++i], &out->min_depth) != 0) goto bad; }
        else if ((!strcmp(a, "-n") || !strcmp(a, "--max-chains")) && i + 1 < argc)
            { if (parse_int_arg(argv[++i], &out->max_chains) != 0) goto bad; }
        else if ((!strcmp(a, "-k") || !strcmp(a, "--max-targets"))&& i + 1 < argc)
            { if (parse_k_list(argv[++i], out->k_list) != 0) goto bad; }
        else if ((!strcmp(a, "-O") || !strcmp(a, "--offset"))     && i + 1 < argc)
            { if (parse_u64_arg(argv[++i], &out->offset) != 0) goto bad; }
        else if ((!strcmp(a, "-o") || !strcmp(a, "--output"))     && i + 1 < argc)
            out->out_file = argv[++i];
        else if ((!strcmp(a, "-i") || !strcmp(a, "--idx"))        && i + 1 < argc)
            out->idx_file = argv[++i];
        else if ((!strcmp(a, "-L") || !strcmp(a, "--log"))        && i + 1 < argc)
            out->log_file = argv[++i];
        else if ((!strcmp(a, "-p") || !strcmp(a, "--pid"))        && i + 1 < argc) {
            int pid_v;
            if (parse_int_arg(argv[++i], &pid_v) != 0) goto bad;
            out->pid = (pid_t)pid_v;
        }
        else if ((!strcmp(a, "-t") || !strcmp(a, "--tail"))       && i + 1 < argc)
            { if (parse_tail_layer(argv[++i], out->tail_flat, TAIL_FLAT_MAX,
                                   &out->tail_flat_n,
                                   out->tail_starts, TAIL_LAYERS_MAX + 1,
                                   &out->tail_layer_count) != 0) goto bad; }
        else if (allow_jobs &&
                 (!strcmp(a, "-j") || !strcmp(a, "--jobs"))       && i + 1 < argc)
            { if (parse_int_arg(argv[++i], &out->jobs) != 0) goto bad; }
        else
            goto bad;
    }
    return 0;
bad:
    fprintf(stderr, "%serror:%s unknown or malformed option '%s'\n",
            C_RED_BOLD, C_RESET, argv[i]);
    return -1;
}

static int cmd_scan(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "%serror:%s usage:\n", C_RED_BOLD, C_RESET);
        fprintf(stderr, "  ptrscan scan <PID> <TARGET> [options]\n");
        fprintf(stderr, "  ptrscan scan <TARGET> -p <PID> [options]\n");
        fprintf(stderr, "  ptrscan scan <TARGET> -i <FILE.idx> [options]\n");
        return 2;
    }
    pid_t     pos_pid = 0;
    uintptr_t target  = 0;
    int       scan_start;
    if (argc >= 3 && argv[2][0] != '-') {
        pos_pid    = (pid_t)atoi(argv[1]);
        target     = (uintptr_t)strtoull(argv[2], NULL, 0);
        scan_start = 3;
    } else {
        target     = (uintptr_t)strtoull(argv[1], NULL, 0);
        scan_start = 2;
    }
    struct scan_args sa;
    if (parse_scan_args(argc, argv, scan_start, 1, &sa) != 0)
        return 2;
    if (target == 0) {
        fprintf(stderr, "%serror:%s invalid target\n",
                C_RED_BOLD, C_RESET);
        return 2;
    }
    pid_t pid = sa.pid > 0 ? sa.pid : pos_pid;

    struct idx *ix = NULL;
    if (sa.idx_file) {
        FILE *fp = fopen(sa.idx_file, "rb");
        if (!fp) {
            fprintf(stderr, "%serror:%s cannot open '%s': %s\n",
                    C_RED_BOLD, C_RESET, sa.idx_file, strerror(errno));
            return 1;
        }
        int lrc = idx_load(fp, &ix);
        fclose(fp);
        if (lrc != IDX_OK) {
            fprintf(stderr, "%serror:%s load failed: %s\n",
                    C_RED_BOLD, C_RESET, idx_strerror(lrc));
            return 1;
        }
    } else {
        if (pid <= 0) {
            fprintf(stderr,
                    "%serror:%s no PID (use <PID> <TARGET>, -p <PID>, "
                    "or -i <FILE.idx>)\n",
                    C_RED_BOLD, C_RESET);
            return 2;
        }
        struct vm_area *vma = load_vma(pid);
        if (!vma) return 1;
        ix = idx_build(pid, vma, default_reader, NULL, sa.jobs, NULL);
        free_vm_area(vma);
        if (!ix) {
            fprintf(stderr, "%serror:%s index build failed\n",
                    C_RED_BOLD, C_RESET);
            return 1;
        }
    }

    FILE *log_fp = NULL;
    if (sa.log_file) {
        log_fp = fopen(sa.log_file, "w");
        if (!log_fp) {
            fprintf(stderr, "%serror:%s cannot open log '%s': %s\n",
                    C_RED_BOLD, C_RESET, sa.log_file, strerror(errno));
            idx_free(ix);
            return 1;
        }
    }

    int rc = run_scan_and_output(ix, target, sa.depth, sa.min_depth,
                                 sa.max_chains, sa.k_list, sa.offset,
                                 sa.tail_flat, sa.tail_flat_n,
                                 sa.tail_starts, sa.tail_layer_count,
                                 sa.out_file, log_fp);

    if (log_fp) fclose(log_fp);
    idx_free(ix);
    return rc;
}

/* analyze */

/* Load pc_list: choose txt/pcf by file extension. */
static int analyze_load(const char *path, struct pc_list **out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    const char *ext = strrchr(path, '.');
    int rc;
    if (ext && strcasecmp(ext, ".pcf") == 0)
        rc = pcf_read_pc_list_everything(fp, out, NULL);
    else
        rc = txt_load(fp, out);
    fclose(fp);
    return rc;
}

/* Find a group in head matching ref (module + seg_type + seg_index). */
static struct pc_list *analyze_find_group(struct pc_list *head,
                                          const struct pc_list *ref)
{
    for (struct pc_list *g = head; g; g = g->next) {
        if (g->seg_type != ref->seg_type) continue;
        if (g->seg_index != ref->seg_index) continue;
        const char *a = g->filename ? g->filename : "";
        const char *b = ref->filename ? ref->filename : "";
        if (strcmp(a, b) == 0) return g;
    }
    return NULL;
}

static const char *ana_seg_name(int seg_type)
{
    switch (seg_type) {
    case VMA_TYPE_TEXT:   return "text";
    case VMA_TYPE_RODATA: return "rodata";
    case VMA_TYPE_DATA:   return "data";
    case VMA_TYPE_BSS:    return "bss";
    }
    return "unknown";
}

/* String set (for chain existence queries). */

struct str_set {
    char **strs;
    int    n;
    int    cap;
};

static int str_set_add(struct str_set *s, const char *key)
{
    if (s->n >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 1024;
        char **ns = realloc(s->strs, (size_t)nc * sizeof(char *));
        if (!ns) return -1;
        s->strs = ns;
        s->cap = nc;
    }
    s->strs[s->n] = strdup(key);
    if (!s->strs[s->n]) return -1;
    s->n++;
    return 0;
}

static void str_set_free(struct str_set *s)
{
    if (!s) return;
    for (int i = 0; i < s->n; i++) free(s->strs[i]);
    free(s->strs);
    s->strs = NULL;
    s->n = s->cap = 0;
}

static int cmp_str_ptr(const void *a, const void *b)
{
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}

static void str_set_sort(struct str_set *s)
{
    if (s->n > 1)
        qsort(s->strs, (size_t)s->n, sizeof(char *), cmp_str_ptr);
}

static int str_set_has(const struct str_set *s, const char *key)
{
    int lo = 0, hi = s->n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int c = strcmp(s->strs[mid], key);
        if (c < 0) lo = mid + 1;
        else if (c > 0) hi = mid;
        else return 1;
    }
    return 0;
}

/* Encode a chain's offset sequence into a string key. */
static void chain_to_key(const int32_t *offs, int n, char *buf, size_t bufsize)
{
    size_t pos = 0;
    if (bufsize == 0) return;
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        int w;
        if (i == 0)
            w = snprintf(buf + pos, bufsize - pos,
                         "%x", (unsigned)(uint32_t)offs[i]);
        else
            w = snprintf(buf + pos, bufsize - pos,
                         ",%x", (unsigned)(uint32_t)offs[i]);
        if (w < 0 || (size_t)w >= bufsize - pos) {
            buf[pos < bufsize ? pos : bufsize - 1] = '\0';
            return;
        }
        pos += (size_t)w;
    }
}

/* pc_list result construction. */

static struct oc_block *pc_list_ensure_block(struct pc_list *p)
{
    struct oc_block *b = p->chains, *last = NULL;
    while (b && atomic_load(&b->used) >= OC_LIST_CAPACITY) {
        last = b;
        b = b->next;
    }
    if (!b) {
        b = calloc(1, sizeof(*b));
        if (!b) return NULL;
        atomic_init(&b->used, 0);
        b->next = NULL;
        if (last) last->next = b;
        else p->chains = b;
    }
    return b;
}

static int pc_list_add_chain(struct pc_list *p, const int32_t *offs, int n)
{
    if (!p || !offs || n <= 0) return -1;
    struct oc_block *b = pc_list_ensure_block(p);
    if (!b) return -1;

    int slot = atomic_load(&b->used);
    struct offset_chain *oc = &b->oc[slot];
    oc->offsets.count  = n;
    oc->offsets.offset = malloc((size_t)n * sizeof(int32_t));
    if (!oc->offsets.offset) return -1;
    memcpy(oc->offsets.offset, offs, (size_t)n * sizeof(int32_t));
    b->oc_cost[slot] = 0;
    atomic_store(&b->used, slot + 1);
    p->chain_count++;
    return 0;
}

/*
 * Full chain intersection: keep only chains present in all input files.
 * Group by (module + seg_type + seg_index). For each group in lists[0],
 * look for the same group in every other file; if found, intersect the
 * chains of that group and output a new pc_list.
 */
static struct pc_list *pc_list_intersect_all(struct pc_list **lists,
                                             int n_files)
{
    if (n_files < 2) return NULL;

    struct pc_list *result = NULL, *result_tail = NULL;

    for (struct pc_list *ref = lists[0]; ref; ref = ref->next) {
        struct pc_list *sub[ANALYZE_MAX_FILES];
        int n_sub = 1;
        sub[0] = ref;

        int all_found = 1;
        for (int i = 1; i < n_files; i++) {
            struct pc_list *g = analyze_find_group(lists[i], ref);
            if (!g) { all_found = 0; break; }
            sub[n_sub++] = g;
        }
        if (!all_found) continue;

        /* Build string sets for files 2..N-1. */
        struct str_set sets[ANALYZE_MAX_FILES];
        memset(sets, 0, sizeof(sets));

        for (int i = 1; i < n_files; i++) {
            for (struct oc_block *b = sub[i]->chains; b; b = b->next) {
                int used = atomic_load(&b->used);
                for (int j = 0; j < used; j++) {
                    struct offset_chain *oc = &b->oc[j];
                    char key[ANALYZE_KEY_MAX];
                    chain_to_key(oc->offsets.offset, oc->offsets.count,
                                 key, sizeof(key));
                    str_set_add(&sets[i], key);
                }
            }
            str_set_sort(&sets[i]);
        }

        /* For each chain in file 1, check presence in all others. */
        struct pc_list *out_group = NULL;

        for (struct oc_block *b = sub[0]->chains; b; b = b->next) {
            int used = atomic_load(&b->used);
            for (int j = 0; j < used; j++) {
                struct offset_chain *oc = &b->oc[j];

                char key[ANALYZE_KEY_MAX];
                chain_to_key(oc->offsets.offset, oc->offsets.count,
                             key, sizeof(key));

                int in_all = 1;
                for (int i = 1; i < n_files; i++) {
                    if (!str_set_has(&sets[i], key)) { in_all = 0; break; }
                }
                if (!in_all) continue;

                if (!out_group) {
                    out_group = calloc(1, sizeof(*out_group));
                    if (!out_group) goto cleanup_group;
                    out_group->filename = ref->filename
                                            ? strdup(ref->filename) : NULL;
                    out_group->seg_type    = ref->seg_type;
                    out_group->seg_index   = ref->seg_index;
                    out_group->chain_count = 0;
                    out_group->chains      = NULL;
                    out_group->next        = NULL;

                    if (!result) result = result_tail = out_group;
                    else { result_tail->next = out_group;
                           result_tail = out_group; }
                }
                pc_list_add_chain(out_group, oc->offsets.offset,
                                  oc->offsets.count);
            }
        }

    cleanup_group:
        for (int i = 1; i < n_files; i++) str_set_free(&sets[i]);
    }

    return result;
}

/* tail suffix analysis (original logic preserved). */

static void analyze_tail(struct pc_list **sub, int n_sub,
                         const char **names, int n_files, FILE *out)
{
    int32_t best[ANALYZE_MAX_TAIL];
    int     best_k = 0;

    for (struct oc_block *b0 = sub[0]->chains; b0; b0 = b0->next) {
        int used0 = atomic_load(&b0->used);
        for (int j0 = 0; j0 < used0; j0++) {
            int L0 = b0->oc[j0].offsets.count;
            if (L0 <= best_k) continue;

            int kmax = L0 < ANALYZE_MAX_TAIL ? L0 : ANALYZE_MAX_TAIL;
            for (int k = kmax; k > best_k; k--) {
                int32_t cand[ANALYZE_MAX_TAIL];
                for (int q = 0; q < k; q++)
                    cand[q] = b0->oc[j0].offsets.offset[L0 - 1 - q];

                int all_have = 1;
                for (int fi = 1; fi < n_sub && all_have; fi++) {
                    int found = 0;
                    for (struct oc_block *b = sub[fi]->chains;
                         b && !found; b = b->next) {
                        int used = atomic_load(&b->used);
                        for (int j = 0; j < used; j++) {
                            int L = b->oc[j].offsets.count;
                            if (L < k) continue;
                            int match = 1;
                            for (int q = 0; q < k; q++) {
                                if (b->oc[j].offsets.offset[L - 1 - q]
                                    != cand[q]) { match = 0; break; }
                            }
                            if (match) { found = 1; break; }
                        }
                    }
                    if (!found) all_have = 0;
                }

                if (all_have) {
                    best_k = k;
                    memcpy(best, cand, k * sizeof(int32_t));
                    break;
                }
            }
        }
    }

    fprintf(out, "%scommon tail%s (present in all %d files)\n",
            C_BLUE_BOLD, C_RESET, n_files);

    if (best_k == 0) {
        fprintf(out, "  (none)\n");
        return;
    }

    fprintf(out, "  target-side first:\n    ");
    for (int q = 0; q < best_k; q++) {
        if (q > 0) fprintf(out, " -> ");
        fprintf(out, "0x%x", (unsigned)best[q]);
    }
    fprintf(out, "\n\n");

    fprintf(out, "  as scan options:\n   ");
    for (int q = 0; q < best_k; q++)
        fprintf(out, " -t 0x%x", (unsigned)best[q]);
    fprintf(out, "\n");

    (void)names;
}

/* analyze command entry */

static int cmd_analyze(int argc, char **argv)
{
    const char *in_files[ANALYZE_MAX_FILES];
    int         n_files = 0;
    const char *out_file = NULL;
    int         want_tail   = 0;
    int         want_inter  = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((!strcmp(a, "-o") || !strcmp(a, "--output")) && i + 1 < argc)
            out_file = argv[++i];
        else if (!strcmp(a, "--tail") || !strcmp(a, "--shared-tail"))
            want_tail = 1;
        else if (!strcmp(a, "--intersection") || !strcmp(a, "--intersect"))
            want_inter = 1;
        else if (a[0] == '-') {
            fprintf(stderr, "%serror:%s unknown option '%s'\n",
                    C_RED_BOLD, C_RESET, a);
            return 2;
        } else {
            if (n_files >= ANALYZE_MAX_FILES) {
                fprintf(stderr, "%serror:%s too many files\n",
                        C_RED_BOLD, C_RESET);
                return 2;
            }
            in_files[n_files++] = a;
        }
    }

    if (n_files < 2) {
        fprintf(stderr, "%serror:%s usage:\n", C_RED_BOLD, C_RESET);
        fprintf(stderr,
                "  ptrscan analyze --tail         <f1> <f2> [...] [-o OUT]\n");
        fprintf(stderr,
                "  ptrscan analyze --intersection <f1> <f2> [...] [-o OUT]\n");
        return 2;
    }

    if (!want_tail && !want_inter) want_tail = 1;

    struct pc_list **lists = calloc((size_t)n_files, sizeof(*lists));
    if (!lists) return 1;

    for (int i = 0; i < n_files; i++) {
        if (analyze_load(in_files[i], &lists[i]) != 0) {
            fprintf(stderr, "%serror:%s cannot load '%s'\n",
                    C_RED_BOLD, C_RESET, in_files[i]);
            for (int j = 0; j < i; j++) free_pc_list(lists[j]);
            free(lists);
            return 1;
        }
    }

    int rc = 0;

    /* intersection: output stable chains as pc_list. */
    if (want_inter) {
        struct pc_list *result = pc_list_intersect_all(lists, n_files);

        if (!result) {
            fprintf(stderr,
                    "%sno common chain across all %d files%s\n",
                    C_YELLOW, n_files, C_RESET);
        } else {
            int n_chains = 0;
            int n_groups = 0;
            for (struct pc_list *p = result; p; p = p->next) {
                n_groups++;
                n_chains += p->chain_count;
            }

            if (out_file) {
                FILE *fp = fopen(out_file, "wb");
                if (!fp) {
                    fprintf(stderr, "%serror:%s cannot open '%s': %s\n",
                            C_RED_BOLD, C_RESET, out_file, strerror(errno));
                    rc = 1;
                } else {
                    const char *ext = strrchr(out_file, '.');
                    int sv;
                    if (ext && strcasecmp(ext, ".pcf") == 0)
                        sv = pcf_write_pc_list_everything(fp, result);
                    else
                        sv = txt_save(fp, result);
                    fclose(fp);
                    if (sv != 0) {
                        fprintf(stderr, "%serror:%s save failed\n",
                                C_RED_BOLD, C_RESET);
                        rc = 1;
                    } else {
                        printf("%swrote:%s %s (%d chains in %d group%s)\n",
                               C_GREEN, C_RESET, out_file,
                               n_chains, n_groups,
                               n_groups == 1 ? "" : "s");
                    }
                }
            } else {
                txt_save(stdout, result);
            }
            free_pc_list(result);
        }
    }

    /* tail: suffix analysis, print to stdout. */
    if (want_tail) {
        for (struct pc_list *ref = lists[0]; ref; ref = ref->next) {
            struct pc_list *sub[ANALYZE_MAX_FILES];
            int n_sub = 1;
            sub[0] = ref;

            int all_found = 1;
            for (int i = 1; i < n_files; i++) {
                struct pc_list *g = analyze_find_group(lists[i], ref);
                if (!g) { all_found = 0; break; }
                sub[n_sub++] = g;
            }
            if (!all_found) continue;

            printf("%sGroup:%s %s .%s[%d]\n",
                   C_BLUE_BOLD, C_RESET,
                   ref->filename ? ref->filename : "(anon)",
                   ana_seg_name(ref->seg_type), ref->seg_index);
            analyze_tail(sub, n_sub, in_files, n_files, stdout);
            printf("\n");
        }
    }

    for (int i = 0; i < n_files; i++) free_pc_list(lists[i]);
    free(lists);
    return rc;
}

/* help */

static int cmd_help(int argc, char **argv)
{
    if (argc >= 2) {
        const char *c = argv[1];
        if (strcmp(c, "index") == 0) {
            printf("Build a pointer index from a running process and save to IDX\n\n");
            printf("%sUsage:%s ptrscan index <PID> [FLAGS]\n\n", C_BOLD, C_RESET);
            printf("%sFlags:%s\n", C_BOLD, C_RESET);
            printf("  %s%-24s%s%s\n", C_GREEN, "-o, --output <FILE>", C_RESET,
                   "Write IDX to FILE (optional)");
            printf("  %s%-24s%s%s\n", C_GREEN, "-j, --jobs <N>", C_RESET,
                   "Worker threads (default: number of CPUs)");
            return 0;
        }
        if (strcmp(c, "scan") == 0) {
            printf("Scan pointer chains\n\n");
            printf("%sUsage:%s\n", C_BOLD, C_RESET);
            printf("  ptrscan scan <PID> <TARGET> [FLAGS]\n");
            printf("  ptrscan scan <TARGET> -p <PID> [FLAGS]\n");
            printf("  ptrscan scan <TARGET> -i <FILE.idx> [FLAGS]\n\n");
            printf("%sFlags:%s\n", C_BOLD, C_RESET);
            printf("  %s%-24s%s%s\n", C_GREEN, "-p, --pid <PID>", C_RESET,
                   "Target PID");
            printf("  %s%-24s%s%s\n", C_GREEN, "-i, --idx <FILE>", C_RESET,
                   "Load IDX from FILE");
            printf("  %s%-24s%s%s\n", C_GREEN, "-L, --log <FILE>", C_RESET,
                   "Write full histogram to log FILE");
            printf("  %s%-24s%s%s\n", C_GREEN, "-d, --depth <N>", C_RESET,
                   "Max pointer depth (default 6)");
            printf("  %s%-24s%s%s\n", C_GREEN, "-m, --min-depth <N>", C_RESET,
                   "Min pointer depth");
            printf("  %s%-24s%s%s\n", C_GREEN, "-n, --max-chains <N>", C_RESET,
                   "Stop after N chains");
            printf("  %s%-24s%s%s\n", C_GREEN, "-j, --jobs <N>", C_RESET,
                   "Worker threads");
            printf("  %s%-24s%s%s\n", C_GREEN, "-k, --max-targets <L>", C_RESET,
                   "Per-layer target cap (default 3)");
            printf("  %s%-24s%s%s\n", C_GREEN, "-O, --offset <N>", C_RESET,
                   "Search window (default 0x1000)");
            printf("  %s%-24s%s%s\n", C_GREEN, "-t, --tail <LIST>", C_RESET,
                   "Per-layer tail offset whitelist");
            printf("  %s%-24s%s%s\n", C_GREEN, "-o, --output <FILE>", C_RESET,
                   "Write chains to FILE");
            return 0;
        }
        if (strcmp(c, "analyze") == 0) {
            printf("Analyze chains / tails across multiple scan outputs\n\n");
            printf("%sUsage:%s\n", C_BOLD, C_RESET);
            printf("  ptrscan analyze --tail         <f1> <f2> [...] [-o OUT]\n");
            printf("  ptrscan analyze --intersection <f1> <f2> [...] [-o OUT]\n\n");
            printf("%sFlags:%s\n", C_BOLD, C_RESET);
            printf("  %s%-24s%s%s\n", C_GREEN, "    --tail", C_RESET,
                   "Report longest common suffix per group");
            printf("  %s%-24s%s%s\n", C_GREEN, "    --intersection", C_RESET,
                   "Write chains present in ALL files as pc_list");
            printf("  %s%-24s%s%s\n", C_GREEN, "-o, --output <FILE>", C_RESET,
                   "Write intersection to FILE (.txt/.pcf)");
            return 0;
        }
    }
    usage();
    return 0;
}

int main(int argc, char **argv)
{
    install_sigint();
    use_color = detect_color();

    if (argc < 2) { usage(); return 1; }

    const char *cmd = argv[1];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) { usage(); return 0; }
    if (strcmp(cmd, "-V") == 0 || strcmp(cmd, "--version") == 0) {
        printf("ptrscan %s\n", VERSION);
        return 0;
    }

    int sub_argc = argc - 1;
    char **sub_argv = argv + 1;

    if (strcmp(cmd, "index")   == 0) return cmd_index(sub_argc, sub_argv);
    if (strcmp(cmd, "scan")    == 0) return cmd_scan(sub_argc, sub_argv);
    if (strcmp(cmd, "analyze") == 0) return cmd_analyze(sub_argc, sub_argv);
    if (strcmp(cmd, "help")    == 0) return cmd_help(sub_argc, sub_argv);

    fprintf(stderr, "%serror:%s unrecognized subcommand '%s'\n\n",
            C_RED_BOLD, C_RESET, cmd);
    usage();
    return 1;
}