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

#include "pcf.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include <limits.h>

static inline uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t read_u64_le(const uint8_t *p) {
    return (uint64_t)p[0] |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static inline void write_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline void write_u64_le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)((v >> (i * 8)) & 0xFF);
}

static void free_super_entries(struct pcf_super_entry *head) {
    while (head) {
        struct pcf_super_entry *next = head->next;
        free(head->info.pathname);
        free(head);
        head = next;
    }
}

static int pcf_write_file_header(FILE *fp, uint64_t group_count,
                                 uint64_t super_offset, uint32_t super_size) {
    uint8_t buf[36];
    write_u32_le(buf + 0, PCF_MAGIC);
    write_u16_le(buf + 4, PCF_VERSION);
    write_u16_le(buf + 6, PCF_FLAG_LITTLE_ENDIAN);
    write_u64_le(buf + 8, group_count);
    write_u64_le(buf + 16, super_offset);
    write_u32_le(buf + 24, super_size);
    write_u32_le(buf + 28, 0);

    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, buf, 32);
    write_u32_le(buf + 32, (uint32_t)crc);

    if (fwrite(buf, 1, 36, fp) != 36)
        return PCF_ERR_IO;
    return PCF_OK;
}

static int pcf_read_file_header(FILE *fp, struct pcf_file_header *header) {
    if (fseek(fp, 0, SEEK_SET) != 0)
        return PCF_ERR_IO;

    uint8_t buf[36];
    if (fread(buf, 1, 36, fp) != 36)
        return PCF_ERR_IO;

    header->magic = read_u32_le(buf + 0);
    header->version = read_u16_le(buf + 4);
    header->flags = read_u16_le(buf + 6);
    header->group_count = read_u64_le(buf + 8);
    header->super_offset = read_u64_le(buf + 16);
    header->super_size = read_u32_le(buf + 24);
    header->reserved = read_u32_le(buf + 28);
    header->crc32 = read_u32_le(buf + 32);

    if (header->magic != PCF_MAGIC)
        return PCF_ERR_BAD_MAGIC;
    if (header->version != PCF_VERSION)
        return PCF_ERR_UNSUPPORTED_VERSION;

    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, buf, 32);
    if ((uint32_t)crc != header->crc32)
        return PCF_ERR_CORRUPT;

    return PCF_OK;
}

static int pcf_write_group(FILE *fp, const char *module_path,
                           uint8_t seg_type, uint8_t seg_index,
                           const struct oc_block *chains,
                           uint64_t *group_offset_out, uint32_t *group_size_out) {
    size_t path_len = strlen(module_path);
    if (path_len > UINT16_MAX)
        return PCF_ERR_INVALID_ARG;

    uint32_t total_chains = 0;
    const struct oc_block *block = chains;
    while (block) {
        total_chains += atomic_load(&block->used);
        block = block->next;
    }

    uint8_t prefix[8];
    write_u32_le(prefix + 0, total_chains);
    prefix[4] = seg_type;
    prefix[5] = seg_index;
    write_u16_le(prefix + 6, (uint16_t)path_len);

    long group_start = ftell(fp);
    if (group_start < 0) return PCF_ERR_IO;

    if (fwrite(prefix, 1, 8, fp) != 8)
        return PCF_ERR_IO;

    if (fwrite(module_path, 1, path_len, fp) != path_len)
        return PCF_ERR_IO;

    block = chains;
    while (block) {
        int used = atomic_load(&block->used);
        for (int i = 0; i < used; i++) {
            const struct offset_chain *oc = &block->oc[i];
            uint8_t chain_header[4];
            write_u32_le(chain_header, (uint32_t)oc->offsets.count);
            if (fwrite(chain_header, 1, 4, fp) != 4)
                return PCF_ERR_IO;

            for (int j = 0; j < oc->offsets.count; j++) {
                uint32_t off = (uint32_t)oc->offsets.offset[j];
                uint8_t off_bytes[4];
                write_u32_le(off_bytes, off);
                if (fwrite(off_bytes, 1, 4, fp) != 4)
                    return PCF_ERR_IO;
            }
        }
        block = block->next;
    }

    long current_pos = ftell(fp);
    if (current_pos < 0) return PCF_ERR_IO;

    if (group_offset_out) *group_offset_out = (uint64_t)group_start;
    if (group_size_out) *group_size_out = (uint32_t)(current_pos - group_start);
    return PCF_OK;
}

