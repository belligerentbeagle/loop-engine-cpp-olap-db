// strata/executor.cpp — vectorized filter + aggregate + GROUP BY (Phase 2).
//
// The hot loops are written branchless and column-at-a-time so -O3 -march=native turns
// them into SIMD. The reduction kernels are range-based ([lo,hi)) so Phase 3 can run them
// on a thread pool and merge partials with no change to the logic here.
#include "strata/executor.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

#include "strata/threadpool.hpp"

namespace strata {

const char* agg_name(Agg a) {
    switch (a) {
        case Agg::Count: return "COUNT";
        case Agg::Sum:   return "SUM";
        case Agg::Avg:   return "AVG";
        case Agg::Min:   return "MIN";
        case Agg::Max:   return "MAX";
    }
    return "?";
}
const char* cmp_name(Cmp c) {
    switch (c) {
        case Cmp::Eq: return "==";
        case Cmp::Ne: return "!=";
        case Cmp::Lt: return "<";
        case Cmp::Le: return "<=";
        case Cmp::Gt: return ">";
        case Cmp::Ge: return ">=";
    }
    return "?";
}

// ---- builder ----------------------------------------------------------------------
Executor& Executor::filter(const std::string& column, Cmp cmp, double value) {
    preds_.push_back({column, cmp, value, std::nullopt});
    return *this;
}
Executor& Executor::filter(const std::string& column, Cmp cmp, std::string_view value) {
    preds_.push_back({column, cmp, 0.0, std::string(value)});
    return *this;
}
Executor& Executor::group_by(const std::string& column, std::int64_t bucket) {
    group_col_ = column;
    group_bucket_ = bucket;
    return *this;
}

// ---- predicate evaluation ---------------------------------------------------------
namespace {

// AND one predicate into `mask` over [lo,hi). Branchless; vectorizes for the numeric paths.
template <Cmp C, class T>
inline void and_cmp(std::uint8_t* mask, const T* data, double v, std::size_t lo, std::size_t hi) {
    for (std::size_t i = lo; i < hi; ++i) {
        bool keep;
        if constexpr (C == Cmp::Eq) keep = (double(data[i]) == v);
        else if constexpr (C == Cmp::Ne) keep = (double(data[i]) != v);
        else if constexpr (C == Cmp::Lt) keep = (double(data[i]) < v);
        else if constexpr (C == Cmp::Le) keep = (double(data[i]) <= v);
        else if constexpr (C == Cmp::Gt) keep = (double(data[i]) > v);
        else                              keep = (double(data[i]) >= v);
        mask[i] &= static_cast<std::uint8_t>(keep);
    }
}

template <class T>
inline void apply_numeric(std::uint8_t* mask, const T* data, Cmp c, double v, std::size_t lo, std::size_t hi) {
    switch (c) {
        case Cmp::Eq: and_cmp<Cmp::Eq>(mask, data, v, lo, hi); break;
        case Cmp::Ne: and_cmp<Cmp::Ne>(mask, data, v, lo, hi); break;
        case Cmp::Lt: and_cmp<Cmp::Lt>(mask, data, v, lo, hi); break;
        case Cmp::Le: and_cmp<Cmp::Le>(mask, data, v, lo, hi); break;
        case Cmp::Gt: and_cmp<Cmp::Gt>(mask, data, v, lo, hi); break;
        case Cmp::Ge: and_cmp<Cmp::Ge>(mask, data, v, lo, hi); break;
    }
}

inline void apply_predicate(const Column& col, const Predicate& p, std::uint8_t* mask,
                            std::size_t lo, std::size_t hi) {
    if (col.type == DType::Dict) {
        const std::int32_t code = p.str_value ? col.dict.lookup(*p.str_value)
                                              : static_cast<std::int32_t>(p.value);
        const std::int32_t* c = col.codes.data();
        // Only Eq/Ne are meaningful on a categorical; map others onto code order if asked.
        for (std::size_t i = lo; i < hi; ++i) {
            bool keep;
            switch (p.cmp) {
                case Cmp::Eq: keep = (c[i] == code); break;
                case Cmp::Ne: keep = (c[i] != code); break;
                case Cmp::Lt: keep = (c[i] <  code); break;
                case Cmp::Le: keep = (c[i] <= code); break;
                case Cmp::Gt: keep = (c[i] >  code); break;
                default:      keep = (c[i] >= code); break;
            }
            mask[i] &= static_cast<std::uint8_t>(keep);
        }
        return;
    }
    if (col.type == DType::Int64) apply_numeric(mask, col.i64.data(), p.cmp, p.value, lo, hi);
    else                          apply_numeric(mask, col.f64.data(), p.cmp, p.value, lo, hi);
}

// A cheap, type-erased view of the measure column read as double.
struct MetricView {
    const std::int64_t* i = nullptr;
    const double*       f = nullptr;
    inline double operator[](std::size_t k) const { return f ? f[k] : double(i[k]); }
    explicit operator bool() const { return i || f; }
};

constexpr double NEG_INF = -std::numeric_limits<double>::infinity();
constexpr double POS_INF =  std::numeric_limits<double>::infinity();

struct GlobalAcc {
    std::int64_t count = 0;
    double sum = 0.0;
    double mn = POS_INF;
    double mx = NEG_INF;
    void merge(const GlobalAcc& o) {
        count += o.count;
        sum += o.sum;
        mn = std::min(mn, o.mn);
        mx = std::max(mx, o.mx);
    }
};

// Ungrouped reduction over [lo,hi). `mask==nullptr` means "all rows pass" (no filter),
// which keeps the SUM loop a clean contiguous reduction the compiler will vectorize.
GlobalAcc reduce_global(std::size_t lo, std::size_t hi, const std::uint8_t* mask,
                        MetricView m, Agg agg) {
    GlobalAcc a;
    if (!mask) {
        a.count = std::int64_t(hi - lo);
        if (agg == Agg::Sum || agg == Agg::Avg) {
            // 4 accumulators break the FP-add dependency chain so the reduction vectorizes.
            double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
            std::size_t i = lo;
            for (; i + 4 <= hi; i += 4) { s0 += m[i]; s1 += m[i + 1]; s2 += m[i + 2]; s3 += m[i + 3]; }
            double s = s0 + s1 + s2 + s3;
            for (; i < hi; ++i) s += m[i];
            a.sum = s;
        } else if (agg == Agg::Min || agg == Agg::Max) {
            double mn = POS_INF, mx = NEG_INF;
            for (std::size_t i = lo; i < hi; ++i) { double v = m[i]; mn = std::min(mn, v); mx = std::max(mx, v); }
            a.mn = mn; a.mx = mx;
        }
        return a;
    }
    if (agg == Agg::Count) {
        std::int64_t c = 0;
        for (std::size_t i = lo; i < hi; ++i) c += mask[i];     // branchless popcount-ish
        a.count = c;
    } else if (agg == Agg::Sum || agg == Agg::Avg) {
        std::int64_t c = 0;
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0;       // branchless masked sum, 4-way unrolled
        std::size_t i = lo;
        for (; i + 4 <= hi; i += 4) {
            c += mask[i] + mask[i + 1] + mask[i + 2] + mask[i + 3];
            s0 += mask[i] * m[i];         s1 += mask[i + 1] * m[i + 1];
            s2 += mask[i + 2] * m[i + 2]; s3 += mask[i + 3] * m[i + 3];
        }
        double s = s0 + s1 + s2 + s3;
        for (; i < hi; ++i) { c += mask[i]; s += mask[i] * m[i]; }
        a.count = c; a.sum = s;
    } else {  // Min / Max
        std::int64_t c = 0; double mn = POS_INF, mx = NEG_INF;
        for (std::size_t i = lo; i < hi; ++i) {
            if (mask[i]) { double v = m[i]; mn = std::min(mn, v); mx = std::max(mx, v); ++c; }
        }
        a.count = c; a.mn = mn; a.mx = mx;
    }
    return a;
}

struct GroupAcc {
    std::vector<std::int64_t> count;
    std::vector<double>       sum;
    std::vector<double>       mn;
    std::vector<double>       mx;
    void init(std::size_t k, Agg agg) {
        count.assign(k, 0);
        sum.assign(k, 0.0);
        if (agg == Agg::Min) mn.assign(k, POS_INF);
        if (agg == Agg::Max) mx.assign(k, NEG_INF);
    }
    void merge(const GroupAcc& o, Agg agg) {
        for (std::size_t g = 0; g < count.size(); ++g) {
            count[g] += o.count[g];
            sum[g] += o.sum[g];
            if (agg == Agg::Min) mn[g] = std::min(mn[g], o.mn[g]);
            if (agg == Agg::Max) mx[g] = std::max(mx[g], o.mx[g]);
        }
    }
};

// Grouped reduction over [lo,hi). `key[i]` is the dense group index of row i.
// The scatter into per-group accumulators inherently breaks vectorization, so a per-row
// branch on the mask is fine here.
void reduce_group(std::size_t lo, std::size_t hi, const std::uint8_t* mask,
                  const std::int32_t* key, MetricView m, Agg agg, GroupAcc& a) {
    for (std::size_t i = lo; i < hi; ++i) {
        if (mask && !mask[i]) continue;
        const std::int32_t g = key[i];
        a.count[g] += 1;
        if (agg == Agg::Sum || agg == Agg::Avg) a.sum[g] += m[i];
        else if (agg == Agg::Min) a.mn[g] = std::min(a.mn[g], m[i]);
        else if (agg == Agg::Max) a.mx[g] = std::max(a.mx[g], m[i]);
    }
}

}  // namespace

// ---- main entry -------------------------------------------------------------------
QueryResult Executor::agg(Agg agg, const std::string& metric_col) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const std::size_t n = table_.num_rows();

