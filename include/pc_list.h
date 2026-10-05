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

#ifndef PC_LIST_H
#define PC_LIST_H

#include <stdint.h>
#include <stdatomic.h>

#define OC_LIST_CAPACITY   0x1000
#define PC_MAX_DEPTH       64

struct offset_chain {
    struct {
        int32_t *offset;
        int count;
    } offsets;
};

struct oc_block {
    struct offset_chain oc[OC_LIST_CAPACITY];
    uint64_t oc_cost[OC_LIST_CAPACITY];
    _Atomic int used;
    struct oc_block *next;
};

struct pc_list {
    char *filename;
    uint8_t seg_type;
    uint8_t seg_index;
    int chain_count;
    struct oc_block *chains;
    struct pc_list *next;
};

void free_pc_list(struct pc_list *list);
void free_oc_block(struct oc_block *block);

struct pc_list *pc_list_copy(const struct pc_list *head);

int pc_list_compute_cost(struct pc_list *head,
                         uint32_t depth_w,
                         uint32_t offset_w,
                         uint32_t depth_offset_slope,
                         uint32_t base_offset_w);
int pc_list_topk(struct pc_list **head, int count);

int pc_list_filter_tail_offsets(struct pc_list **head,
                                struct offset_chain *filter);

/* Drop chains with depth > max_depth. Returns the number of chains
 * remaining. A max_depth <= 0 disables the filter. */
int pc_list_filter_max_depth(struct pc_list **head, int max_depth);

/* Drop chains with depth < min_depth. Returns the number of chains
 * remaining. A min_depth <= 0 disables the filter. */
int pc_list_filter_min_depth(struct pc_list **head, int min_depth);

#endif  /* PC_LIST_H */