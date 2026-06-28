// bench/bench_scan.cpp — the numbers the README asks you to defend.
//
//   1. Scan throughput (GB/s, rows/s) for a single-column filter+count  [headline]
//   2. -O2 vs -O3 -march=native on the identical kernel  (build this file both ways)
//   3. Thread scaling 1 -> N cores on a GROUP BY aggregation
//   4. Row-major (array-of-structs) vs columnar on the same query  [cache effect]
//
// The headline/cache kernels are defined *in this TU* so the compile flags used to build
// the benchmark actually change their codegen (that's the point of the O2-vs-O3 compare).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "strata/executor.hpp"
#include "strata/parser.hpp"

using namespace strata;
using Clock = std::chrono::steady_clock;

template <class F>
static double best_seconds(int reps, F&& f) {
    double best = 1e30;
    for (int r = 0; r < reps; ++r) {
        auto t0 = Clock::now();
        f();
        double s = std::chrono::duration<double>(Clock::now() - t0).count();
        best = std::min(best, s);
    }
    return best;
}

// ---- kernels (compiled at this file's flags) --------------------------------------
// The canonical hot loop from the README: contiguous, branch-light, auto-vectorizable.
__attribute__((noinline)) std::size_t count_eq_i64(const std::int64_t* d, std::size_t n, std::int64_t target) {
    std::size_t c = 0;
    for (std::size_t i = 0; i < n; ++i) c += (d[i] == target);
    return c;
}

// SUM(cost) WHERE click==1, columnar: touches only the two columns it needs, contiguously.
// Four accumulators break the FP-add dependency chain (FP add isn't associative, so the
// compiler won't reassociate a single-accumulator reduction) — leaving it memory-bound,
// which is exactly the variable we want to isolate for the layout comparison.
__attribute__((noinline)) double sum_cost_if_click_columnar(const std::int64_t* click, const double* cost, std::size_t n) {
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += double(click[i + 0]) * cost[i + 0];
        s1 += double(click[i + 1]) * cost[i + 1];
        s2 += double(click[i + 2]) * cost[i + 2];
        s3 += double(click[i + 3]) * cost[i + 3];
    }
    double s = s0 + s1 + s2 + s3;
    for (; i < n; ++i) s += double(click[i]) * cost[i];
    return s;
}

// The same query over a row-major array-of-structs. The stride drags every other field of
// each record through the cache even though the query reads just two of them.
struct Row {
    std::int64_t timestamp, uid, conversion, conversion_timestamp, conversion_id;
    std::int64_t attribution, click, time_since_last_click, click_pos, click_nb;
    std::int32_t campaign, cat[9];
    double cost, cpo;
};
__attribute__((noinline)) double sum_cost_if_click_rowmajor(const Row* rows, std::size_t n) {
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;  // same accumulator trick; only the layout differs
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += double(rows[i + 0].click) * rows[i + 0].cost;
        s1 += double(rows[i + 1].click) * rows[i + 1].cost;
        s2 += double(rows[i + 2].click) * rows[i + 2].cost;
        s3 += double(rows[i + 3].click) * rows[i + 3].cost;
    }
    double s = s0 + s1 + s2 + s3;
    for (; i < n; ++i) s += double(rows[i].click) * rows[i].cost;
    return s;
}

static volatile std::size_t g_sink_u = 0;
static volatile double g_sink_d = 0;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: bench_scan FILE [reps]\n"); return 1; }
    const int reps = argc > 2 ? std::atoi(argv[2]) : 7;

#if defined(__OPTIMIZE__)
    const char* opt = "optimized";
#else
    const char* opt = "UNOPTIMIZED";
#endif
    std::printf("# strata bench  (build: %s)\n", opt);

    LoadOptions o; o.verbose = true;
    Table t = Table::from_csv(argv[1], criteo_schema(), o);
    const std::size_t n = t.num_rows();
    const auto& click = t.column("click").i64;
    const auto& cost  = t.column("cost").f64;
    std::printf("\nrows = %zu\n", n);

    // --- 1 & 2: headline single-column filter+count -------------------------------
    {
        double s = best_seconds(reps, [&] { g_sink_u = count_eq_i64(click.data(), n, 1); });
        double gbps = (n * 8.0) / 1e9 / s;
        std::printf("\n[1] COUNT WHERE click==1   : %.3f ms  | %.2f Grows/s | %.2f GB/s  (result=%zu)\n",
                    s * 1e3, n / s / 1e9, gbps, (std::size_t)g_sink_u);
    }

    // --- 3: thread scaling on a GROUP BY ------------------------------------------
    std::printf("\n[3] thread scaling: GROUP BY campaign SUM(cost) WHERE click==1\n");
    double t1 = 0;
    for (unsigned p : {1u, 2u, 4u, 8u}) {
        double s = best_seconds(reps, [&] {
            auto r = Executor(t).threads(p).filter_eq("click", 1).group_by("campaign").agg(Agg::Sum, "cost");
            g_sink_u = r.values.size();
        });
        if (p == 1) t1 = s;
        std::printf("    threads=%u : %7.3f ms  | %.2f Grows/s | speedup %.2fx\n",
                    p, s * 1e3, n / s / 1e9, t1 / s);
    }

    // --- 4: row-major vs columnar on SUM(cost) WHERE click==1 ---------------------
    {
        const std::size_t m = std::min<std::size_t>(n, 8'000'000);  // cap AoS memory
        std::vector<Row> rows(m);
        const auto& cnv = t.column("conversion").i64;
        for (std::size_t i = 0; i < m; ++i) {
            rows[i].click = click[i];
            rows[i].cost  = cost[i];
            rows[i].conversion = cnv[i];      // present so the struct is a realistic full record
            rows[i].campaign = t.column("campaign").codes[i];
        }
        double sc = best_seconds(reps, [&] { g_sink_d = sum_cost_if_click_columnar(click.data(), cost.data(), m); });
        double sr = best_seconds(reps, [&] { g_sink_d = sum_cost_if_click_rowmajor(rows.data(), m); });
        std::printf("\n[4] SUM(cost) WHERE click==1 over %zu rows (sizeof(Row)=%zuB)\n", m, sizeof(Row));
        std::printf("    columnar (16 B/row touched) : %7.3f ms | %.2f GB/s\n", sc * 1e3, (m * 16.0) / 1e9 / sc);
        std::printf("    row-major (%zu B/row strided): %7.3f ms | %.2f GB/s\n", sizeof(Row), sr * 1e3,
                    (m * (double)sizeof(Row)) / 1e9 / sr);
        std::printf("    --> columnar is %.2fx faster (same query, cache-friendly layout)\n", sr / sc);
    }
    return 0;
}
