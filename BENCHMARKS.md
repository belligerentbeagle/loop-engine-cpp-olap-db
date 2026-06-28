# Benchmarks

> The project is only as impressive as the numbers you can defend. These are measured, not asserted — reproduce them with the commands shown.

## Hardware

| | |
|---|---|
| Machine | Apple MacBook (Apple **M2**, `arm64`) |
| Cores | 8 physical (4 performance + 4 efficiency) |
| RAM | 24 GB unified memory |
| OS / toolchain | macOS 26 (Darwin 25), **Apple Clang 17**, libc++ |
| Build flags | `-O3 -march=native` (Release) |
| Dataset | 10,000,000 synthetic rows, Criteo Attribution schema (`tools/gen_synthetic.py --rows 10_000_000`), 765 MB TSV |

> Note: the README's example hardware is a ThinkPad X1 Carbon running Linux; the numbers below are from the actual machine this was built on (Apple M2). On x86, `-march=native` unlocks AVX and the `-O2`→`-O3` story is more dramatic; on Apple Silicon NEON is the baseline, which changes the interpretation (see §2).

Reproduce:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
python tools/gen_synthetic.py --rows 10_000_000 --out data/criteo_attribution.tsv --seed 7
./build/strata_bench data/criteo_attribution.tsv 11        # O3 -march=native
./build/strata_bench_o2 data/criteo_attribution.tsv 11     # -O2
./build/strata_bench_o0 data/criteo_attribution.tsv 7      # -O0 (scalar)
```

## 0. Ingestion (Phase 1 milestone)

mmap + parse + row→column pivot, single-threaded, 10 M rows × 22 columns:

| state | time | rows/s | MB/s | resident |
|---|---|---|---|---|
| cold (file not in page cache) | 9.5 s | 1.1 M/s | 80 MB/s | 1360 MB |
| warm (page cache hot) | 5.1 s | 2.0 M/s | 151 MB/s | 1360 MB |

Resident memory (1.36 GB) is **~1.8× smaller than the 2.4 GB source** thanks to dictionary
encoding of `campaign` + `cat1..9` (int32 codes instead of strings) and native numeric columns.
Parallel parsing is a known lever (left as a stretch goal — ingest here is single-threaded).

## 1. Headline: single-column filter + count  `COUNT(*) WHERE click == 1`

```
[1] COUNT WHERE click==1 : 1.35 ms | 7.41 Grows/s | 59.2 GB/s   (10M int64, branchless)
```

**~59 GB/s, ~7.4 billion rows/s.** The loop is the canonical branchless reduction
`c += (col[i] == target)`; the compiler auto-vectorizes it to NEON (see §2).

## 2. `-O0` vs `-O2` vs `-O3 -march=native` — the auto-vectorization question

Same kernel, three builds (best of N):

| build | GB/s | SIMD in hot loop? |
|---|---|---|
| `-O0` (scalar) | ~37 GB/s | **0** NEON instructions |
| `-O2` | ~54 GB/s | yes (NEON) |
| `-O3 -march=native` | ~59 GB/s | yes (NEON, 4 accumulators) |

The `-O3` disassembly of the count loop (via `clang++ -O3 -march=native -S`):
```asm
ldp   q5, q6, [x8, #-32]      ; load 4× int64 (two 128-bit regs)
cmeq.2d v5, v5, v0            ; vector compare-equal, 2 int64 lanes
...                          ; 4 vector accumulators (v1..v4) → 8 lanes/iter
```
`-O0` emits **zero** vector instructions — pure scalar — and is ~1.6× slower.

**The honest takeaway:** on Apple Silicon NEON is the baseline ISA, so `-O2` already
auto-vectorizes and `-O3 -march=native` adds only modest unrolling — the two are within noise.
More importantly, a single-column scan at this width is **memory-bandwidth bound** (reading an
80 MB column), so neither beats the ~55–60 GB/s the memory subsystem can stream. The SIMD win is
real (vs `-O0`) but the wall is RAM, not instructions — which is exactly why the *layout* matters
more than the *vectorization* for analytics (next two sections).

## 3. Thread scaling — `GROUP BY campaign SUM(cost) WHERE click == 1`

Filter (parallel mask) + dictionary GROUP BY with per-thread, cache-line-padded partials:

| threads | time | speedup |
|---|---|---|
| 1 | 10.3 ms | 1.00× |
| 2 | 5.9 ms | 1.73× |
| 4 | 5.2 ms | 1.96× |
| 8 | 5.4 ms | 1.90× |

**Scaling is near-2× by 2–4 cores, then flat — it even regresses at 8.** This is the predicted
result: the query streams three columns (~200 MB: `click`, `campaign` codes, `cost`) and a few
cores already **saturate the shared memory bandwidth**. Past that, more threads contend rather
than help. The bottleneck is bandwidth, not compute — adding cores can't add memory channels.

## 4. Row-major vs columnar — the cache argument  `SUM(cost) WHERE click == 1`

Identical query, identical data, two physical layouts (8 M rows; `sizeof(Row)=136 B`):

| layout | bytes touched / row | time | wall-clock |
|---|---|---|---|
| **columnar** (2 contiguous arrays) | 16 B | **3.2 ms** | **7.3× faster** |
| row-major (array-of-structs) | 136 B (whole record strided in) | 23.5 ms | — |

The query reads only `click` + `cost` (16 B). Columnar stores each contiguously, so the scan
streams exactly those 16 B/row. Row-major interleaves all 22 fields, so every cache line drags
120 wasted bytes along — **8.5× more memory traffic**, which shows up as a **7.3× slowdown**.
This is *the* reason columnar beats row-major for analytics, and it's mechanical, not magic.

## Correctness

Every aggregate is validated exactly against pandas (`tests/` + `python/demo.py --check`):

```
COUNT(*)            strata=10000000  pandas=10000000  OK
COUNT click==1      strata=500873    pandas=500873    OK
COUNT conversion==1 strata=26788     pandas=26788     OK
SUM(cost) (int)     strata=1620175   pandas=1620175   OK
```

Single-threaded and 8-threaded paths agree to floating-point noise (≤ 1e-12 relative) on the
200k-row equivalence test in `tests/test_strata.cpp`.

## What would move these numbers

- **Ingest:** parallel chunked parsing (split the mmap at newline boundaries) — the obvious 4–8× lever.
- **Filtered scans:** late materialization / predicate fusion (carry a selection vector, avoid
  the separate mask pass) — fewer memory passes on selective queries.
- **Group-by:** narrower columns (int32/int8 for flags) to halve memory traffic on the hot columns.
- **`MIN`/`MAX`:** currently a per-row branch; a masked-blend would vectorize it like `SUM`.
