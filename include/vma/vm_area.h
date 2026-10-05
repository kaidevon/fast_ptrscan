/*
 * Copyright (C) 2026 kaidev
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; version 3 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef VM_AREA_H
#define VM_AREA_H

#include <sys/types.h>
#include <stdint.h>

#define VM_LIST_CAPACITY   0x2000

enum vma_type {
    VMA_TYPE_UNKNOWN = 0,
    VMA_TYPE_TEXT,
    VMA_TYPE_RODATA,
    VMA_TYPE_DATA,
    VMA_TYPE_BSS,
};

struct vm_area {
    uint32_t    seg_type;
    uint32_t    seg_index;

    uint64_t    start;
    uint64_t    end;
    char        perm[5];
    off_t       offset;
    dev_t       dev;
    ino_t       inode;
    char        *pathname;
    struct vm_area *module;

    struct vm_area *next;
};

struct vm_list {
    uint64_t addr[VM_LIST_CAPACITY];
    _Atomic int used;
    struct vm_list *next;
};

int parse_maps(pid_t pid, struct vm_area** vm_area);
void free_vm_area(struct vm_area *head);

struct vm_area* vm_area_copy(const struct vm_area *src);

/* vma copy to vm list */
struct vm_list* vma_copy2_vm(const struct vm_area *src);

/* Analyze VMA list with libelf, filling type and module fields */
int vma_elf(struct vm_area *head);

#endif  /* VM_AREA_H */