#!/usr/bin/env python3
"""Strata dashboard — drive the C++ OLAP engine from Streamlit + Plotly.

Pick a metric and a dimension; the query runs in the C++ core (mmap'd columnar scan,
multi-threaded aggregation), returns Arrow zero-copy, and renders instantly. The query
time shown is the real C++ round-trip.

Run:
    pip install streamlit plotly       # if needed
    cmake --build build                # build the strata module
    streamlit run python/dashboard.py -- --data data/criteo_attribution.tsv
"""
import argparse
import os
import sys
import time

import plotly.express as px
import streamlit as st

# Make `import strata` work straight from the build tree (no install step needed).
_HERE = os.path.dirname(os.path.abspath(__file__))
for cand in (os.path.join(_HERE, "..", "build"), os.path.join(_HERE, "..")):
    if os.path.isdir(cand):
        sys.path.insert(0, os.path.abspath(cand))
import strata  # noqa: E402


def cli_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="data/criteo_attribution.tsv")
    # Streamlit passes its own args; ignore unknowns.
    args, _ = ap.parse_known_args()
    return args


@st.cache_resource(show_spinner="mmap + parse into columnar store…")
def load_table(path: str):
    t0 = time.perf_counter()
    t = strata.load(path, sep="\t")
    return t, time.perf_counter() - t0


st.set_page_config(page_title="Strata OLAP", layout="wide", page_icon="📊")
st.title("📊 Strata — columnar OLAP engine")
st.caption("C++ core · mmap'd columnar storage · vectorized + multithreaded aggregation · Arrow → Python")

args = cli_args()
data_path = st.sidebar.text_input("Dataset (TSV)", value=args.data)
if not os.path.exists(data_path):
    st.error(f"File not found: {data_path}\n\nGenerate one with:\n"
             f"`python tools/gen_synthetic.py --rows 5_000_000 --out {data_path}`")
    st.stop()

table, load_secs = load_table(data_path)
schema = dict(table.schema)
dict_cols = [c for c, ty in schema.items() if ty == "dict"]
num_cols = [c for c, ty in schema.items() if ty in ("int64", "float64")]

# Sidebar controls -----------------------------------------------------------------
st.sidebar.header("Query")
dimension = st.sidebar.selectbox("Group by (dimension)", dict_cols + ["timestamp (per day)"],
                                 index=0)
agg = st.sidebar.selectbox("Aggregation", ["COUNT", "SUM", "AVG", "MIN", "MAX"], index=0)
metric_col = None
if agg != "COUNT":
    metric_col = st.sidebar.selectbox("Measure column", [c for c in num_cols if c != "uid"],
                                      index=max(0, [c for c in num_cols].index("cost") if "cost" in num_cols else 0))
threads = st.sidebar.slider("Threads", 1, 8, 4)
topn = st.sidebar.slider("Top N", 5, 50, 15)

st.sidebar.subheader("Filter (optional)")
flt_col = st.sidebar.selectbox("Where column", ["(none)"] + list(schema.keys()))
flt_op = st.sidebar.selectbox("op", ["==", "!=", ">", ">=", "<", "<="], index=0)
flt_val = st.sidebar.text_input("value", value="1")

# Build + run query ----------------------------------------------------------------
q = table.threads(threads)
if flt_col != "(none)" and flt_val != "":
    val = flt_val if schema[flt_col] == "dict" else float(flt_val)
    q = q.where(flt_col, flt_op, val)

bucket = 0
group_field = dimension
if dimension.startswith("timestamp"):
    group_field, bucket = "timestamp", 86400  # one bucket per day
q = q.group_by(group_field, bucket)

t0 = time.perf_counter()
if agg == "COUNT":
    res = q.count()
elif agg == "SUM":
    res = q.sum(metric_col)
elif agg == "AVG":
    res = q.avg(metric_col)
elif agg == "MIN":
    res = q.min(metric_col)
else:
    res = q.max(metric_col)
arrow = res.to_arrow()
roundtrip_ms = (time.perf_counter() - t0) * 1e3

# KPIs -----------------------------------------------------------------------------
c1, c2, c3, c4 = st.columns(4)
c1.metric("Rows scanned", f"{res.rows_scanned:,}")
c2.metric("C++ query time", f"{res.seconds*1e3:.2f} ms")
c3.metric("Scan throughput", f"{res.gb_per_sec:.2f} GB/s")
c4.metric("Groups", f"{len(res.keys):,}")
st.caption(f"Loaded {table.num_rows:,} rows × {table.num_cols} cols "
           f"({table.resident_mb:.0f} MB resident) in {load_secs:.2f}s · "
           f"Python round-trip incl. Arrow: {roundtrip_ms:.2f} ms · {threads} threads")

# Chart + table --------------------------------------------------------------------
df = arrow.to_pandas()
value_col = "count" if agg == "COUNT" else [c for c in df.columns if c not in (group_field, "count")][0]
df = df.sort_values(value_col, ascending=False).head(topn)

label = f"{agg}({metric_col})" if metric_col else "COUNT(*)"
fig = px.bar(df, x=group_field, y=value_col, title=f"{label} by {dimension}",
             labels={value_col: label, group_field: dimension})
fig.update_layout(xaxis_type="category", height=460, margin=dict(l=10, r=10, t=50, b=10))
st.plotly_chart(fig, use_container_width=True)

with st.expander("Result table"):
    st.dataframe(df, use_container_width=True)
with st.expander("Schema"):
    st.code(table.describe())