static int pcf_read_group_at(FILE *fp, uint64_t group_offset,
                             char *module_path, size_t path_buf_size,
                             uint8_t *seg_type, uint8_t *seg_index,
                             struct oc_block **chains_out) {
    if (fseek(fp, group_offset, SEEK_SET) != 0)
        return PCF_ERR_IO;

    uint8_t header[8];
    if (fread(header, 1, 8, fp) != 8)
        return PCF_ERR_IO;

    uint32_t chain_count = read_u32_le(header + 0);
    uint8_t st = header[4];
    uint8_t si = header[5];
    uint16_t path_len = read_u16_le(header + 6);

    if (module_path) {
        if (path_buf_size < (size_t)path_len + 1)
            return PCF_ERR_INVALID_ARG;
        if (path_len > 0) {
            if (fread(module_path, 1, path_len, fp) != path_len)
                return PCF_ERR_IO;
        }
        module_path[path_len] = '\0';
    } else {
        if (fseek(fp, path_len, SEEK_CUR) != 0)
            return PCF_ERR_IO;
    }

    struct oc_block *head = NULL, *tail = NULL;
    uint32_t chains_read = 0;

    while (chains_read < chain_count) {
        struct oc_block *block = calloc(1, sizeof(struct oc_block));
        if (!block) {
            free_oc_block(head);
            return PCF_ERR_NOMEM;
        }
        atomic_init(&block->used, 0);
        block->next = NULL;

        int slot = 0;
        int block_has_error = 0;
        while (slot < OC_LIST_CAPACITY && chains_read < chain_count) {
            uint8_t chain_header[4];
            if (fread(chain_header, 1, 4, fp) != 4) {
                block_has_error = 1;
                break;
            }

            uint32_t offset_count = read_u32_le(chain_header);
            if (offset_count > PC_MAX_DEPTH) {
                block_has_error = 1;
                break;
            }

            struct offset_chain *oc = &block->oc[slot];
            oc->offsets.count = (int)offset_count;

            if (offset_count > 0) {
                oc->offsets.offset = malloc(offset_count * sizeof(int32_t));
                if (!oc->offsets.offset) {
                    block_has_error = 1;
                    break;
                }
                for (uint32_t j = 0; j < offset_count; j++) {
                    uint8_t off_bytes[4];
                    if (fread(off_bytes, 1, 4, fp) != 4) {
                        free(oc->offsets.offset);
                        block_has_error = 1;
                        break;
                    }
                    oc->offsets.offset[j] = (int32_t)read_u32_le(off_bytes);
                }
                if (block_has_error) break;
            } else {
                oc->offsets.offset = NULL;
            }

            atomic_store(&block->used, slot + 1);
            slot++;
            chains_read++;
        }

        if (block_has_error) {
            for (int i = 0; i < slot; i++) {
                free(block->oc[i].offsets.offset);
            }
            free(block);
            free_oc_block(head);
            return PCF_ERR_IO;
        }

        if (slot > 0) {
            if (!head) head = tail = block;
            else { tail->next = block; tail = block; }
        } else {
            free(block);
        }
    }

    if (seg_type) *seg_type = st;
    if (seg_index) *seg_index = si;
    if (chains_out) *chains_out = head;
    return PCF_OK;
}

static int pcf_calc_global_crc(FILE *fp, long end_offset, uint32_t *crc_out) {
    long current = ftell(fp);
    if (current < 0) return PCF_ERR_IO;
    if (fseek(fp, 0, SEEK_SET) != 0) return PCF_ERR_IO;

    uLong crc = crc32(0L, Z_NULL, 0);
    uint8_t buf[4096];
    long remaining = end_offset;
    while (remaining > 0) {
        size_t to_read = remaining < (long)sizeof(buf) ? (size_t)remaining : sizeof(buf);
        if (fread(buf, 1, to_read, fp) != to_read)
            return PCF_ERR_IO;
        crc = crc32(crc, buf, (uInt)to_read);
        remaining -= (long)to_read;
    }

    *crc_out = (uint32_t)crc;
    if (fseek(fp, current, SEEK_SET) != 0)
        return PCF_ERR_IO;
    return PCF_OK;
}

/*
 * Write entire pc_list to file with header, super block, groups, and CRC.
 */
