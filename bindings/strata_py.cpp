// bindings/strata_py.cpp — pybind11 module exposing the engine as `import strata` (Phase 4).
//
// Design:
//  * A query is a small builder (PyQuery) that accumulates filters/group-by/threads and runs
//    a strata::Executor when an aggregation method is called.
//  * Results cross to Python as numpy arrays that *view* the C++ result vectors zero-copy
//    (the numpy `base` keeps the C++ result object alive). .to_arrow()/.to_pandas() wrap those
//    same arrays via pyarrow/pandas — still zero-copy for the numeric columns.
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cctype>
#include <memory>
#include <string>

#include "strata/executor.hpp"
#include "strata/parser.hpp"

namespace py = pybind11;
using namespace strata;

namespace {

DType dtype_from_str(const std::string& s) {
    if (s == "int" || s == "int64" || s == "i64") return DType::Int64;
    if (s == "float" || s == "float64" || s == "f64" || s == "double") return DType::Float64;
    if (s == "dict" || s == "str" || s == "category" || s == "categorical") return DType::Dict;
    throw std::invalid_argument("strata: unknown column type '" + s + "'");
}

Schema schema_from_obj(const py::object& obj) {
    if (obj.is_none()) return criteo_schema();
    Schema s;
    for (auto item : obj.cast<py::dict>())
        s.add(item.first.cast<std::string>(), dtype_from_str(item.second.cast<std::string>()));
    return s;
}

// ---- Result -----------------------------------------------------------------------
struct PyResult {
    QueryResult r;

    std::string metric_label() const {
        if (r.agg == Agg::Count) return "count";
        std::string a = agg_name(r.agg);  // "SUM" -> "sum_cost" (idiomatic column name)
        for (char& c : a) c = static_cast<char>(std::tolower(c));
        return a + "_" + r.metric_col;
    }
};

// numpy view over a std::vector<T> owned by a PyResult python object (zero-copy).
template <class T>
py::array_t<T> view(py::object owner, std::vector<T>& v) {
    return py::array_t<T>({static_cast<py::ssize_t>(v.size())}, {sizeof(T)}, v.data(), owner);
}

py::list keys_list(const PyResult& self) {
    py::list out;
    for (const auto& k : self.r.keys) out.append(py::str(k));
    return out;
}

// Build {name: array} preserving insertion order, for pandas/pyarrow.
py::dict result_columns(py::object self_obj) {
    PyResult& self = self_obj.cast<PyResult&>();
    py::dict cols;
    if (self.r.grouped) cols[py::str(self.r.group_col)] = keys_list(self);
    cols[py::str("count")] = view<std::int64_t>(self_obj, self.r.counts);
    // For COUNT the measure *is* the count (int64); avoid emitting a duplicate float column.
    if (self.r.agg != Agg::Count)
        cols[py::str(self.metric_label())] = view<double>(self_obj, self.r.values);
    return cols;
}

// ---- Query builder ----------------------------------------------------------------
struct PyQuery {
    std::shared_ptr<const Table> table;
    std::vector<Predicate>       preds;
    std::string                  group;
    std::int64_t                 bucket = 0;
    unsigned                     nthreads = 1;

    PyQuery with_eq(const std::string& col, const py::handle& val) const {
        PyQuery q = *this;
        const Column& c = table->column(col);
        if (c.type == DType::Dict)
            q.preds.push_back({col, Cmp::Eq, 0.0, py::str(val).cast<std::string>()});
        else
            q.preds.push_back({col, Cmp::Eq, val.cast<double>(), std::nullopt});
        return q;
    }

    PyQuery filter_kwargs(const py::kwargs& kw) const {
        PyQuery q = *this;
        for (auto item : kw) q = q.with_eq(item.first.cast<std::string>(), item.second);
        return q;
    }

