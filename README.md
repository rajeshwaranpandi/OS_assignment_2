# Pre-Faulting and Content-Aware Page Mapping Across Program Phases

This repo contains `benchmark_prefault.c`, a small benchmark that simulates a **two-phase** program over two large virtual buffers (`buff1`, `buff2`) and checks whether your OS mechanism sets up **Page mappings** correctly.

## Expected flow

1. `buff1 = mmap(...)`  
2. `buff2 = mmap(...)`  
3. `Syscall1_enable_prefault(buff1, buff2, len)`  
   - **Enables** kernel state so that when Phase-1 touches/faults pages in `buff1`, the corresponding pages in `buff2` are established too.
4. `compute_phase1(buff1)`  
   - touches a **random subset** of pages in `buff1`  
   - also creates some **duplicate-content page pairs** (extension workload)
5. `Syscall2_dedup(buff1, buff2, len)`  
   - (extension) merges identical pages into one physical page using **safe semantics** (typically read-only sharing + **copy-on-write**)
6. `compute_phase2(buff2)`  
   - touches all Phase-1 pages (should already be mapped) **plus** some extra "new" random pages (should read as zero)

## What the benchmark tests

- **Core correctness (prefault/copy for Phase-1 pages):** every page touched in Phase-1 must appear in `buff2` with the **same content**; Phase-2 should ideally avoid page-faulting for those pages (fault delta drops).
- **New Phase-2 pages:** pages not touched in Phase-1 may fault on first touch, but must become **zero-filled** pages.
- **Extension safety (dedup):** if pages are deduplicated, writing to one must **not** silently modify another (COW correctness / no writable aliasing).

## syscall failure

Before the kernel syscalls are implemented, the syscall numbers used by the benchmark typically return **`-1` with `errno=ENOSYS`** ("Function not implemented"). This normally **does not crash** the process by itself; instead the feature is absent, so:
- `buff2` remains demand-zero and will **not** match Phase-1 content -> the benchmark reports mismatches and prints `RESULT: FAIL`.

(If you want fail-fast behavior, modify the benchmark to `exit(1)` on `ENOSYS`.)

## What must be implemented to pass cleanly

1. **Syscall1** must validate arguments and establish a mechanism so that pages touched in `buff1` during Phase-1 are also mapped into `buff2` with identical content (ready before Phase-2 touches them).
2. **Syscall2 (extension)** must deduplicate identical pages while preserving correctness (typically via **copy-on-write** on write attempts).
3. Phase-2 pages not seen in Phase-1 must map to **zero-filled** pages when first touched.

## Build & run

```bash
gcc -O2 -Wall -Wextra benchmark_prefault.c -o bench
./bench
```

Reproduce a specific random plan:

```bash
SEED=123 ./bench
```
