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

#ifndef FS_PTRSCAN_H
#define FS_PTRSCAN_H

#include <stdint.h>
#include <sys/types.h>
#include <stdatomic.h>
#include <pthread.h>

#include "callback.h"
#include "ptr_index.h"
#include "pc_list.h"

#define FS_SCAN_FINISH      0
#define FS_SCAN_FAILED     -1
#define FS_SCAN_CANCELLED  -2

#define FS_HIST_MAX_LAYERS  512
#define FS_HIST_FLUSH_BATCH 2048

enum fs_phase {
    FS_SCAN_PHASE_IDLE   = 0,
    FS_SCAN_PHASE_BFS    = 1,
    FS_SCAN_PHASE_ENUM   = 2,
    FS_SCAN_PHASE_DONE   = 3,
    FS_SCAN_PHASE_FAILED = 4,
};

struct fs_perf {
    atomic_uint_least64_t start_ns;
    atomic_uint_least64_t end_ns;
    atomic_uint_least64_t bfs_start_ns;
    atomic_uint_least64_t bfs_end_ns;
    atomic_uint_least64_t enum_start_ns;
    atomic_uint_least64_t enum_end_ns;
    double                total_ms;
};

struct fs_hist_slot {
    int32_t  prev;
    int32_t  delta;
    uint64_t hits;
    uint64_t anc;
};

struct fs_hist_layer {
    struct fs_hist_slot *slots;
    int                 *index;
    int                  index_cap;
    int                  n;
    int                  cap;
    uint64_t             boundary_in;
    uint64_t             total_hits;
};

struct fs_progress {
    _Atomic int      phase;
    _Atomic uint64_t start_ms;

    _Atomic int      depth;
    _Atomic int      max_depth_reached;
    _Atomic uint64_t layer_in;
    _Atomic uint64_t layer_out;
    _Atomic uint64_t layer_hits;

    _Atomic uint64_t total_pm;
    _Atomic uint64_t total_edges;
    _Atomic uint64_t total_dup;
    _Atomic uint64_t total_anchors;
    _Atomic uint64_t total_chains;

    _Atomic int      enum_index;
    _Atomic int      enum_total;

    _Atomic int      cancelled;

    pthread_mutex_t      hist_mtx;
    struct fs_hist_layer hist_layers[FS_HIST_MAX_LAYERS];
    _Atomic int          hist_max_depth;
};

struct fs_result {
    struct pc_list *chains;
    struct fs_perf  perf;
};

struct fs_opts {
    uintptr_t target;

    int       max_depth;
    int       max_chains;
    int       min_depth;

    const int32_t *tail_flat;
    const int     *tail_starts;
    int            tail_layer_count;

    const uint64_t *max_off;
    int             max_off_len;

    const int *max_targets_per_node;
    int        max_targets_per_node_len;

    struct vma_select **anchors;
    int                 anchor_count;
};

int fs_ptrscan(const struct idx *ix,
               const struct fs_opts *opts,
               struct fs_progress *progress,
               struct fs_result **result);

void free_fs_result(struct fs_result **result);

struct fs_progress *create_fs_progress(void);
void                free_fs_progress(struct fs_progress *progress);

void fs_progress_cancel(struct fs_progress *p);

void fs_progress_hist_begin(struct fs_progress *p,
                            int depth, uint64_t boundary_in);

int  fs_progress_hist_snapshot_one(struct fs_progress *p,
                                   int layer_1based,
                                   struct fs_hist_layer *out);

void fs_progress_hist_sort(struct fs_hist_layer *L);
void fs_progress_hist_release(struct fs_hist_layer *layers, int n);

#endif /* FS_PTRSCAN_H */