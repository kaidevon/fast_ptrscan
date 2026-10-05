# ptrscan

Fast in-process pointer-chain scanner for Android (arm64-v8a).

Scans a target process for pointer chains leading to a given address, using
a compact in-memory index of every valid pointer slot in the process image.

## Features

- Single-pass memory scan into a sorted index (IDX), with parallel radix sort.
- Persistent IDX files: build once, query many times.
- Reverse BFS with per-node top-K pruning to bound memory.
- Per-layer offset whitelist (`-t`) for precise tail constraints.
- Realtime progress bar and per-layer delta histogram.
- Reads chains from `.txt` and `.pcf`, writes the same.

## Build

Requires the Android NDK.

```sh
ndk-build
```

The binary lands in `libs/arm64-v8a/ptrscan`.

Push and run on a device or emulator:

```sh
adb push libs/arm64-v8a/ptrscan /data/local/tmp/
adb shell /data/local/tmp/ptrscan --help
```

Root is required to read another process's memory via `process_vm_readv`.

## Usage

```
ptrscan <COMMAND> [ARGS]

Commands:
  index    Build a pointer index from a running process and save to IDX
  scan     Build index on the fly (or load from IDX) and scan pointer chains
  help     Print help
```

### index

Build an IDX snapshot of the target process.

```sh
ptrscan index <PID> [-o FILE] [-j N]
```

- `-o, --output FILE` — write IDX to FILE (optional; without it the index
  is only reported, not saved).
- `-j, --jobs N` — worker threads (default: number of CPUs).

Example:

```sh
ptrscan index 5160 -o 5160.idx
```

### scan

```sh
ptrscan scan <PID> <TARGET> [FLAGS]
ptrscan scan <TARGET> -p <PID> [FLAGS]
ptrscan scan <TARGET> -i <FILE.idx> [FLAGS]
```

Three equivalent ways to specify the process: positional `<PID>`, `-p/--pid`,
or an IDX file via `-i/--idx` (no live process needed).

Flags:

| Flag | Description |
|------|-------------|
| `-p, --pid N`         | Target PID |
| `-i, --idx FILE`      | Load IDX from FILE instead of building from PID |
| `-d, --depth N`       | Max pointer depth (default 6, 0 = unlimited) |
| `-m, --min-depth N`   | Min pointer depth |
| `-n, --max-chains N`  | Stop after N chains |
| `-j, --jobs N`        | Worker threads (index build only) |
| `-k, --max-targets L` | Per-layer cap on retained targets, comma list |
| `-O, --offset N`      | Pointer search window per layer (default 0x1000) |
| `-t, --tail LIST`     | Per-layer tail offset whitelist, one `-t` per layer |
| `-o, --output FILE`   | Write chains to FILE (`.txt` or `.pcf`) |

Examples:

```sh
# Live process, build index on the fly
ptrscan scan 5160 0x7f2e4a0012a0 -d 6 -k 3 -o chains.txt

# Reuse a saved index, no live process needed
ptrscan scan 0x7f2e4a0012a0 -i 5160.idx -d 6 -k 3 -o chains.txt

# Constrain the first three layers to known offsets
ptrscan scan 0x7f2e4a0012a0 -i 5160.idx \
    -t 0x45c -t 0x110,0x118 -t 0x2b0 \
    -o chains.pcf
```

### Parameters in detail

**`-k / --max-targets`**

Per-layer cap on how many targets are kept for each parent slot. This is the
main defense against exponential blow-up.

- Comma-separated list, e.g. `-k 0,0,128,64,32,16,8`.
- Index mapping: `idx[1]` = layer 0, `idx[2]` = layer 1, `idx[0]` unused.
- A single value applies to every layer: `-k 3`.
- `0` means unlimited (not recommended for deep scans).

**`-t / --tail`**

One `-t` per layer, ordered from nearest to farthest from the target.
Comma-separated offsets apply to the same layer.

```sh
-t 0x45c -t 0x110,0x118,0x130 -t 0x2b0
```

A candidate edge is kept only if its delta matches one of the offsets for
that layer. Any layer without a `-t` is unconstrained.

**`-O / --offset`**

Size of the value window scanned below each target: a slot `P` is a hit if
`P.value ∈ [T - offset, T]`.

## Output format

`.txt` — human-readable. Header comments, then a group per module/segment
with indented offset chains.

```
# ptrscan
# groups: 1 chains: 2

/path/to/libgame.so .data[0]
  0x45c->0x110->0x2b0
  0x45c->0x118->0x2b0
```

`.pcf` — binary with super block, CRC32 over header and file, grouped chains.
Selected by file extension; any other extension (or none) uses `.txt`.

## Progress

During scan, stderr shows a one-line status:

```
[bfs   ] d=3  in=78204 out=402910 hits=908528 pm=2.0M anc=445 chains=0 14.2s
```

Fields: phase, current depth, nodes entering the layer, next-layer size, hits
after pruning, parent-map size, anchors found, chains emitted, elapsed time.

## Design notes

- **Index (IDX).** A snapshot of every 8-byte-aligned pointer-sized value in
  the process that lies within a mapped segment. Stored as
  `(value, segment | slot)` pairs, sorted by value for O(log n) window queries.
- **Parent map.** During BFS, each new parent slot is stored once with a small
  list of targets it points to. Enumeration later walks this map from anchors
  back to the target.
- **Bounded memory.** Per-node target cap, global visited set, edge dedup, and
  the search window together bound the working set to `O(K · unique_slots)`
  regardless of depth.

## License

Licensed under the Apache License, Version 2.0.
See [LICENSE](LICENSE) for the full text.