    // Resolve the measure column (not needed for COUNT).
    MetricView metric;
    std::size_t metric_bytes = 0;
    if (agg != Agg::Count) {
        if (metric_col.empty())
            throw std::invalid_argument("strata: agg requires a metric column");
        const Column& mc = table_.column(metric_col);
        if (mc.type == DType::Int64)      { metric.i = mc.i64.data(); metric_bytes = n * 8; }
        else if (mc.type == DType::Float64){ metric.f = mc.f64.data(); metric_bytes = n * 8; }
        else throw std::invalid_argument("strata: cannot aggregate a dict column '" + metric_col + "'");
    }

    ThreadPool* pool = (threads_ > 1) ? &ThreadPool::shared(threads_) : nullptr;

    // Build the filter mask (skip entirely when there are no predicates). The mask passes are
    // row-independent, so split them across the pool too — otherwise this serial pass would
    // cap thread scaling on filtered queries.
    std::vector<std::uint8_t> mask;
    const std::uint8_t* maskp = nullptr;
    std::size_t pred_bytes = 0;
    if (!preds_.empty()) {
        mask.assign(n, 1);
        maskp = mask.data();
        auto apply_range = [&](std::size_t lo, std::size_t hi) {
            for (const auto& p : preds_) apply_predicate(table_.column(p.column), p, mask.data(), lo, hi);
        };
        if (pool) pool->for_ranges(n, apply_range);
        else apply_range(0, n);
        pred_bytes = preds_.size() * n * 8;  // ~bytes touched across predicates
    }

