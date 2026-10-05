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

#ifndef PTR_INDEX_H
#define PTR_INDEX_H

#include <stdint.h>
#include <sys/types.h>
#include "vma/vm_area.h"
#include "callback.h"

enum idx_phase {
    FS_PTRSCAN_PHASE_IDLE   = 0,
    FS_PTRSCAN_PHASE_SCAN   = 1,
    FS_PTRSCAN_PHASE_MERGE  = 2,
    FS_PTRSCAN_PHASE_SORT   = 3,
    FS_PTRSCAN_PHASE_DONE   = 4,
    FS_PTRSCAN_PHASE_FAILED = 5,
};

struct idx_progress {
    _Atomic int      phase;
    _Atomic int      cancel;
    _Atomic int      pause;

    _Atomic uint64_t total_bytes;
    _Atomic uint64_t scanned_bytes;
    _Atomic uint64_t total_entries;

    _Atomic int      sort_pass;
    _Atomic int      sort_total;

    pthread_mutex_t  mtx;
    pthread_cond_t   cond;
};

struct idx_progress_info {
    int      phase;           /* enum idx_phase */
    int      cancelled;       /* 0/1 */
    int      paused;          /* 0/1 */

    uint64_t total_bytes;
    uint64_t scanned_bytes;

    uint64_t total_entries;
    int      sort_pass;
    int      sort_total;
};

struct seg {
    uint64_t start;
    uint64_t end;
    uint32_t type;
    uint32_t index;
    uint64_t mod_start;
    char    *pathname;
};

struct ent {
    uint64_t v;
    uint64_t meta;
};

struct idx_hit {
    uint32_t  target_index;
    uintptr_t source;
    uintptr_t value;
};

struct idx_perf {
    uint64_t seg_copy_ns;
    uint64_t seg_sort_ns;
    uint64_t scan_ns;
    uint64_t merge_ns;
    uint64_t radix_sort_ns;
    uint64_t total_ns;
    uint64_t bytes_scanned;
    uint64_t entries;
    int      jobs_used;
};

struct idx {
    struct ent *e;
    uint64_t    n;
    uint64_t    cap;

    struct seg *segs;
    uint32_t    nseg;

    uint64_t    vmin;
    uint64_t    vmax;

    struct idx_perf perf;
};

struct idx *idx_build(pid_t pid, const struct vm_area *vma,
                      fs_ptrscan_process_reader_t reader, void *userdata,
                      int jobs, struct idx_progress *progress);
void idx_free(struct idx *ix);

int64_t idx_scan(const struct idx *ix,
                 const uint64_t *targets, uint32_t n,
                 uint64_t delta,
                 struct idx_hit **out);

uint64_t idx_count(const struct idx *ix);
uint32_t idx_seg_count(const struct idx *ix);
int idx_find_seg(const struct idx *ix, uintptr_t value);

/* progress */
struct idx_progress *idx_progress_create(void);
void idx_progress_free(struct idx_progress *p);

void idx_progress_pause(struct idx_progress *p);
void idx_progress_resume(struct idx_progress *p);
void idx_progress_cancel(struct idx_progress *p);

void idx_progress_get(const struct idx_progress *p,
                      struct idx_progress_info *out);

#endif /* PTR_INDEX_H */