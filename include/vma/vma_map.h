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

#ifndef VMA_MAP_H
#define VMA_MAP_H

#include "vm_area.h"
#include <stdint.h>

typedef struct vma_map vma_map_t;

vma_map_t *vma_map_create(struct vm_area *vmas);

void vma_map_destroy(vma_map_t *map);

struct vm_area *vma_map_find(vma_map_t *map, uintptr_t addr);

#endif /* VMA_MAP_H */