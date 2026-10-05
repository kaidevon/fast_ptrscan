LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE     := ptrscan
LOCAL_SRC_FILES  := \
    ../src/main.c \
    ../src/fs_ptrscan.c \
    ../src/ptr_index.c \
    ../src/pc_list.c \
    ../src/format/idx.c \
    ../src/format/pcf.c \
    ../src/format/txt.c \
    ../src/vma/vm_area.c \
    ../src/vma/vma_map.c \
    ../src/vma/vma_select.c

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/../include \
    $(LOCAL_PATH)/../include/format \
    $(LOCAL_PATH)/../include/vma

LOCAL_CFLAGS     := -std=c11 -D_GNU_SOURCE -O2 -Wall
LOCAL_LDLIBS     := -lz

include $(BUILD_EXECUTABLE)