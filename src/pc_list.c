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

#include "pc_list.h"
#include <stdlib.h>
#include <string.h>

static char *pc_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

void free_oc_block(struct oc_block *block) {
    while (block) {
        struct oc_block *next = block->next;
        int used = atomic_load(&block->used);
        for (int i = 0; i < used; i++) {
            free(block->oc[i].offsets.offset);
        }
        free(block);
        block = next;
    }  /* while block */
}

void free_pc_list(struct pc_list *list) {
    while (list) {
        struct pc_list *next = list->next;
        free(list->filename);
        free_oc_block(list->chains);
        free(list);
        list = next;
    }  /* while list */
}

static struct oc_block *oc_block_copy(const struct oc_block *src_block) {
    int used = atomic_load(&src_block->used);
    if (used == 0) return NULL;

    struct oc_block *new_block = calloc(1, sizeof(struct oc_block));
    if (!new_block) return NULL;

    atomic_init(&new_block->used, 0);
    new_block->next = NULL;

    for (int i = 0; i < used; i++) {
        const struct offset_chain *src = &src_block->oc[i];
        struct offset_chain *dst = &new_block->oc[i];

        dst->offsets.count = src->offsets.count;
        if (src->offsets.count > 0) {
            dst->offsets.offset = malloc(src->offsets.count * sizeof(int32_t));
            if (!dst->offsets.offset) {
                for (int j = 0; j < i; j++) free(new_block->oc[j].offsets.offset);
                free(new_block);
                return NULL;
            }
            memcpy(dst->offsets.offset, src->offsets.offset,
                   src->offsets.count * sizeof(int32_t));
        } else {
            dst->offsets.offset = NULL;
        }
        new_block->oc_cost[i] = src_block->oc_cost[i];
        atomic_store(&new_block->used, i + 1);
    }  /* for i */
    return new_block;
}  /* oc_block_copy */

struct pc_list *pc_list_copy(const struct pc_list *head) {
    if (!head) return NULL;

    struct pc_list *new_head = NULL, *new_tail = NULL;
    for (const struct pc_list *src = head; src; src = src->next) {
        struct pc_list *node = calloc(1, sizeof(struct pc_list));
        if (!node) {
            free_pc_list(new_head);
            return NULL;
        }

        node->filename = pc_strdup(src->filename);
        if (!node->filename) {
            free(node);
            free_pc_list(new_head);
            return NULL;
        }

        node->seg_type = src->seg_type;
        node->seg_index = src->seg_index;
        node->chain_count = src->chain_count;
        node->next = NULL;

        struct oc_block *src_block = src->chains;
        struct oc_block *prev = NULL;
        while (src_block) {
            struct oc_block *new_block = oc_block_copy(src_block);
            if (!new_block) {
                free(node->filename);
                free_oc_block(node->chains);
                free(node);
                free_pc_list(new_head);
                return NULL;
            }
            if (!node->chains) node->chains = new_block;
            else prev->next = new_block;
            prev = new_block;
            src_block = src_block->next;
        }  /* while src_block */

        if (!new_head) new_head = new_tail = node;
        else { new_tail->next = node; new_tail = node; }
    }  /* for src */
    return new_head;
}  /* pc_list_copy */

int pc_list_compute_cost(struct pc_list *head,
                         uint32_t depth_w,
                         uint32_t offset_w,
                         uint32_t depth_offset_slope,
                         uint32_t base_offset_w)
{
    for (struct pc_list *p = head; p; p = p->next) {
        struct oc_block *b = p->chains;
        while (b) {
            int used = atomic_load(&b->used);
            for (int i = 0; i < used; i++) {
                struct offset_chain *oc = &b->oc[i];
                uint32_t depth = (uint32_t)oc->offsets.count;
                uint64_t cost = (uint64_t)depth_w * depth;

                if (base_offset_w && oc->offsets.count > 0) {
                    int32_t base_val = oc->offsets.offset[0];
                    uint32_t abs_base = (uint32_t)(base_val >= 0 ? base_val : -base_val);
                    cost += (uint64_t)base_offset_w * abs_base;
                }  /* if base_offset_w */

                for (int j = 1; j < oc->offsets.count; j++) {
                    int32_t val = oc->offsets.offset[j];
                    uint32_t abs_val = (uint32_t)(val >= 0 ? val : -val);

                    uint32_t w = offset_w + (uint32_t)(j - 1) * depth_offset_slope;
                    cost += (uint64_t)w * abs_val;
                }  /* for j */

                b->oc_cost[i] = cost;
            }  /* for i */
            b = b->next;
        }  /* while b */
    }  /* for p */
    return 0;
}  /* pc_list_compute_cost */