int pcf_write_pc_list_everything(FILE *fp, const struct pc_list *pc_list) {
    if (!fp || !pc_list) return PCF_ERR_INVALID_ARG;

    uint64_t group_count = 0;
    const struct pc_list *g = pc_list;
    while (g) { group_count++; g = g->next; }

    if (pcf_write_file_header(fp, group_count, 0, 0) != PCF_OK)
        return PCF_ERR_IO;

    long super_start = ftell(fp);
    if (super_start < 0) return PCF_ERR_IO;

    uint32_t super_size = offsetof(struct pcf_super_block, entries) +
                      group_count * sizeof(struct pcf_super_entry_raw);

    uint8_t super_magic[4];
    write_u32_le(super_magic, PCF_SUPER_MAGIC);
    if (fwrite(super_magic, 1, 4, fp) != 4) return PCF_ERR_IO;

    uint8_t count_bytes[4];
    write_u32_le(count_bytes, (uint32_t)group_count);
    if (fwrite(count_bytes, 1, 4, fp) != 4) return PCF_ERR_IO;

    long entries_start = ftell(fp);
    for (uint64_t i = 0; i < group_count; i++) {
        uint8_t zero[20] = {0};
        if (fwrite(zero, 1, 20, fp) != 20) return PCF_ERR_IO;
    }

    struct pcf_super_entry_raw *entries = malloc(group_count * sizeof(struct pcf_super_entry_raw));
    if (!entries) return PCF_ERR_NOMEM;

    g = pc_list;
    uint64_t idx = 0;
    while (g) {
        uint64_t offset;
        uint32_t size;
        int ret = pcf_write_group(fp, g->filename, g->seg_type, g->seg_index,
                                  g->chains, &offset, &size);
        if (ret != PCF_OK) {
            free(entries);
            return ret;
        }
        entries[idx].group_offset = offset;
        entries[idx].group_size = size;
        entries[idx].chain_count = g->chain_count;
        entries[idx].seg_type = g->seg_type;
        entries[idx].seg_index = g->seg_index;
        entries[idx].path_len = (uint16_t)strlen(g->filename);
        idx++;
        g = g->next;
    }

    long data_end = ftell(fp);
    if (data_end < 0) { free(entries); return PCF_ERR_IO; }

    if (fseek(fp, entries_start, SEEK_SET) != 0) { free(entries); return PCF_ERR_IO; }
    for (uint64_t i = 0; i < group_count; i++) {
        uint8_t buf20[20];
        write_u64_le(buf20 + 0, entries[i].group_offset);
        write_u32_le(buf20 + 8, entries[i].group_size);
        write_u32_le(buf20 + 12, entries[i].chain_count);
        buf20[16] = entries[i].seg_type;
        buf20[17] = entries[i].seg_index;
        write_u16_le(buf20 + 18, entries[i].path_len);
        if (fwrite(buf20, 1, 20, fp) != 20) {
            free(entries);
            return PCF_ERR_IO;
        }
    }
    free(entries);

    if (fseek(fp, 16, SEEK_SET) != 0) return PCF_ERR_IO;
    uint8_t off_bytes[8];
    write_u64_le(off_bytes, (uint64_t)super_start);
    fwrite(off_bytes, 1, 8, fp);
    uint8_t size_bytes[4];
    write_u32_le(size_bytes, super_size);
    fwrite(size_bytes, 1, 4, fp);

    if (fseek(fp, 0, SEEK_SET) != 0) return PCF_ERR_IO;
    uint8_t header_buf[36];
    if (fread(header_buf, 1, 36, fp) != 36) return PCF_ERR_IO;
    uLong header_crc = crc32(0L, Z_NULL, 0);
    header_crc = crc32(header_crc, header_buf, 32);
    write_u32_le(header_buf + 32, (uint32_t)header_crc);
    if (fseek(fp, 32, SEEK_SET) != 0) return PCF_ERR_IO;
    if (fwrite(header_buf + 32, 1, 4, fp) != 4) return PCF_ERR_IO;

    if (fseek(fp, data_end, SEEK_SET) != 0) return PCF_ERR_IO;

    uint32_t global_crc;
    if (pcf_calc_global_crc(fp, data_end, &global_crc) != PCF_OK)
        return PCF_ERR_IO;

    uint8_t crc_bytes[4];
    write_u32_le(crc_bytes, global_crc);
    if (fwrite(crc_bytes, 1, 4, fp) != 4) return PCF_ERR_IO;

    fseek(fp, 0, SEEK_END);
    return PCF_OK;
}

