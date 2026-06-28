// tests/test_strata.cpp — self-contained correctness tests (no external test framework).
//
// Covers: dictionary encoding, mmap parsing, every aggregation + GROUP BY against a tiny
// hand-computed fixture, single- vs multi-threaded equivalence, and the streaming MemTable.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "strata/executor.hpp"
#include "strata/memtable.hpp"
#include "strata/parser.hpp"
#include "strata/sql.hpp"

namespace fs = std::filesystem;
using namespace strata;

// ---- tiny test harness ------------------------------------------------------------
static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) { std::printf("  FAIL [%s:%d] %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)
#define CHECK_EQ(a, b)                                                                    \
    do {                                                                                  \
        ++g_checks;                                                                       \
        auto _a = (a); auto _b = (b);                                                     \
        if (!(_a == _b)) { std::printf("  FAIL [%s:%d] %s == %s (got %lld vs %lld)\n",    \
            __FILE__, __LINE__, #a, #b, (long long)_a, (long long)_b); ++g_failures; }    \
    } while (0)
#define CHECK_NEAR(a, b, eps)                                                             \
    do {                                                                                  \
        ++g_checks;                                                                       \
        double _a = (a), _b = (b);                                                        \
        if (std::fabs(_a - _b) > (eps)) { std::printf("  FAIL [%s:%d] %s ~= %s (got %.9g vs %.9g)\n", \
            __FILE__, __LINE__, #a, #b, _a, _b); ++g_failures; }                          \
    } while (0)

static Schema mini_schema() {
    return Schema{{"timestamp", DType::Int64}, {"campaign", DType::Dict},
                  {"click", DType::Int64},     {"conversion", DType::Int64},
                  {"cost", DType::Float64}};
}

// 6-row fixture with hand-computed expected answers (see comments in test_executor).
static const char* kFixture =
    "timestamp\tcampaign\tclick\tconversion\tcost\n"
    "10\tA\t1\t0\t1.0\n"
    "20\tA\t0\t0\t2.0\n"
    "30\tB\t1\t1\t3.0\n"
    "40\tB\t1\t0\t4.0\n"
    "50\tC\t0\t0\t5.0\n"
    "60\tA\t1\t1\t6.0\n";

static fs::path write_temp(const std::string& name, const std::string& contents) {
    fs::path p = fs::temp_directory_path() / name;
    std::ofstream(p) << contents;
    return p;
}

// ---- tests ------------------------------------------------------------------------
static void test_dictionary() {
    std::printf("test_dictionary\n");
    Dictionary d;
    CHECK_EQ(d.encode("alpha"), 0);
    CHECK_EQ(d.encode("beta"), 1);
    CHECK_EQ(d.encode("alpha"), 0);  // stable
    CHECK_EQ(d.size(), 2u);
    CHECK_EQ(d.lookup("beta"), 1);
    CHECK_EQ(d.lookup("missing"), -1);
    CHECK(d.decode(0) == "alpha");
}

static void test_parser() {
    std::printf("test_parser\n");
    auto p = write_temp("strata_fixture.tsv", kFixture);
    Table t = Table::from_csv(p.string(), mini_schema());
    CHECK_EQ(t.num_rows(), 6u);
    CHECK_EQ(t.num_cols(), 5u);
    CHECK_EQ(t.column("campaign").dict.size(), 3u);   // A,B,C
    CHECK_EQ(t.column("timestamp").i64[0], 10);
    CHECK_EQ(t.column("timestamp").i64[5], 60);
    CHECK_NEAR(t.column("cost").f64[2], 3.0, 1e-12);
    // A column present in file but absent from a narrower schema is skipped:
    Schema narrow{{"campaign", DType::Dict}, {"cost", DType::Float64}};
    Table t2 = Table::from_csv(p.string(), narrow);
    CHECK_EQ(t2.num_cols(), 2u);
    CHECK_EQ(t2.num_rows(), 6u);
    fs::remove(p);
}

static void test_executor() {
    std::printf("test_executor\n");
    auto p = write_temp("strata_fixture2.tsv", kFixture);
    Table t = Table::from_csv(p.string(), mini_schema());

    CHECK_EQ((long long)Executor(t).count().values[0], 6);
    CHECK_EQ((long long)Executor(t).filter_eq("click", 1).count().values[0], 4);
    CHECK_EQ((long long)Executor(t).filter_eq("conversion", 1).count().values[0], 2);
    CHECK_NEAR(Executor(t).agg(Agg::Sum, "cost").values[0], 21.0, 1e-12);
    CHECK_NEAR(Executor(t).filter_eq("click", 1).agg(Agg::Sum, "cost").values[0], 14.0, 1e-12);
    CHECK_NEAR(Executor(t).agg(Agg::Avg, "cost").values[0], 3.5, 1e-12);
    CHECK_NEAR(Executor(t).agg(Agg::Min, "cost").values[0], 1.0, 1e-12);
    CHECK_NEAR(Executor(t).agg(Agg::Max, "cost").values[0], 6.0, 1e-12);

    // filter with a string predicate on a dict column
    CHECK_EQ((long long)Executor(t).filter_eq("campaign", std::string_view("A")).count().values[0], 3);

    // GROUP BY campaign -> SUM(cost): A=9, B=7, C=5
    auto g = Executor(t).group_by("campaign").agg(Agg::Sum, "cost");
    std::map<std::string, double> got;
    for (std::size_t i = 0; i < g.keys.size(); ++i) got[g.keys[i]] = g.values[i];
    CHECK_NEAR(got["A"], 9.0, 1e-12);
    CHECK_NEAR(got["B"], 7.0, 1e-12);
    CHECK_NEAR(got["C"], 5.0, 1e-12);

    // GROUP BY campaign with filter click==1 -> A=7, B=7, C absent
    auto g2 = Executor(t).filter_eq("click", 1).group_by("campaign").agg(Agg::Sum, "cost");
    std::map<std::string, double> got2;
    for (std::size_t i = 0; i < g2.keys.size(); ++i) got2[g2.keys[i]] = g2.values[i];
    CHECK_NEAR(got2["A"], 7.0, 1e-12);
    CHECK_NEAR(got2["B"], 7.0, 1e-12);
    CHECK(got2.find("C") == got2.end());

    // GROUP BY a numeric column (click): groups 0 and 1
    auto g3 = Executor(t).group_by("click").count();
    std::map<long long, long long> got3;
    for (std::size_t i = 0; i < g3.keys_num.size(); ++i) got3[g3.keys_num[i]] = g3.counts[i];
    CHECK_EQ(got3[0], 2LL);
    CHECK_EQ(got3[1], 4LL);
    fs::remove(p);
}

// Build a deterministic in-memory table and check single- vs multi-threaded agreement.
static void test_parallel_equiv() {
    std::printf("test_parallel_equiv\n");
    Table t;
    t.add_column("g", DType::Dict);
    t.add_column("x", DType::Int64);
    Column& g = t.column("g");   // fetch *after* all columns exist (add_column may realloc)
    Column& x = t.column("x");
    const int N = 200000, K = 37;
    std::uint64_t s = 88172645463325252ULL;
    double expect_sum = 0;
    std::map<int, double> expect_group;
    for (int i = 0; i < N; ++i) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;          // xorshift
        int k = int(s % K);
        long long v = int(s % 100);
        g.codes.push_back(g.dict.encode("g" + std::to_string(k)));
        x.i64.push_back(v);
        expect_sum += v;
        expect_group[k] += v;
    }
    t.set_num_rows(N);

    CHECK_NEAR(Executor(t).threads(1).agg(Agg::Sum, "x").values[0], expect_sum, 1e-6);
    CHECK_NEAR(Executor(t).threads(8).agg(Agg::Sum, "x").values[0], expect_sum, 1e-6);

    auto r1 = Executor(t).threads(1).group_by("g").agg(Agg::Sum, "x");
    auto r8 = Executor(t).threads(8).group_by("g").agg(Agg::Sum, "x");
    CHECK_EQ(r1.keys.size(), r8.keys.size());
    std::map<std::string, double> m1, m8;
    for (std::size_t i = 0; i < r1.keys.size(); ++i) m1[r1.keys[i]] = r1.values[i];
    for (std::size_t i = 0; i < r8.keys.size(); ++i) m8[r8.keys[i]] = r8.values[i];
    bool match = true;
    for (auto& [k, _] : expect_group) {
        if (std::fabs(m1["g" + std::to_string(k)] - expect_group[k]) > 1e-6) match = false;
        if (std::fabs(m8["g" + std::to_string(k)] - expect_group[k]) > 1e-6) match = false;
    }
    CHECK(match);
}

static void test_streaming() {
    std::printf("test_streaming\n");
    // Stream the fixture rows through the MemTable and verify they become queryable.
    StreamingTable st(mini_schema(), /*flush_threshold=*/2);
    const char* rows[] = {"10\tA\t1\t0\t1.0", "20\tA\t0\t0\t2.0", "30\tB\t1\t1\t3.0",
                          "40\tB\t1\t0\t4.0", "50\tC\t0\t0\t5.0", "60\tA\t1\t1\t6.0"};
    for (const char* r : rows) st.insert(r);
    st.flush_sync();
    CHECK_EQ(st.visible_rows(), 6u);
    CHECK_EQ(st.pending_rows(), 0u);

    // A query during/after ingest sees the same answers as the bulk path.
    long long cnt_click = st.query([](const Table& t) {
        return (long long)Executor(t).filter_eq("click", 1).count().values[0];
    });
    double sum_cost = st.query([](const Table& t) {
        return Executor(t).agg(Agg::Sum, "cost").values[0];
    });
    CHECK_EQ(cnt_click, 4LL);
    CHECK_NEAR(sum_cost, 21.0, 1e-12);
}

static void test_sql() {
    std::printf("test_sql\n");
    auto p = write_temp("strata_fixture_sql.tsv", kFixture);
    Table t = Table::from_csv(p.string(), mini_schema());

    CHECK_EQ((long long)run_sql(t, "SELECT COUNT(*) FROM t").values[0], 6);
    CHECK_EQ((long long)run_sql(t, "select count(*) from t where click = 1").values[0], 4);
    CHECK_NEAR(run_sql(t, "SELECT SUM(cost) FROM t WHERE click = 1").values[0], 14.0, 1e-12);
    CHECK_NEAR(run_sql(t, "SELECT AVG(cost) FROM t").values[0], 3.5, 1e-12);
    CHECK_EQ((long long)run_sql(t, "SELECT COUNT(*) FROM t WHERE campaign = 'A'").values[0], 3);
    CHECK_EQ((long long)run_sql(t, "SELECT COUNT(*) FROM t WHERE campaign = A").values[0], 3);  // unquoted ok

    // GROUP BY + ORDER BY DESC + LIMIT
    auto g = run_sql(t, "SELECT campaign, SUM(cost) FROM t GROUP BY campaign");
    std::map<std::string, double> got;
    for (std::size_t i = 0; i < g.keys.size(); ++i) got[g.keys[i]] = g.values[i];
    CHECK_NEAR(got["A"], 9.0, 1e-12);
    CHECK_NEAR(got["B"], 7.0, 1e-12);
    CHECK_NEAR(got["C"], 5.0, 1e-12);

    auto lim = run_sql(t, "SELECT campaign, SUM(cost) FROM t GROUP BY campaign ORDER BY 2 DESC LIMIT 2");
    CHECK_EQ(lim.keys.size(), 2u);                       // LIMIT truncates
    CHECK(lim.values[0] >= lim.values[1]);               // ORDER BY ... DESC
    CHECK_NEAR(lim.values[0], 9.0, 1e-12);               // A is the largest

    // error handling
    int threw = 0;
    try { run_sql(t, "SELECT * FROM t"); } catch (const std::exception&) { ++threw; }
    try { run_sql(t, "SELECT COUNT(*) FROM t WHERE nope = 1"); } catch (const std::exception&) { ++threw; }
    try { run_sql(t, "SELECT SUM(cost) FROM t WHERE a=1 OR b=2"); } catch (const std::exception&) { ++threw; }
    CHECK_EQ(threw, 3);
    fs::remove(p);
}

int main() {
    std::printf("=== strata tests ===\n");
    test_dictionary();
    test_parser();
    test_executor();
    test_parallel_equiv();
    test_streaming();
    test_sql();
    std::printf("=== %d checks, %d failures ===\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
