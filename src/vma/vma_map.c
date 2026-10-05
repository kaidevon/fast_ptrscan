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

#include "vma/vma_map.h"
#include <stdlib.h>
#include <stddef.h>

struct vma_map {
    struct vm_area **vmas;   /* sorted array of VMA pointers */
    int             count;   /* number of VMAs */
};

static int cmp_vma_ptr(const void *a, const void *b)
{
    const struct vm_area *va = *(const struct vm_area **)a;
    const struct vm_area *vb = *(const struct vm_area **)b;
    if (va->start < vb->start) return -1;
    if (va->start > vb->start) return 1;
    return 0;
}

vma_map_t *vma_map_create(struct vm_area *vmas)
{
    if (!vmas)
        return NULL;

    int count = 0;
    for (struct vm_area *v = vmas; v; v = v->next)
        count++;

    vma_map_t *map = malloc(sizeof(*map));
    if (!map)
        return NULL;

    map->vmas = malloc((size_t)count * sizeof(struct vm_area *));
    if (!map->vmas) {
        free(map);
        return NULL;
    }

    map->count = count;
    int i = 0;
    for (struct vm_area *v = vmas; v; v = v->next)
        map->vmas[i++] = v;

    qsort(map->vmas, count, sizeof(struct vm_area *), cmp_vma_ptr);

    return map;
}

void vma_map_destroy(vma_map_t *map)
{
    if (!map)
        return;
    free(map->vmas);
    free(map);
}

struct vm_area *vma_map_find(vma_map_t *map, uintptr_t addr)
{
    if (!map || map->count == 0)
        return NULL;

    int lo = 0, hi = map->count - 1;
    int idx = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (map->vmas[mid]->start <= addr) {
            idx = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (idx == -1)
        return NULL;

    struct vm_area *vma = map->vmas[idx];
    if (addr < vma->end)
        return vma;
    return NULL;
}