    QueryResult r;
    r.agg = agg;
    r.metric_col = metric_col;
    r.rows_scanned = n;
    r.bytes_scanned = metric_bytes + pred_bytes + (maskp ? n : 0);

    if (group_col_.empty()) {
        // ---- ungrouped ----
        GlobalAcc acc;
        if (pool) {
            acc = pool->map_reduce<GlobalAcc>(
                n, [&](std::size_t lo, std::size_t hi) { return reduce_global(lo, hi, maskp, metric, agg); },
                [](GlobalAcc& a, const GlobalAcc& b) { a.merge(b); });
        } else {
            acc = reduce_global(0, n, maskp, metric, agg);
        }
        r.grouped = false;
        r.counts.push_back(acc.count);
        switch (agg) {
            case Agg::Count: r.values.push_back(double(acc.count)); break;
            case Agg::Sum:   r.values.push_back(acc.sum); break;
            case Agg::Avg:   r.values.push_back(acc.count ? acc.sum / acc.count : 0.0); break;
            case Agg::Min:   r.values.push_back(acc.count ? acc.mn : 0.0); break;
            case Agg::Max:   r.values.push_back(acc.count ? acc.mx : 0.0); break;
        }
    } else {
        // ---- grouped ----
        const Column& gc = table_.column(group_col_);
        r.grouped = true;
        r.group_col = group_col_;

        // Derive a dense int32 group key per row. Dict columns are already dense.
        std::size_t K = 0;
        const std::int32_t* keyp = nullptr;
        std::vector<std::int32_t> key_storage;
        std::vector<std::int64_t> key_decode;   // group index -> raw numeric value (numeric grouping)

        if (gc.type == DType::Dict && group_bucket_ == 0) {
            K = gc.dict.size();
            keyp = gc.codes.data();
            r.bytes_scanned += n * 4;
        } else {
            // Numeric (optionally bucketed) grouping: densify values via a hash map.
            std::unordered_map<std::int64_t, std::int32_t> dense;
            key_storage.resize(n);
            auto raw_at = [&](std::size_t i) -> std::int64_t {
                std::int64_t v = (gc.type == DType::Int64) ? gc.i64[i]
                                 : (gc.type == DType::Float64) ? std::int64_t(gc.f64[i])
                                 : gc.codes[i];
                return group_bucket_ > 0 ? (v / group_bucket_) : v;
            };
            for (std::size_t i = 0; i < n; ++i) {
                std::int64_t v = raw_at(i);
                auto it = dense.find(v);
                std::int32_t g;
                if (it == dense.end()) { g = std::int32_t(key_decode.size()); dense.emplace(v, g); key_decode.push_back(v); }
                else g = it->second;
                key_storage[i] = g;
            }
            K = key_decode.size();
            keyp = key_storage.data();
            r.bytes_scanned += n * 8;
        }

        GroupAcc acc;
        acc.init(K, agg);
        if (pool) {
            acc = pool->map_reduce<GroupAcc>(
                n,
                [&](std::size_t lo, std::size_t hi) {
                    GroupAcc local; local.init(K, agg);
                    reduce_group(lo, hi, maskp, keyp, metric, agg, local);
                    return local;
                },
                [&](GroupAcc& a, const GroupAcc& b) { a.merge(b, agg); });
        } else {
            reduce_group(0, n, maskp, keyp, metric, agg, acc);
        }

        // Emit only non-empty groups.
        for (std::size_t g = 0; g < K; ++g) {
            if (acc.count[g] == 0) continue;
            if (gc.type == DType::Dict && group_bucket_ == 0)
                r.keys.push_back(gc.dict.decode(std::int32_t(g)));
            else
                r.keys.push_back(std::to_string(key_decode[g] * (group_bucket_ > 0 ? group_bucket_ : 1)));
            r.keys_num.push_back(key_decode.empty() ? std::int64_t(g)
                                                    : key_decode[g] * (group_bucket_ > 0 ? group_bucket_ : 1));
            r.counts.push_back(acc.count[g]);
            switch (agg) {
                case Agg::Count: r.values.push_back(double(acc.count[g])); break;
                case Agg::Sum:   r.values.push_back(acc.sum[g]); break;
                case Agg::Avg:   r.values.push_back(acc.sum[g] / acc.count[g]); break;
                case Agg::Min:   r.values.push_back(acc.mn[g]); break;
                case Agg::Max:   r.values.push_back(acc.mx[g]); break;
            }
        }
    }