struct chain_ref {
    uint64_t cost;
    struct offset_chain *chain;
    struct pc_list *group;
    struct oc_block *block;
    int index_in_block;
};

static int compare_cost(const void *a, const void *b) {
    const struct chain_ref *ca = (const struct chain_ref *)a;
    const struct chain_ref *cb = (const struct chain_ref *)b;
    return (ca->cost > cb->cost) - (ca->cost < cb->cost);
}

int pc_list_topk(struct pc_list **head, int count) {
    if (!head || !*head || count <= 0) return 0;

    int total = 0;
    for (struct pc_list *p = *head; p; p = p->next) {
        struct oc_block *b = p->chains;
        while (b) {
            total += atomic_load(&b->used);
            b = b->next;
        }  /* while b */
    }  /* for p */
    if (total == 0) return 0;
    if (count > total) count = total;

    struct chain_ref *refs = malloc(total * sizeof(struct chain_ref));
    if (!refs) return -1;

    int idx = 0;
    for (struct pc_list *p = *head; p; p = p->next) {
        struct oc_block *b = p->chains;
        while (b) {
            int used = atomic_load(&b->used);
            for (int i = 0; i < used; i++) {
                refs[idx].cost = b->oc_cost[i];
                refs[idx].chain = &b->oc[i];
                refs[idx].group = p;
                refs[idx].block = b;
                refs[idx].index_in_block = i;
                idx++;
            }  /* for i */
            b = b->next;
        }  /* while b */
    }  /* for p */

    qsort(refs, total, sizeof(struct chain_ref), compare_cost);
    struct pc_list *new_head = NULL, *new_tail = NULL;

    for (int i = 0; i < count; i++) {
        struct chain_ref *ref = &refs[i];
        struct pc_list *src_group = ref->group;
        struct oc_block *src_block = ref->block;
        int src_idx = ref->index_in_block;

        struct pc_list *new_node = NULL;
        for (struct pc_list *p = new_head; p; p = p->next) {
            if (p->filename && src_group->filename &&
                strcmp(p->filename, src_group->filename) == 0 &&
                p->seg_type == src_group->seg_type &&
                p->seg_index == src_group->seg_index) {
                new_node = p;
                break;
            }
        }  /* for p */
        if (!new_node) {
            new_node = calloc(1, sizeof(struct pc_list));
            if (!new_node) {
                free(refs);
                free_pc_list(new_head);
                return -1;
            }
            new_node->filename = pc_strdup(src_group->filename);
            if (!new_node->filename) {
                free(new_node);
                free(refs);
                free_pc_list(new_head);
                return -1;
            }
            new_node->seg_type = src_group->seg_type;
            new_node->seg_index = src_group->seg_index;
            new_node->chain_count = 0;
            new_node->chains = NULL;
            new_node->next = NULL;

            if (!new_head) new_head = new_tail = new_node;
            else { new_tail->next = new_node; new_tail = new_node; }
        }  /* if !new_node */

        struct oc_block *dest_block = new_node->chains;
        struct oc_block *last_block = NULL;
        while (dest_block) {
            if (atomic_load(&dest_block->used) < OC_LIST_CAPACITY) break;
            last_block = dest_block;
            dest_block = dest_block->next;
        }  /* while dest_block */
        if (!dest_block) {
            dest_block = calloc(1, sizeof(struct oc_block));
            if (!dest_block) {
                free(refs);
                free_pc_list(new_head);
                return -1;
            }
            atomic_init(&dest_block->used, 0);
            dest_block->next = NULL;
            if (last_block) last_block->next = dest_block;
            else new_node->chains = dest_block;
        }  /* if !dest_block */

        int slot = atomic_load(&dest_block->used);
        dest_block->oc[slot] = src_block->oc[src_idx];
        dest_block->oc_cost[slot] = src_block->oc_cost[src_idx];
        atomic_store(&dest_block->used, slot + 1);
        new_node->chain_count++;

        src_block->oc[src_idx].offsets.offset = NULL;
        src_block->oc[src_idx].offsets.count = 0;
    }  /* for i */

    free(refs);
    free_pc_list(*head);

    *head = new_head;
    return count;
}  /* pc_list_topk */