int pcf_read_super_entries(FILE *fp, struct pcf_super_entry **entries_out) {
    if (!fp || !entries_out) return PCF_ERR_INVALID_ARG;
    *entries_out = NULL;

    struct pcf_file_header header;
    int ret = pcf_read_file_header(fp, &header);
    if (ret != PCF_OK) return ret;

    if (header.super_offset == 0 || header.super_size == 0)
        return PCF_ERR_INVALID_ARG;

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    if (file_size < 0 || (uint64_t)file_size < header.super_offset + header.super_size)
        return PCF_ERR_CORRUPT;

    if (fseek(fp, header.super_offset, SEEK_SET) != 0)
        return PCF_ERR_IO;

    uint8_t super_magic[4];
    if (fread(super_magic, 1, 4, fp) != 4) return PCF_ERR_IO;
    if (read_u32_le(super_magic) != PCF_SUPER_MAGIC) return PCF_ERR_BAD_MAGIC;

    uint8_t count_bytes[4];
    if (fread(count_bytes, 1, 4, fp) != 4) return PCF_ERR_IO;
    uint32_t entry_count = read_u32_le(count_bytes);

    if (entry_count != header.group_count) return PCF_ERR_CORRUPT;

    struct pcf_super_entry *head = NULL, *tail = NULL;
    for (uint32_t i = 0; i < entry_count; i++) {
        uint8_t buf[20];
        if (fread(buf, 1, 20, fp) != 20) {
            free_super_entries(head);
            return PCF_ERR_IO;
        }

        struct pcf_super_entry *entry = calloc(1, sizeof(struct pcf_super_entry));
        if (!entry) {
            free_super_entries(head);
            return PCF_ERR_NOMEM;
        }

        entry->group_offset      = read_u64_le(buf + 0);
        entry->info.group_size   = read_u32_le(buf + 8);
        entry->info.chain_count  = read_u32_le(buf + 12);
        entry->info.seg_type     = buf[16];
        entry->info.seg_index    = buf[17];
        entry->info.pathname     = NULL;
        entry->next              = NULL;

        uint16_t path_len = read_u16_le(buf + 18);

        if (!head) head = tail = entry;
        else { tail->next = entry; tail = entry; }

        long saved_pos = ftell(fp);
        if (saved_pos < 0) {
            free_super_entries(head);
            return PCF_ERR_IO;
        }

        if (entry->group_offset > (uint64_t)LONG_MAX) {
            free_super_entries(head);
            return PCF_ERR_CORRUPT;
        }

        if (fseek(fp, (long)entry->group_offset, SEEK_SET) != 0) {
            free_super_entries(head);
            return PCF_ERR_IO;
        }

        uint8_t grp_header[8];
        if (fread(grp_header, 1, 8, fp) != 8) {
            free_super_entries(head);
            return PCF_ERR_IO;
        }

        uint16_t grp_path_len = read_u16_le(grp_header + 6);
        if (grp_path_len != path_len) {
            free_super_entries(head);
            return PCF_ERR_CORRUPT;
        }

        entry->info.pathname = malloc((size_t)path_len + 1);
        if (!entry->info.pathname) {
            free_super_entries(head);
            return PCF_ERR_NOMEM;
        }

        if (path_len > 0) {
            if (fread(entry->info.pathname, 1, path_len, fp) != path_len) {
                free_super_entries(head);
                return PCF_ERR_IO;
            }
        }
        entry->info.pathname[path_len] = '\0';

        if (fseek(fp, saved_pos, SEEK_SET) != 0) {
            free_super_entries(head);
            return PCF_ERR_IO;
        }
    }

    *entries_out = head;
    return PCF_OK;
}

void pcf_free_super_entries(struct pcf_super_entry *entries) {
    while (entries) {
        struct pcf_super_entry *next = entries->next;
        free(entries->info.pathname);
        free(entries);
        entries = next;
    }
}

