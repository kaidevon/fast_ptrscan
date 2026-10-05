# Changelog

All notable changes to **ptrscan** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.0.0-rc.1] - 2026-10-06

> A cleaner `fs_ptrscan.h`, a more memory-efficient `fs_ptrscan.c`,
> and clearer progress visualization. Tested against the game
> `com.ShuiSha.FPS2`; performs well in a 7-layer pointer scenario.

First public release candidate. This version is a broad cleanup of the scan
engine, the public interface, and progress reporting. **The public API is
not compatible with `0.1.0`** — read the migration notes below before
upgrading.

### ⚠ Breaking Changes

- **Core public API rewritten.** The old `fs_ptrscan()` entry point and its
  scattered option fields are gone. The public interface is now consolidated
  into three structs — `fs_scan_opts`, `fs_scan_progress`, and
  `fs_scan_perf` — for easier long-term maintenance.
  - Affected: any code that calls `fs_ptrscan()` directly and builds its
    options by hand.
  - Migration:
    - Options: use `fs_scan_opts_create()`, then set each field via
      `fs_scan_opts_set_max_off()`,
      `fs_scan_opts_set_max_targets_per_node()`,
      `fs_scan_opts_set_tail_layers()`, and
      `fs_scan_opts_set_anchors()`.
    - Progress: use `fs_scan_progress_create()` to create a progress
      object, and read snapshots via `fs_scan_progress_get()` /
      `fs_scan_perf_get()`. The old atomic counter fields (`depth`,
      `layer_in`, `layer_out`, `total_edges`, etc.) have been removed.
  - See the [migration guide](docs/migration-0.x-to-1.0.md) for details
    (to be written).

- **`-k` layer index semantics changed.** `max_targets_per_node[0]` is now
  unused; indexing starts at `[1]` for Layer 1, matching the `Layer N`
  labels shown in progress output.
  - Affected: users who pass a comma list to `-k` and rely on the old
    0-based mapping.
  - Migration: prepend a placeholder to the list, e.g.
    `-k 0,0,128,64,32,16,8`; or use a single value such as `-k 3` to apply
    to every layer.

- **`.idx` on-disk layout changed.** The `format/idx` module version has
  been bumped; existing IDX files can no longer be loaded.
  - Affected: users with IDX files saved by `0.1.0`.
  - Migration: rebuild with the new version via
    `ptrscan index <PID> -o new.idx`.

### Changed

- **`fs_ptrscan.h`**: the public API is consolidated into `fs_scan_opts` /
  `fs_scan_progress` / `fs_scan_perf`; internal fields are no longer
  exposed.
- **`fs_ptrscan.c`**: significantly improved memory efficiency.
  - `pm_target` entries in `parent_map` are now slab-allocated and packed,
    eliminating the per-node `malloc` overhead of the old linked list.
  - A per-node fan-out cap (top-K pruning) bounds parent map memory to
    `unique_parents × K`, independent of BFS depth.
  - `visited` is now a bitmap: one bit per 8-byte pointer slot.
  - `(P, T)` edge deduplication is handled by an open-addressing `edge_set`.
- **Progress visualization**: progress reporting for the index and scan
  phases has been rewritten around snapshots; the per-layer delta
  histogram has been updated; the enum phase now reports progress too.
  The single-line status shows `in` / `out` / `hits` / `pm` / `anc` /
  `chains` and elapsed time layer by layer.

### Added

- Cooperative cancellation: `fs_scan_progress_cancel()` and an external
  `_Atomic int *cancel` flag. The CLI installs a SIGINT handler that
  cancels both index build and scan.
- `analyze` subcommand:
  - `--tail` / `--shared-tail`: report the common tail offsets across all
    input files, grouped by module/segment.
  - `--intersection` / `--intersect`: emit only chains that appear in every
    input file.
- `-L/--log FILE`: write the full per-layer delta histogram to a log file.
- `-V/--version` and `help <cmd>`.
- Combined short option parsing (`-d6`, `-k0,0,32`, `-j8`, `-oFILE`,
  `-p1234`).
- Colored output honoring `NO_COLOR`, `CLICOLOR_FORCE`, and `TERM=dumb`.

### Fixed

- Duplicate chains produced when multiple targets hit the same slot in the
  same BFS layer.
- Unbounded growth of the parent map at large BFS depths.
- Unbounded recursion depth in the ENUM phase; it is now bounded by
  `min_depth` monotonicity.

### Notes

- The version string `VERSION` is defined exactly once, in `ptrscan.c`, and
  is used only by `-V` / `--version`.
- License split:
  - `fs_ptrscan.h`, `fs_ptrscan.c` — Apache License 2.0.
  - `ptrscan.c` — MIT License.
- The `.idx` on-disk layout is versioned by the `format/idx` module. It has
  been bumped in this release and is not yet declared stable across major
  versions.

[Unreleased]: https://example.com/ptrscan/compare/v1.0.0-rc.1...HEAD
[1.0.0-rc.1]: https://example.com/ptrscan/releases/tag/v1.0.0-rc.1