int pc_list_filter_tail_offsets(struct pc_list **head, struct offset_chain *filter) {
    if (!head || !*head) return 0;
    if (!filter || filter->offsets.count <= 0) return -1;

    int total_kept = 0;
    struct pc_list **node_ptr = head;

    while (*node_ptr) {
        struct pc_list *node = *node_ptr;
        struct oc_block **block_ptr = &node->chains;
        int node_kept = 0;

        while (*block_ptr) {
            struct oc_block *block = *block_ptr;
            int used = atomic_load(&block->used);
            int write_idx = 0;

            for (int i = 0; i < used; i++) {
                struct offset_chain *oc = &block->oc[i];
                int match = 0;

                if (oc->offsets.count >= filter->offsets.count) {
                    match = 1;
                    int tail_start = oc->offsets.count - filter->offsets.count;
                    for (int j = 0; j < filter->offsets.count; j++) {
                        if (oc->offsets.offset[tail_start + j] != filter->offsets.offset[j]) {
                            match = 0;
                            break;
                        }
                    }  /* for j */
                }  /* if count >= */

                if (match) {
                    if (write_idx != i) {
                        block->oc[write_idx] = block->oc[i];
                        block->oc_cost[write_idx] = block->oc_cost[i];
                    }
                    write_idx++;
                } else {
                    free(oc->offsets.offset);
                    oc->offsets.offset = NULL;
                    oc->offsets.count = 0;
                }  /* if match */
            }  /* for i */

            atomic_store(&block->used, write_idx);
            node_kept += write_idx;

            if (write_idx == 0) {
                struct oc_block *next = block->next;
                free(block);
                *block_ptr = next;
            } else {
                block_ptr = &block->next;
            }  /* if write_idx */
        }  /* while *block_ptr */

        node->chain_count = node_kept;
        total_kept += node_kept;

        if (node_kept == 0) {
            struct pc_list *next = node->next;
            free(node->filename);
            free(node);
            *node_ptr = next;
        } else {
            node_ptr = &node->next;
        }  /* if node_kept */
    }  /* while *node_ptr */

    return total_kept;
}  /* pc_list_filter_tail_offsets */

static int pc_list_filter_depth(struct pc_list **head,
                                int min_depth, int max_depth)
{
    if (!head || !*head)
        return 0;

    /* No constraint on either side: just count and return. */
    if (min_depth <= 0 && max_depth <= 0) {
        int total = 0;
        for (struct pc_list *p = *head; p; p = p->next)
            total += p->chain_count;
        return total;
    }

    int total_kept = 0;
    struct pc_list *prev = NULL;
    struct pc_list *curr = *head;

    while (curr) {
        int node_kept = 0;
        struct oc_block *bprev = NULL;
        struct oc_block *bcurr = curr->chains;

        while (bcurr) {
            int used = atomic_load(&bcurr->used);
            int new_used = 0;

            for (int i = 0; i < used; i++) {
                struct offset_chain *oc = &bcurr->oc[i];
                int depth = oc->offsets.count;

                int keep = 1;
                if (min_depth > 0 && depth < min_depth) keep = 0;
                if (max_depth > 0 && depth > max_depth) keep = 0;

                if (keep) {
                    /* Compact in place: keep survivors at the front. */
                    if (new_used != i) {
                        bcurr->oc[new_used]      = *oc;
                        bcurr->oc_cost[new_used] = bcurr->oc_cost[i];
                    }
                    new_used++;
                } else {
                    free(oc->offsets.offset);
                    oc->offsets.offset = NULL;
                    oc->offsets.count  = 0;
                }
            }

            atomic_store(&bcurr->used, new_used);

            if (new_used == 0) {
                /* Block is empty: unlink and free it.
                 * All chains inside have already been released above. */
                struct oc_block *to_free = bcurr;
                if (bprev)
                    bprev->next = bcurr->next;
                else
                    curr->chains = bcurr->next;
                bcurr = bcurr->next;
                free(to_free);
            } else {
                node_kept += new_used;
                bprev = bcurr;
                bcurr = bcurr->next;
            }
        }

        curr->chain_count = node_kept;
        total_kept += node_kept;

        if (node_kept == 0) {
            /* Whole pc_list node is empty: unlink and free it. */
            struct pc_list *to_free = curr;
            if (prev)
                prev->next = curr->next;
            else
                *head = curr->next;
            curr = curr->next;
            free(to_free->filename);
            free(to_free);          /* chains is already NULL */
        } else {
            prev = curr;
            curr = curr->next;
        }
    }

    return total_kept;
}

int pc_list_filter_max_depth(struct pc_list **head, int max_depth)
{
    return pc_list_filter_depth(head, 0, max_depth);
}

int pc_list_filter_min_depth(struct pc_list **head, int min_depth)
{
    return pc_list_filter_depth(head, min_depth, 0);
}