int pcf_read_pc_list_everything(FILE *fp, struct pc_list **pc_list_out,
                                const struct pcf_super_entry *entries) {
    if (!fp || !pc_list_out) return PCF_ERR_INVALID_ARG;
    *pc_list_out = NULL;

    struct pc_list *head = NULL, *tail = NULL;

    struct pcf_super_entry *all_entries = NULL;
    const struct pcf_super_entry *cur_entries = entries;
    if (!cur_entries) {
        int ret = pcf_read_super_entries(fp, &all_entries);
        if (ret != PCF_OK) return ret;
        cur_entries = all_entries;
    }

    const struct pcf_super_entry *e = cur_entries;
    while (e) {
        char *module_path = NULL;
        uint8_t seg_type, seg_index;
        struct oc_block *chains = NULL;

        if (fseek(fp, e->group_offset, SEEK_SET) != 0) {
            free_pc_list(head);
            if (all_entries) free_super_entries(all_entries);
            return PCF_ERR_IO;
        }
        uint8_t grp_header[8];
        if (fread(grp_header, 1, 8, fp) != 8) {
            free_pc_list(head);
            if (all_entries) free_super_entries(all_entries);
            return PCF_ERR_IO;
        }
        uint16_t path_len = read_u16_le(grp_header + 6);

        module_path = malloc(path_len + 1);
        if (!module_path) {
            free_pc_list(head);
            if (all_entries) free_super_entries(all_entries);
            return PCF_ERR_NOMEM;
        }

        int ret = pcf_read_group_at(fp, e->group_offset, module_path, path_len + 1,
                                    &seg_type, &seg_index, &chains);
        if (ret != PCF_OK) {
            free(module_path);
            free_pc_list(head);
            if (all_entries) free_super_entries(all_entries);
            return ret;
        }

        struct pc_list *node = calloc(1, sizeof(struct pc_list));
        if (!node) {
            free(module_path);
            free_oc_block(chains);
            free_pc_list(head);
            if (all_entries) free_super_entries(all_entries);
            return PCF_ERR_NOMEM;
        }

        node->filename = module_path;
        node->seg_type = seg_type;
        node->seg_index = seg_index;
        node->chains = chains;

        int total_chains = 0;
        struct oc_block *b = chains;
        while (b) {
            total_chains += atomic_load(&b->used);
            b = b->next;
        }
        node->chain_count = total_chains;
        node->next = NULL;

        if (!head) head = tail = node;
        else { tail->next = node; tail = node; }

        e = e->next;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    if (file_size < 0) {
        free_pc_list(head);
        if (all_entries) free_super_entries(all_entries);
        return PCF_ERR_IO;
    }
    long data_end = file_size - 4;
    if (data_end < 0) {
        free_pc_list(head);
        if (all_entries) free_super_entries(all_entries);
        return PCF_ERR_CORRUPT;
    }

    uint32_t calc_crc;
    if (pcf_calc_global_crc(fp, data_end, &calc_crc) != PCF_OK) {
        free_pc_list(head);
        if (all_entries) free_super_entries(all_entries);
        return PCF_ERR_IO;
    }

    fseek(fp, data_end, SEEK_SET);
    uint8_t crc_bytes[4];
    if (fread(crc_bytes, 1, 4, fp) != 4) {
        free_pc_list(head);
        if (all_entries) free_super_entries(all_entries);
        return PCF_ERR_IO;
    }
    uint32_t stored_crc = read_u32_le(crc_bytes);

    if (calc_crc != stored_crc) {
        free_pc_list(head);
        if (all_entries) free_super_entries(all_entries);
        return PCF_ERR_CORRUPT;
    }

    if (all_entries) free_super_entries(all_entries);

    *pc_list_out = head;
    return PCF_OK;
}

int pcf_super_entries_filter(struct pcf_super_entry **entries,
                             const char *module_path,
                             int seg_type, int seg_index) {
    if (!entries || !*entries) return 0;

    struct pcf_super_entry *prev = NULL;
    struct pcf_super_entry *curr = *entries;
    int remaining = 0;

    while (curr) {
        int keep = 1;
        if (module_path && (!curr->info.pathname || strcmp(curr->info.pathname, module_path) != 0))
            keep = 0;
        if (seg_type != -1 && curr->info.seg_type != seg_type)
            keep = 0;
        if (seg_index != -1 && curr->info.seg_index != seg_index)
            keep = 0;

        if (!keep) {
            if (prev) {
                prev->next = curr->next;
                free(curr->info.pathname);
                free(curr);
                curr = prev->next;
            } else {
                *entries = curr->next;
                free(curr->info.pathname);
                free(curr);
                curr = *entries;
            }
        } else {
            remaining++;
            prev = curr;
            curr = curr->next;
        }
    }

    return remaining;
}