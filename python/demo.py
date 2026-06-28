#!/usr/bin/env python3
"""End-to-end Strata demo: Python query -> C++ scan -> Arrow -> result, with timings.

    python python/demo.py --data data/criteo_attribution.tsv [--check]

--check cross-validates a few aggregates against pandas.
"""
import argparse
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
for cand in (os.path.join(_HERE, "..", "build"), os.path.join(_HERE, "..")):
    if os.path.isdir(cand):
        sys.path.insert(0, os.path.abspath(cand))
import strata  # noqa: E402


def timed(label, fn):
    t0 = time.perf_counter()
    out = fn()
    dt = (time.perf_counter() - t0) * 1e3
    print(f"  {label:<48} {dt:8.2f} ms")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="data/criteo_attribution.tsv")
    ap.add_argument("--check", action="store_true", help="cross-check vs pandas")
    args = ap.parse_args()

    if not os.path.exists(args.data):
        sys.exit(f"missing {args.data}: generate with tools/gen_synthetic.py")

    print(f"== loading {args.data} ==")
    t0 = time.perf_counter()
    t = strata.load(args.data, sep="\t", verbose=True)
    load = time.perf_counter() - t0
    print(f"loaded {t.num_rows:,} rows x {t.num_cols} cols "
          f"({t.resident_mb:.0f} MB resident) in {load:.2f}s "
          f"({t.num_rows/load/1e6:.1f} Mrows/s)\n")

    total = t.count().values[0]
    print("== representative queries ==")
    clicks = timed("COUNT WHERE click==1", lambda: t.filter(click=1).count().values[0])
    convs = timed("COUNT WHERE conversion==1", lambda: t.filter(conversion=1).count().values[0])
    sum_cost = timed("SUM(cost)", lambda: t.sum("cost").values[0])
    avg_cost = timed("AVG(cost) WHERE click==1", lambda: t.filter(click=1).avg("cost").values[0])
    top = timed("GROUP BY campaign SUM(cost) WHERE click==1 [8 threads] -> Arrow",
                lambda: t.threads(8).filter(click=1).group_by("campaign").sum("cost").to_arrow())
    days = timed("GROUP BY day COUNT (timestamp bucket=86400)",
                 lambda: t.group_by("timestamp", 86400).count().to_arrow())

    print(f"\n  total impressions : {int(total):,}")
    print(f"  CTR               : {clicks/total*100:.3f}%  ({int(clicks):,} clicks)")
    print(f"  CVR               : {convs/total*100:.3f}%  ({int(convs):,} conversions)")
    print(f"  total spend       : {sum_cost:,.2f}")
    print(f"  avg cost / click  : {avg_cost:.5f}")

    tdf = top.to_pandas().sort_values("sum_cost", ascending=False).head(5)
    print("\n  top campaigns by spend (clicked):")
    for _, r in tdf.iterrows():
        print(f"    {r['campaign']:<12} spend={r['sum_cost']:.2f}  clicks={int(r['count'])}")
    print(f"\n  active days in window: {days.num_rows}")

    # thread scaling on one heavy query
    print("\n== thread scaling: GROUP BY campaign SUM(cost) WHERE click==1 ==")
    base = None
    for p in (1, 2, 4, 8):
        s = min(_run(t, p) for _ in range(3))
        base = base or s
        print(f"  threads={p}: {s*1e3:7.2f} ms  speedup {base/s:.2f}x")

    if args.check:
        print("\n== cross-check vs pandas ==")
        import pandas as pd
        df = pd.read_csv(args.data, sep="\t")
        checks = [
            ("COUNT(*)", int(total), len(df)),
            ("COUNT click==1", int(clicks), int((df.click == 1).sum())),
            ("COUNT conversion==1", int(convs), int((df.conversion == 1).sum())),
            ("SUM(cost) (int)", int(sum_cost), int(df.cost.sum())),
        ]
        ok = True
        for name, got, exp in checks:
            good = got == exp
            ok &= good
            print(f"  {name:<22} strata={got:<14} pandas={exp:<14} {'OK' if good else 'MISMATCH'}")
        print("ALL OK" if ok else "MISMATCH DETECTED")


def _run(t, p):
    t0 = time.perf_counter()
    t.threads(p).filter(click=1).group_by("campaign").sum("cost")
    return time.perf_counter() - t0


if __name__ == "__main__":
    main()