    r.seconds = std::chrono::duration<double>(clock::now() - t0).count();
    return r;
}

// ---- result helpers ---------------------------------------------------------------
void QueryResult::sort_by_value(bool descending) {
    std::vector<std::size_t> idx(values.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
        return descending ? values[a] > values[b] : values[a] < values[b];
    });
    auto reorder_d = [&](std::vector<double>& v) { std::vector<double> t(v.size()); for (std::size_t i=0;i<idx.size();++i) t[i]=v[idx[i]]; v.swap(t); };
    auto reorder_i = [&](std::vector<std::int64_t>& v) { if(v.empty())return; std::vector<std::int64_t> t(v.size()); for (std::size_t i=0;i<idx.size();++i) t[i]=v[idx[i]]; v.swap(t); };
    auto reorder_s = [&](std::vector<std::string>& v) { if(v.empty())return; std::vector<std::string> t(v.size()); for (std::size_t i=0;i<idx.size();++i) t[i]=v[idx[i]]; v.swap(t); };
    reorder_d(values);
    reorder_i(counts);
    reorder_i(keys_num);
    reorder_s(keys);
}

std::string QueryResult::to_string(std::size_t max_rows) const {
    char buf[256];
    std::string out;
    if (!grouped) {
        std::snprintf(buf, sizeof(buf), "%s(%s) = %.6g   [%lld rows matched]\n",
                      agg_name(agg), metric_col.empty() ? "*" : metric_col.c_str(),
                      values.empty() ? 0.0 : values[0],
                      counts.empty() ? 0LL : (long long)counts[0]);
        out += buf;
    } else {
        std::snprintf(buf, sizeof(buf), "%-24s %12s %14s\n", group_col.c_str(), "count",
                      (std::string(agg_name(agg)) + "(" + (metric_col.empty() ? "*" : metric_col) + ")").c_str());
        out += buf;
        const std::size_t rows = std::min(max_rows, keys.size());
        for (std::size_t i = 0; i < rows; ++i) {
            std::snprintf(buf, sizeof(buf), "%-24s %12lld %14.6g\n", keys[i].c_str(),
                          (long long)counts[i], values[i]);
            out += buf;
        }
        if (keys.size() > rows) {
            std::snprintf(buf, sizeof(buf), "... (%zu more groups)\n", keys.size() - rows);
            out += buf;
        }
    }
    std::snprintf(buf, sizeof(buf), "scanned %zu rows in %.3f ms (%.2f Grows/s, %.2f GB/s)\n",
                  rows_scanned, seconds * 1e3, rows_per_sec() / 1e9, gb_per_sec());
    out += buf;
    return out;
}

}  // namespace strata
