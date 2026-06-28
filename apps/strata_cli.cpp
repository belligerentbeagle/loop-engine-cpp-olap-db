// apps/strata_cli.cpp — a small driver to exercise the engine without Python.
//
//   strata describe FILE
//   strata head     FILE [N]
//   strata query    FILE [--threads N] [--where COL OP VAL]... [--group COL [--bucket W]]
//                        [--count | --sum COL | --avg COL | --min COL | --max COL] [--limit N]
//   strata stream   FILE [--threshold N] [--limit N]   (demo: query while ingesting)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "strata/executor.hpp"
#include "strata/memtable.hpp"
#include "strata/parser.hpp"

using namespace strata;

static void usage() {
    std::puts(
        "usage:\n"
        "  strata describe FILE\n"
        "  strata head     FILE [N]\n"
        "  strata query    FILE [--threads N] [--where COL OP VAL]... [--group COL [--bucket W]]\n"
        "                       [--count | --sum COL | --avg COL | --min COL | --max COL] [--limit N]\n"
        "  strata stream   FILE [--threshold N] [--limit N]\n"
        "\n  OP is one of: == != < <= > >=    (use 'eq ne lt le gt ge' to avoid shell quoting)\n");
}

static Cmp parse_cmp(const std::string& s) {
    if (s == "==" || s == "eq") return Cmp::Eq;
    if (s == "!=" || s == "ne") return Cmp::Ne;
    if (s == "<"  || s == "lt") return Cmp::Lt;
    if (s == "<=" || s == "le") return Cmp::Le;
    if (s == ">"  || s == "gt") return Cmp::Gt;
    if (s == ">=" || s == "ge") return Cmp::Ge;
    std::fprintf(stderr, "unknown comparator '%s'\n", s.c_str());
    std::exit(2);
}

static int cmd_describe(const char* path) {
    Table t = Table::from_criteo(path);
    std::fputs(Table_describe(t).c_str(), stdout);
    return 0;
}

static int cmd_head(const char* path, int n) {
    Table t = Table::from_criteo(path);
    std::fputs(Table_head(t, n).c_str(), stdout);
    return 0;
}

static int cmd_query(int argc, char** argv) {
    const char* path = argv[2];
    LoadOptions opt;
    opt.verbose = true;
    Table t = Table::from_csv(path, criteo_schema(), opt);

    Executor ex(t);
    Agg agg = Agg::Count;
    std::string metric, group;
    std::int64_t bucket = 0;
    std::size_t limit = 20;

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing arg for %s\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--threads") ex.threads((unsigned)std::atoi(need("--threads")));
        else if (a == "--where") {
            std::string col = need("--where COL");
            Cmp cmp = parse_cmp(need("--where OP"));
            std::string val = need("--where VAL");
            if (t.column(col).type == DType::Dict) ex.filter(col, cmp, std::string_view(val));
            else ex.filter(col, cmp, std::strtod(val.c_str(), nullptr));
        }
        else if (a == "--group") group = need("--group");
        else if (a == "--bucket") bucket = std::atoll(need("--bucket"));
        else if (a == "--count") agg = Agg::Count;
        else if (a == "--sum") { agg = Agg::Sum; metric = need("--sum"); }
        else if (a == "--avg") { agg = Agg::Avg; metric = need("--avg"); }
        else if (a == "--min") { agg = Agg::Min; metric = need("--min"); }
        else if (a == "--max") { agg = Agg::Max; metric = need("--max"); }
        else if (a == "--limit") limit = (std::size_t)std::atoll(need("--limit"));
        else { std::fprintf(stderr, "unknown flag '%s'\n", a.c_str()); usage(); return 2; }
    }
    if (!group.empty()) ex.group_by(group, bucket);

    QueryResult r = ex.agg(agg, metric);
    if (r.grouped) r.sort_by_value_desc();
    std::fputs(r.to_string(limit).c_str(), stdout);
    return 0;
}

// Demo: a producer thread streams rows from FILE into a StreamingTable while the main
// thread repeatedly runs COUNT(*) and COUNT WHERE click==1 — showing queries during ingest.
static int cmd_stream(int argc, char** argv) {
    const char* path = argv[2];
    std::size_t threshold = 10000, limit = 0;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--threshold" && i + 1 < argc) threshold = (std::size_t)std::atoll(argv[++i]);
        else if (a == "--limit" && i + 1 < argc) limit = (std::size_t)std::atoll(argv[++i]);
    }

    StreamingTable st(criteo_schema(), threshold);
    std::atomic<bool> done{false};

    std::thread producer([&] {
        std::ifstream in(path);
        std::string line;
        std::getline(in, line);  // skip header
        std::size_t n = 0;
        while (std::getline(in, line)) {
            st.insert(line);
            if (limit && ++n >= limit) break;
        }
        st.flush_sync();
        done.store(true);
    });

    const auto t0 = std::chrono::steady_clock::now();
    while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::size_t visible = st.visible_rows(), pending = st.pending_rows();
        long long clicks = st.query([](const Table& t) {
            if (t.num_rows() == 0) return 0LL;
            return (long long)Executor(t).filter_eq("click", 1).count().values[0];
        });
        std::printf("\r[stream] visible=%zu pending=%zu  click==1 so far=%lld   ", visible, pending, clicks);
        std::fflush(stdout);
    }
    producer.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\n[stream] done: %zu rows visible, %zu batches flushed in %.2fs (%.2f Mrows/s)\n",
                st.visible_rows(), st.batches_flushed(), secs, st.visible_rows() / secs / 1e6);
    std::fputs(st.query([](const Table& t) {
        auto r = Executor(t).group_by("campaign").count();
        r.sort_by_value_desc();
        return r.to_string(5);
    }).c_str(), stdout);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 1; }
    std::string cmd = argv[1];
    try {
        if (cmd == "describe") return cmd_describe(argv[2]);
        if (cmd == "head")     return cmd_head(argv[2], argc > 3 ? std::atoi(argv[3]) : 5);
        if (cmd == "query")    return cmd_query(argc, argv);
        if (cmd == "stream")   return cmd_stream(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    usage();
    return 1;
}
