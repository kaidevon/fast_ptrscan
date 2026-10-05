CC      := gcc
CFLAGS  := -std=c11 -D_GNU_SOURCE -Wall -Wextra -O2 -pthread \
           -Iinclude -Iinclude/format -Iinclude/vma
LDFLAGS := -pthread
LDLIBS  := -lz

BUILD   := build
TARGET  := $(BUILD)/ptrscan

SRC := \
    src/main.c \
    src/pc_list.c \
    src/ptr_index.c \
    src/fs_ptrscan.c \
    src/format/txt.c \
    src/format/idx.c \
    src/format/pcf.c \
    src/vma/vma_map.c \
    src/vma/vm_area.c \
    src/vma/vma_select.c

OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SRC))

all: $(TARGET)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

clean:
	rm -rf $(BUILD)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run