    PyQuery filter_op(const std::string& col, const std::string& op, const py::handle& val) const {
        static const std::unordered_map<std::string, Cmp> ops = {
            {"==", Cmp::Eq}, {"eq", Cmp::Eq}, {"!=", Cmp::Ne}, {"ne", Cmp::Ne},
            {"<", Cmp::Lt}, {"lt", Cmp::Lt}, {"<=", Cmp::Le}, {"le", Cmp::Le},
            {">", Cmp::Gt}, {"gt", Cmp::Gt}, {">=", Cmp::Ge}, {"ge", Cmp::Ge}};
        auto it = ops.find(op);
        if (it == ops.end()) throw std::invalid_argument("strata: bad comparator '" + op + "'");
        PyQuery q = *this;
        const Column& c = table->column(col);
        if (c.type == DType::Dict)
            q.preds.push_back({col, it->second, 0.0, py::str(val).cast<std::string>()});
        else
            q.preds.push_back({col, it->second, val.cast<double>(), std::nullopt});
        return q;
    }

    PyQuery group_by(const std::string& col, std::int64_t b) const {
        PyQuery q = *this; q.group = col; q.bucket = b; return q;
    }
    PyQuery set_threads(unsigned n) const { PyQuery q = *this; q.nthreads = n ? n : 1; return q; }

    PyResult run(Agg agg, const std::string& metric) const {
        Executor ex(*table);
        for (const auto& p : preds) {
            if (p.str_value) ex.filter(p.column, p.cmp, std::string_view(*p.str_value));
            else ex.filter(p.column, p.cmp, p.value);
        }
        if (!group.empty()) ex.group_by(group, bucket);
        ex.threads(nthreads);
        PyResult out;
        out.r = ex.agg(agg, metric);
        return out;
    }
    PyResult count() const { return run(Agg::Count, ""); }
    PyResult sum(const std::string& c) const { return run(Agg::Sum, c); }
    PyResult avg(const std::string& c) const { return run(Agg::Avg, c); }
    PyResult min(const std::string& c) const { return run(Agg::Min, c); }
    PyResult max(const std::string& c) const { return run(Agg::Max, c); }
};

// ---- Table ------------------------------------------------------------------------
struct PyTable {
    std::shared_ptr<Table> table;

    PyQuery seed() const { return PyQuery{table, {}, "", 0, 1}; }
};

PyTable py_load(const std::string& path, const std::string& sep, py::object schema,
                std::size_t max_rows, bool verbose) {
    LoadOptions o;
    o.delimiter = sep.empty() ? '\t' : sep[0];
    o.max_rows = max_rows;
    o.verbose = verbose;
    auto t = std::make_shared<Table>(Table::from_csv(path, schema_from_obj(schema), o));
    return PyTable{std::move(t)};
}

}  // namespace

