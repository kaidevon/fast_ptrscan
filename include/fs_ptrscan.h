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

#ifndef FS_PTRSCAN_H
#define FS_PTRSCAN_H

#include <stdint.h>
#include <sys/types.h>
#include <stdatomic.h>
#include <pthread.h>

#include "callback.h"
#include "ptr_index.h"
#include "pc_list.h"

struct fs_anchor_info {
    uint32_t    seg_type;
    uint32_t    seg_index;
    uint64_t    mod_start;
    const char *module;
};

enum fs_scan_phase {
    FS_SCAN_PHASE_IDLE   = 0,
    FS_SCAN_PHASE_BFS    = 1,
    FS_SCAN_PHASE_ENUM   = 2,
    FS_SCAN_PHASE_DONE   = 3,
    FS_SCAN_PHASE_FAILED = 4,
};

struct fs_scan_progress_info {
    enum fs_scan_phase phase;

    /* Layer information */
    int      depth;          /* Current layer number, 1-based */
    uint64_t layer_in;       /* Number of entry nodes in this layer */
    uint64_t layer_out;      /* Number of edges expanded in this layer (= number of nodes in the next layer) */
    uint64_t layer_hits;     /* Number of pointer hits in this layer (after pruning) */
    uint64_t layer_bytes;    /* Number of bytes read in this layer (not tracked in this version, always 0) */

    /* cumulative */
    uint64_t total_pm;       /* parent_map nodes */
    uint64_t total_edges;    /* newly added edges */
    uint64_t total_dup;      /* duplicate edges */
    uint64_t total_anchors;  /* anchors found */
    uint64_t total_chains;   /* chains produced */

    int      enum_index;     /* 1-based*/
    int      enum_total;     /* Total number of anchors */
    uint64_t enum_paths;     /* Number of paths newly added for this anchor */
    uint64_t enum_chains;    /* Number of chains newly added for this anchor */

    double   elapsed_ms;
};

struct fs_scan_perf {
    double   bfs_total_ms;    /* BFS wall-clock total */
    double   bfs_idx_ms;      /* idx_scan query total */
    double   bfs_proc_ms;     /* process_hit body total */
    double   bfs_queue_ms;    /* queue push/pop (always 0) */
    double   bfs_dedup_ms;    /* pm_put internal dedup total */
    double   bfs_alloc_ms;    /* vis/av/anchor insert total */

    uint64_t bfs_depth_max;
    uint64_t bfs_nodes;       /* entry nodes visited */
    uint64_t bfs_edges;       /* new edges */
    uint64_t bfs_dup;         /* duplicate edges */
    uint64_t bfs_bytes_read;  /* remote bytes read (always 0) */

    double   enum_total_ms;
    uint64_t enum_anchors;
    uint64_t enum_paths;
    uint64_t enum_chains;

    double   total_ms;
    int      cancelled;
};

struct fs_scan_progress {
    pthread_mutex_t mtx;
    struct fs_scan_progress_info info;
    struct fs_scan_perf          perf;

    _Atomic int cancelled;
    _Atomic int finished;
};

struct fs_scan_opts {
    uintptr_t target;

    int       max_depth;
    int       max_chains;
    int       min_depth;

    const int32_t *tail_flat;
    const int     *tail_starts;
    int            tail_layer_count;

    const uint64_t* max_off;
    int        max_off_len;

    const int *max_targets_per_node;
    int        max_targets_per_node_len;

    struct vma_select **anchors;
    int                 anchor_count;

    _Atomic int *cancel;
    struct fs_scan_progress *progress;
};

struct fs_scan_opts *fs_scan_opts_create(void);
void fs_scan_opts_free(struct fs_scan_opts *opts);

int fs_scan_opts_set_max_off(struct fs_scan_opts *o,
                             const uint64_t *v, int n);
int fs_scan_opts_set_max_targets_per_node(struct fs_scan_opts *o,
                                          const int *v, int n);
int fs_scan_opts_set_anchors(struct fs_scan_opts *o,
                             struct vma_select * const *v, int n);
int fs_scan_opts_set_tail_layers(struct fs_scan_opts *o,
                                 const int32_t *flat, int flat_n,
                                 const int *starts, int layer_count);

struct pc_list *fs_ptrscan(const struct idx *ix,
                           const struct fs_scan_opts *opts);

struct fs_scan_progress *fs_scan_progress_create(void);
void                     fs_scan_progress_free(struct fs_scan_progress *p);

void fs_scan_progress_get(const struct fs_scan_progress *p,
                          struct fs_scan_progress_info *out);
void fs_scan_perf_get(const struct fs_scan_progress *p,
                      struct fs_scan_perf *out);

void fs_scan_progress_cancel(struct fs_scan_progress *p);
int  fs_scan_progress_cancelled(const struct fs_scan_progress *p);

#endif /* FS_PTRSCAN_H */