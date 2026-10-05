#ifndef VMA_SELECT_H
#define VMA_SELECT_H

#include "vma/vm_area.h"

struct vma_select {
    char *module;
    char *perm;
    int seg_type;
    int index;
};

struct vma_select **to_vma_select(const char **s, int count);
void free_vma_select(struct vma_select **select, int count);

// Destructive select
int vma_filter_select(struct vm_area **area, struct vma_select **select, int count);

#endif  /* VMA_SELECT_H */