PYBIND11_MODULE(strata, m) {
    m.doc() = "Strata — a vectorized columnar OLAP engine (C++ core, Python front door).";

    py::enum_<Agg>(m, "Agg")
        .value("Count", Agg::Count).value("Sum", Agg::Sum).value("Avg", Agg::Avg)
        .value("Min", Agg::Min).value("Max", Agg::Max);

    m.def("load", &py_load, py::arg("path"), py::arg("sep") = "\t",
          py::arg("schema") = py::none(), py::arg("max_rows") = 0, py::arg("verbose") = false,
          "mmap + parse a CSV/TSV into a columnar table (default schema = Criteo Attribution).");

    py::class_<PyResult>(m, "Result")
        .def_property_readonly("grouped", [](const PyResult& r) { return r.r.grouped; })
        .def_property_readonly("group_col", [](const PyResult& r) { return r.r.group_col; })
        .def_property_readonly("agg", [](const PyResult& r) { return std::string(agg_name(r.r.agg)); })
        .def_property_readonly("seconds", [](const PyResult& r) { return r.r.seconds; })
        .def_property_readonly("rows_scanned", [](const PyResult& r) { return r.r.rows_scanned; })
        .def_property_readonly("gb_per_sec", [](const PyResult& r) { return r.r.gb_per_sec(); })
        .def_property_readonly("keys", [](const PyResult& r) { return keys_list(r); })
        .def_property_readonly("counts", [](py::object s) { return view<std::int64_t>(s, s.cast<PyResult&>().r.counts); })
        .def_property_readonly("values", [](py::object s) { return view<double>(s, s.cast<PyResult&>().r.values); })
        .def("sort_by_value_desc", [](PyResult& r) { r.r.sort_by_value_desc(); return &r; })
        .def("to_dict", &result_columns)
        .def("to_pandas", [](py::object s) {
            return py::module_::import("pandas").attr("DataFrame")(result_columns(s));
        })
        .def("to_arrow", [](py::object s) {
            // pyarrow wraps the numpy numeric arrays zero-copy.
            return py::module_::import("pyarrow").attr("table")(result_columns(s));
        })
        .def("__repr__", [](const PyResult& r) { return r.r.to_string(); });

    py::class_<PyQuery>(m, "Query")
        .def("filter", &PyQuery::filter_kwargs)
        .def("filter", &PyQuery::filter_op, py::arg("column"), py::arg("op"), py::arg("value"))
        .def("where", &PyQuery::filter_op, py::arg("column"), py::arg("op"), py::arg("value"))
        .def("group_by", &PyQuery::group_by, py::arg("column"), py::arg("bucket") = 0)
        .def("threads", &PyQuery::set_threads, py::arg("n"))
        .def("count", &PyQuery::count)
        .def("sum", &PyQuery::sum, py::arg("column"))
        .def("avg", &PyQuery::avg, py::arg("column"))
        .def("min", &PyQuery::min, py::arg("column"))
        .def("max", &PyQuery::max, py::arg("column"));

    py::class_<PyTable>(m, "Table")
        .def_property_readonly("num_rows", [](const PyTable& t) { return t.table->num_rows(); })
        .def_property_readonly("num_cols", [](const PyTable& t) { return t.table->num_cols(); })
        .def_property_readonly("columns", [](const PyTable& t) {
            py::list names;
            for (const auto& c : t.table->columns()) names.append(c.name);
            return names;
        })
        .def_property_readonly("resident_mb", [](const PyTable& t) { return t.table->resident_bytes() / 1e6; })
        .def_property_readonly("schema", [](const PyTable& t) {
            py::dict d;  // name -> "int64" | "float64" | "dict"
            for (const auto& c : t.table->columns()) d[py::str(c.name)] = std::string(dtype_name(c.type));
            return d;
        })
        .def("describe", [](const PyTable& t) { return Table_describe(*t.table); })
        .def("filter", [](const PyTable& t, const py::kwargs& kw) { return t.seed().filter_kwargs(kw); })
        .def("where", [](const PyTable& t, const std::string& c, const std::string& op, const py::handle& v) {
            return t.seed().filter_op(c, op, v); }, py::arg("column"), py::arg("op"), py::arg("value"))
        .def("group_by", [](const PyTable& t, const std::string& c, std::int64_t b) {
            return t.seed().group_by(c, b); }, py::arg("column"), py::arg("bucket") = 0)
        .def("threads", [](const PyTable& t, unsigned n) { return t.seed().set_threads(n); }, py::arg("n"))
        .def("count", [](const PyTable& t) { return t.seed().count(); })
        .def("sum", [](const PyTable& t, const std::string& c) { return t.seed().sum(c); }, py::arg("column"))
        .def("avg", [](const PyTable& t, const std::string& c) { return t.seed().avg(c); }, py::arg("column"))
        .def("min", [](const PyTable& t, const std::string& c) { return t.seed().min(c); }, py::arg("column"))
        .def("max", [](const PyTable& t, const std::string& c) { return t.seed().max(c); }, py::arg("column"))
        .def("__repr__", [](const PyTable& t) {
            return "<strata.Table " + std::to_string(t.table->num_rows()) + " rows x " +
                   std::to_string(t.table->num_cols()) + " cols>";
        });
}
