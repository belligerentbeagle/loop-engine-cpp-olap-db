# Strata

> A vectorized, columnar OLAP engine for advertising event logs — built from scratch in C++, queried from Python.

Strata ingests millions of advertising events (impressions, clicks, conversions) and answers analytical questions over them in milliseconds. It is a learning-grade reimplementation of the core ideas behind systems like [ClickHouse](https://clickhouse.com/) and [DuckDB](https://duckdb.org/): columnar storage, vectorized execution, SIMD-friendly tight loops, memory-mapped I/O, and multi-threaded aggregation. A thin `pybind11` layer exposes the engine to Python so you can drive it from a notebook or a dashboard.

---

## Status — all four phases implemented ✅

| | |
|---|---|
| **Phase 1** mmap'd columnar storage + TSV parser | 10 M rows ingested, 22 columns, ~2 M rows/s (warm), dictionary-encoded categoricals |
| **Phase 2** vectorized filter / aggregate / GROUP BY | **~59 GB/s** single-column scan; auto-vectorized NEON hot loops |
| **Phase 3** `std::jthread` pool + streaming `MemTable` | parallel aggregation; lock-free SPSC queue; query-while-ingest |
| **Phase 4** `pybind11` + Arrow + Streamlit dashboard | `import strata`; zero-copy numpy/Arrow; interactive Plotly UI |

Every aggregate is validated exactly against pandas. Headline result: **columnar is 7.3× faster than a row-major baseline** on the same query, and scans run at the machine's memory-bandwidth limit. Full measured numbers, hardware, and the SIMD disassembly: **[BENCHMARKS.md](BENCHMARKS.md)**.

**Quickstart:**
```bash
./build.sh                                                   # cmake + build everything
python tools/gen_synthetic.py --rows 10_000_000 \
       --out data/criteo_attribution.tsv                     # or drop the real Criteo TSV here
(cd build && ctest --output-on-failure)                      # 40 correctness checks
./build/strata query data/criteo_attribution.tsv \
       --threads 8 --where click eq 1 --group campaign --sum cost
python python/demo.py --data data/criteo_attribution.tsv --check
streamlit run python/dashboard.py -- --data data/criteo_attribution.tsv
```

---

## Why this exists

Most "database" side projects are CRUD apps over SQLite, and they teach you almost nothing about how a database actually spends its CPU cycles. Strata exists to make a few systems ideas concrete and measurable:

- **Why columnar beats row-major for analytics.** Analytical queries touch a few columns across *all* rows. Storing a column contiguously means a scan reads only the bytes it needs, and the CPU prefetcher and cache lines work *for* you instead of against you. The win is mechanical, and you'll see it in `perf stat` cache-miss counts, not just wall-clock.
- **Why a tight loop over a primitive array is fast.** A `for` loop over a `std::vector<int64_t>` with no virtual calls and no pointer chasing is exactly the shape a compiler can auto-vectorize into SIMD. You write scalar code; `-O3 -march=native` turns it into 4–8 lanes per instruction. The point of the project is to *watch this happen* (via the assembly and the benchmark) rather than take it on faith.
- **Why ingestion and query have to coexist.** Real analytics systems answer queries while new data streams in. That forces a buffer/flush design (a "MemTable") and concurrency primitives, which is where `std::jthread`, `std::stop_token`, and lock-free queues earn their place.

The deliverable is not a toy. By the end you have an artifact that demonstrates you understand CPU caching, memory alignment, data-oriented design, and the cost model of real query engines — the things a CRUD app can't show.

---

## Architecture

```
                        Python (Streamlit / FastAPI / notebook)
                                       │
                                       │  import strata     ← pybind11 module
                                       ▼
  ┌─────────────────────────────────────────────────────────────────────┐
  │                          C++ CORE ENGINE                             │
  │                                                                      │
  │   Ingestion path                         Query path                  │
  │   ──────────────                         ──────────                  │
  │                                                                      │
  │   CSV/TSV  ──► Parser ──► MemTable        QueryRequest                │
  │   (mmap)             (row buffer)            │                       │
  │                          │                   ▼                       │
  │                    flush @ N rows      Vectorized Executor           │
  │                          │             (filter → aggregate)          │
  │                          ▼                   │                       │
  │                   Columnar Store ◄───────────┘                       │
  │              (one contiguous array per column)                       │
  │                          │                                           │
  │              Thread pool splits column into chunks,                  │
  │              each core aggregates a slice, results merge             │
  └─────────────────────────────────────────────────────────────────────┘
                                       │
                       Apache Arrow (zero-copy) ──► Python / Plotly
```

**Two layers, on purpose:**

1. **C++ core** owns everything performance-critical: mmap'd file reads, the row→column pivot, the columnar in-memory store, and multi-threaded aggregation.
2. **Python wrapper** owns ergonomics: you don't write C++ to look at a chart. Arrays cross the boundary as Apache Arrow buffers so there's no copy on the way out.

---

## Feature roadmap

The project is built in four phases. Each phase is independently demoable and benchmarkable — resist the urge to build everything before measuring anything.

### Phase 1 — Columnar storage engine *(beginner → intermediate)*
- [x] Parse a large CSV/TSV event log.
- [x] Pivot rows into per-column contiguous arrays (`std::vector<int64_t>` for timestamps, dictionary-encoded ids for low-cardinality categoricals, `std::vector<std::string>` or an offset+blob layout for the rest).
- [x] Memory-map the source file (`mmap` on Linux) so reads page in lazily without buffering the whole file in user space.
- **Milestone:** load the dataset and report rows ingested, ingest throughput (rows/s, MB/s), and resident memory.

### Phase 2 — Vectorized query execution *(intermediate)*
- [x] A column-at-a-time evaluator: filters and aggregations operate on raw arrays, never on per-row objects.
- [x] Implement `COUNT`, `SUM`, `AVG`, and a `GROUP BY` over a dictionary-encoded column.
- [x] Compile with `-O3 -march=native`, inspect the emitted assembly (`-S` or [godbolt](https://godbolt.org/)), and confirm the hot loop auto-vectorized.
- **Milestone:** scan throughput in GB/s, and a side-by-side of `-O2` vs `-O3 -march=native` on the same query.

```cpp
// The canonical hot loop: contiguous, branch-light, auto-vectorizable.
std::size_t count_events(const std::vector<std::int32_t>& event_col,
                         std::int32_t target) {
    std::size_t n = 0;
    for (auto e : event_col) n += (e == target);   // branchless; compiler emits SIMD
    return n;
}
```

### Phase 3 — Multithreaded execution & buffering *(advanced)*
- [x] Split column scans into chunks; a `std::jthread` pool aggregates partials, then merge. Target near-linear scaling to physical core count.
- [x] A row-oriented `MemTable` buffers incoming events; on hitting a threshold (e.g. 10k rows) a background thread flushes and pivots them into the columnar store — so queries can run during ingest.
- [x] Use a lock-free (or at least lock-minimal) queue between producer and flush thread; `std::stop_token` for clean shutdown.
- **Milestone:** thread-scaling chart (1→N cores) and a demo querying while streaming.

### Phase 4 — Expose to the frontend
- [x] `pybind11` builds the engine into a native module (`import strata`).
- [x] Return results as Apache Arrow arrays so Python reads them zero-copy.
- [x] A small Streamlit/Plotly dashboard: pick a metric, group by a dimension, see it render instantly.
- **Milestone:** end-to-end — Python query → C++ scan → Arrow → chart, with the round-trip timed.

---

## Tech stack

| Concern            | Choice                                              |
|--------------------|-----------------------------------------------------|
| Core language      | C++20 (`std::jthread`, `std::stop_token`, concepts) |
| Build              | CMake ≥ 3.20                                         |
| Python bindings    | pybind11                                            |
| Cross-lang data    | Apache Arrow (zero-copy buffers)                    |
| Frontend           | Streamlit or FastAPI + Plotly                       |
| Bench / profiling  | Google Benchmark, `perf`, godbolt                  |
| Tests              | GoogleTest or Catch2                                |

---

## Project structure

```
strata/
├── CMakeLists.txt          # core lib + CLI + tests + bench(O3/O2/O0) + pybind11 module
├── build.sh                # one-shot configure + build
├── README.md
├── BENCHMARKS.md           # measured numbers, hardware, SIMD disassembly
├── include/strata/
│   ├── dictionary.hpp      # dictionary encoding (transparent string hashing)
│   ├── column.hpp          # typed columns (Int64 / Float64 / Dict)
│   ├── schema.hpp          # column name→type map + criteo_schema()
│   ├── table.hpp           # columnar store
│   ├── parser.hpp          # mmap + CSV/TSV parsing
│   ├── executor.hpp        # vectorized filter / aggregate / GROUP BY
│   ├── sql.hpp             # SQL-string frontend (parsed to the executor)
│   ├── threadpool.hpp      # jthread pool, map_reduce, for_ranges
│   └── memtable.hpp        # lock-free SPSC ring + streaming row buffer
├── src/
│   ├── parser.cpp  table.cpp  executor.cpp  threadpool.cpp  memtable.cpp  sql.cpp
├── apps/
│   └── strata_cli.cpp      # describe / head / query / stream
├── bindings/
│   └── strata_py.cpp       # pybind11 module (import strata)
├── bench/
│   └── bench_scan.cpp      # scan throughput, thread scaling, row-vs-columnar
├── tests/
│   └── test_strata.cpp     # self-contained correctness suite (no external framework)
├── tools/
│   ├── gen_synthetic.py    # synthetic Criteo-schema generator (deterministic)
│   └── fetch_criteo.sh     # best-effort real-dataset download
├── python/
│   ├── dashboard.py        # Streamlit + Plotly demo
│   └── demo.py             # headless end-to-end demo (+ pandas cross-check)
└── data/
    └── README.md           # where to drop / how to generate the dataset
```

> **Dependency choices (deliberate):** the engine has **zero hard third-party dependencies** —
> tests and benchmarks are self-contained (no GoogleTest/Google Benchmark), and results cross to
> Python as numpy arrays wrapped zero-copy into `pyarrow` rather than linking the Arrow C++ SDK.
> This keeps the build a single `cmake` away on any machine with a C++20 compiler.

---

## Build

```bash
# C++ core + bindings
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-O3 -march=native"
cmake --build build -j

# install the Python module into your venv
pip install ./build      # or: pip install -e . if you wire up scikit-build-core
```

Minimal `CMakeLists.txt` shape (bindings only shown):

```cmake
cmake_minimum_required(VERSION 3.20)
project(strata LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(pybind11 CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)

add_library(strata_core src/parser.cpp src/table.cpp src/executor.cpp src/threadpool.cpp)
target_include_directories(strata_core PUBLIC include)
target_link_libraries(strata_core PUBLIC Arrow::arrow_shared)

pybind11_add_module(strata bindings/strata_py.cpp)
target_link_libraries(strata PRIVATE strata_core)
```

---

## Usage

**From C++:**

```cpp
#include "strata/table.hpp"
#include "strata/executor.hpp"

// mmap + parse with the built-in Criteo schema (header-driven; column order doesn't matter)
auto table = strata::Table::from_criteo("data/criteo_attribution.tsv");

// SUM(cost) WHERE click == 1 GROUP BY campaign, across 8 threads
auto result = strata::Executor(table)
                  .threads(8)
                  .filter_eq("click", 1)
                  .group_by("campaign")
                  .agg(strata::Agg::Sum, "cost");
result.sort_by_value_desc();
std::puts(result.to_string(/*top*/ 10).c_str());
```

Or from the CLI (no Python needed):
```bash
./build/strata query data/criteo_attribution.tsv --threads 8 \
    --where click eq 1 --group campaign --sum cost --limit 10
./build/strata stream data/criteo_attribution.tsv   # query while streaming ingest
```

**From Python:**

```python
import strata

t = strata.load("data/criteo_attribution.tsv", sep="\t")   # default schema = Criteo

# returns an Arrow table; numeric columns cross the boundary zero-copy
df = (t.filter(click=1)
       .group_by("campaign")
       .count()
       .to_arrow())

print(df.to_pandas().sort_values("count", ascending=False).head())

# scalars, other aggregates, threads, time-bucketing:
ctr = t.filter(click=1).count().values[0] / t.count().values[0]
spend = t.threads(8).filter(click=1).group_by("campaign").sum("cost")
by_day = t.group_by("timestamp", 86400).count()        # one bucket per day
```

**Or just type SQL** (a small `SELECT … WHERE … GROUP BY … ORDER BY … LIMIT` subset, parsed to the executor):

```python
df = t.sql("""SELECT campaign, SUM(cost)
              FROM events WHERE click = 1
              GROUP BY campaign ORDER BY 2 DESC LIMIT 10""", threads=8).to_pandas()
```

```bash
# one-shot
./build/strata sql data/criteo_attribution.tsv --threads 8 \
    "SELECT campaign, SUM(cost) FROM events WHERE click=1 GROUP BY campaign ORDER BY 2 DESC LIMIT 10"

# interactive REPL (no query argument)
./build/strata sql data/criteo_attribution.tsv
sql> SELECT cat1, AVG(cpo) FROM events WHERE conversion=1 GROUP BY cat1 ORDER BY 2 DESC LIMIT 5
```

Supported: `COUNT(*)`/`SUM`/`AVG`/`MIN`/`MAX`, `WHERE` with `= != < <= > >=` joined by `AND`,
`GROUP BY` one column, `ORDER BY` the measure, `LIMIT`. Not (yet): joins, `OR`, `HAVING`,
projections without an aggregate — each rejected with a clear message.

---

## Sample data — advertising analytics

You asked for real ad data large enough to stress the engine. Below are three options at increasing scale. All three are anonymized real-world advertising logs from **Criteo** / **Avazu**. Pick based on how hard you want to push the system.

> **Licensing note:** the Criteo datasets are released under **CC BY-NC-SA 4.0** (non-commercial). Fine for a portfolio/learning project; just don't build a commercial product on them, and attribute Criteo.

### Tier 1 — Criteo Attribution Modeling for Bidding *(recommended starting point)*

The best **semantic** fit for an analytics dashboard, because — unlike the hashed datasets below — its columns have **readable meaning**.

- **Scale:** ~16.5M rows, a 30-day sample of live Criteo traffic; one row per impression. Compresses to a single manageable file (hundreds of MB).
- **Schema (tab-separated), the useful columns:** `conversion` (1 if a conversion occurred in the 30 days after the impression), `conversion_timestamp`, `conversion_id` (so timelines can be reconstructed), `attribution` (1 if the conversion was attributed to Criteo), `click` (1 if clicked), `click_pos`, `click_nb`, `cost` (price paid for the display), `cpo` (cost-per-order). Plus a per-impression `timestamp`, a `campaign` id, and `cat[1-9]`, contextual categorical features whose meaning is not disclosed.
- **Why it fits Strata:** you get a real **timestamp** (time-bucketed aggregation), a real **campaign** dimension (`GROUP BY`), numeric **cost/cpo** (`SUM`/`AVG`), and binary **click/conversion** flags (filters + CTR/CVR ratios). That's a complete analytics surface with meaningful column names — ideal for a dashboard demo.
- **Get it:**
  - Source page: https://ailab.criteo.com/criteo-attribution-modeling-bidding-dataset/
  - Kaggle mirror: https://www.kaggle.com/datasets/sharatsachin/criteo-attribution-modeling

### Tier 2 — Avazu Click-Through Rate Prediction *(middle weight)*

A 2014 Kaggle competition dataset of mobile ad impressions. Bigger than Tier 1, partially readable columns.

- **Scale:** over 40 million records of ad impressions, each with 24 features; the training set is ~6.7GB, spanning 11 days. Because there are ~10 days of training data, a clean approach is to use a single day as a representative working sample rather than a random sub-sample.
- **Schema:** a `click` label plus `hour`, `banner_pos`, `site_*`/`app_*` ids, `device_type`, `device_conn_type`, and anonymized `C1`–`C21` categoricals. The `hour` field doubles as a coarse timestamp (format `YYMMDDHH`).
- **Why it fits Strata:** big enough that naive row-by-row processing hurts, so the columnar win is visible; high-cardinality categoricals make dictionary encoding worthwhile.
- **Get it:** https://www.kaggle.com/c/avazu-ctr-prediction/data

### Tier 3 — Criteo 1TB Click Logs *(the stress test — go here when you want to hurt your laptop)*

The canonical large-scale ad dataset. Use this once the engine works, to benchmark out-of-core / chunked ingestion.

- **Scale:** ~1.3 TB uncompressed, over four billion samples spanning 24 days. Each record has 40 columns: a click label, 13 integer features (I1–I13), and 26 categorical features (C1–C26), hashed onto 32 bits, tab-separated. Criteo provides one ~15GB compressed `.gz` file per day, so you can grab a single day and still have tens of millions of rows.
- **Caveat:** columns are fully anonymized/hashed — great for raw scan/aggregation **throughput** benchmarks, weak for a *meaningful* dashboard. Use it to prove the engine scales, not to tell an analytics story.
- **Get it:**
  - Source + terms: https://ailab.criteo.com/download-criteo-1tb-click-logs-dataset/
  - HuggingFace mirror: https://huggingface.co/datasets/criteo/CriteoClickLogs
  - Smaller 7-day Kaggle variant (Display Advertising Challenge): https://www.kaggle.com/c/criteo-display-ad-challenge

### Suggested progression

Start on **Tier 1** so your dashboard has meaningful dimensions and you can sanity-check correctness against pandas/DuckDB. Move to **Tier 2** to feel the columnar/SIMD payoff. Reach for a single day of **Tier 3** only when you're benchmarking ingestion and want a number with a "millions of rows/sec" in it.

If you want a deterministic, license-free option for unit tests, generate synthetic events (`timestamp, user_id, campaign_id, event_type, cost`) with a small Python script — handy for asserting exact aggregation results that real anonymized data can't give you.

---

## Benchmarking methodology

The project is only as impressive as the numbers you can defend. Measure, don't assert:

- **Scan throughput** (GB/s and rows/s) for a single-column filter+count. This is your headline metric.
- **`-O2` vs `-O3 -march=native`** on the identical query — quantify the auto-vectorization win.
- **Thread scaling**: 1 → N cores, plotted; note where it stops being linear and reason about why (memory bandwidth, not compute, usually caps it).
- **Row-major vs columnar**: implement a naive row-struct baseline and show the cache-miss difference with `perf stat -e cache-misses,cache-references`.
- **Sanity baseline**: run the same query in DuckDB and pandas. You don't need to *win* (DuckDB is a decade of engineering); being within a small multiple while understanding the gap is the real result.

State your hardware (CPU model, core count, RAM, that it's the ThinkPad X1 Carbon running Linux) next to every number, or the numbers mean nothing.

---

## Design notes worth defending in an interview

- **Dictionary encoding** for categoricals (`campaign`, `site_id`): map strings to small ints once, store the ints, aggregate on ints. Cheaper memory, faster compares, and `GROUP BY` becomes array indexing.
- **`mmap` vs `read()`**: mmap lets the page cache serve hot data and pages in lazily, but you trade explicit control of I/O and risk page-fault stalls inside hot loops. Know when each wins.
- **Branchless aggregation**: `n += (e == target)` instead of an `if` keeps the loop vectorizable and avoids branch mispredicts on unpredictable data.
- **False sharing** in the thread pool: pad per-thread partial accumulators to a cache line so cores aren't fighting over the same line.
- **Why Arrow at the boundary**: it's the lingua franca of columnar data; returning Arrow means Python (and pandas/Polars/DuckDB) read your results with no serialization copy.

---

## Roadmap / stretch goals

- Late-materialization (carry row selections as index vectors, materialize columns only at the end).
- Run-length / bit-packed encoding for sortable columns.
- ✅ **A tiny SQL-ish parser so queries are strings, not builder calls.** *(done — `strata sql`, `t.sql()`; see [Usage](#usage))*
- Predicate pushdown into the parse step (skip rows during ingest).
- Explicit SIMD with `std::experimental::simd` or intrinsics, compared against the auto-vectorized baseline.

---

## License

Code: your choice (MIT is a sensible default for a portfolio project).
Data: the Criteo datasets above are **CC BY-NC-SA 4.0** — non-commercial, attribute Criteo, share-alike.

## References

- ClickHouse — https://clickhouse.com/
- DuckDB — https://duckdb.org/
- Criteo AI Lab datasets — https://ailab.criteo.com/ressources/
- pybind11 — https://pybind11.readthedocs.io/
- Apache Arrow — https://arrow.apache.org/
