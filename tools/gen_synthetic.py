#!/usr/bin/env python3
"""Generate a synthetic advertising event log matching the Criteo Attribution schema.

The real Criteo "Attribution Modeling for Bidding" dataset (~16.5M rows, 623 MB gz) lives
behind a terms-gate / Kaggle auth and its public direct link currently 404s. This script
produces a tab-separated file with the *identical column names and types* so the engine,
benchmarks and dashboard run end-to-end at any scale you choose -- and, with a fixed seed,
deterministically, which anonymized real data can't give you for exact-aggregate tests.

Columns (tab-separated, header row):
  timestamp uid campaign conversion conversion_timestamp conversion_id attribution
  click cost cpo time_since_last_click click_pos click_nb cat1..cat9

Usage:
  python3 tools/gen_synthetic.py --rows 5_000_000 --out data/criteo_attribution.tsv --seed 7
"""
import argparse
import os
import sys
import time

import numpy as np
import pandas as pd

# Per-impression base rates roughly echo the published dataset
# (45K conversions over 16.5M impressions ~= 0.27%).
CONV_RATE = 0.0027
CLICK_RATE = 0.05
ATTRIB_GIVEN_CONV = 0.55
WINDOW_SECONDS = 30 * 24 * 3600  # 30-day sample

# cat1..cat9 cardinalities (mix of low- and high-cardinality contextual features).
CAT_CARD = [12, 45, 120, 8, 300, 1500, 60, 25, 5000]

COLUMNS = ([
    "timestamp", "uid", "campaign", "conversion", "conversion_timestamp", "conversion_id",
    "attribution", "click", "cost", "cpo", "time_since_last_click", "click_pos", "click_nb",
] + [f"cat{i}" for i in range(1, 10)])


def zipf_choice(rng, n, size, a=1.3):
    """Draw `size` integer ids in [0, n) with a Zipf-ish skew (a few hot values)."""
    return ((rng.zipf(a, size=size) - 1) % n).astype(np.int32)


def make_chunk(rng, n, conv_id_base, n_campaigns):
    timestamp = np.sort(rng.integers(0, WINDOW_SECONDS, size=n)).astype(np.int64)
    uid = rng.integers(0, 5_000_000, size=n, dtype=np.int64)
    campaign = zipf_choice(rng, n_campaigns, n)

    conversion = (rng.random(n) < CONV_RATE).astype(np.int64)
    click = (rng.random(n) < CLICK_RATE).astype(np.int64)
    attribution = (conversion & (rng.random(n) < ATTRIB_GIVEN_CONV).astype(np.int64))

    conv_delay = rng.integers(60, 14 * 24 * 3600, size=n, dtype=np.int64)
    conversion_timestamp = np.where(conversion == 1, timestamp + conv_delay, -1)
    conv_ids = np.full(n, -1, dtype=np.int64)
    n_conv = int(conversion.sum())
    conv_ids[conversion == 1] = conv_id_base + np.arange(n_conv, dtype=np.int64)

    cost = np.round(rng.lognormal(-2.0, 0.6, size=n), 6)
    cpo = np.where(conversion == 1, np.round(rng.lognormal(2.5, 0.7, size=n), 4), -1.0)

    click_nb = np.where(click == 1, rng.integers(1, 6, size=n), 0).astype(np.int64)
    click_pos = np.where(click == 1, rng.integers(0, np.maximum(click_nb, 1)), -1).astype(np.int64)
    tslc = np.where(click == 1, rng.integers(0, 7 * 24 * 3600, size=n), -1).astype(np.int64)

    data = {
        "timestamp": timestamp,
        "uid": uid,
        "campaign": np.char.add("camp_", campaign.astype(str)),  # readable categorical
        "conversion": conversion,
        "conversion_timestamp": conversion_timestamp,
        "conversion_id": conv_ids,
        "attribution": attribution,
        "click": click,
        "cost": cost,
        "cpo": cpo,
        "time_since_last_click": tslc,
        "click_pos": click_pos,
        "click_nb": click_nb,
    }
    for i in range(9):
        data[f"cat{i+1}"] = zipf_choice(rng, CAT_CARD[i], n)
    return pd.DataFrame(data, columns=COLUMNS), n_conv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", type=lambda s: int(s.replace("_", "")), default=2_000_000)
    ap.add_argument("--out", default="data/criteo_attribution.tsv")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--campaigns", type=int, default=700)
    ap.add_argument("--chunk", type=lambda s: int(s.replace("_", "")), default=1_000_000)
    args = ap.parse_args()

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    rng = np.random.default_rng(args.seed)
    t0 = time.time()
    written, conv_id_base = 0, 0

    with open(args.out, "w", buffering=1 << 22) as f:
        while written < args.rows:
            n = min(args.chunk, args.rows - written)
            df, n_conv = make_chunk(rng, n, conv_id_base, args.campaigns)
            df.to_csv(f, sep="\t", header=(written == 0), index=False, float_format="%.6g",
                      lineterminator="\n")
            written += n
            conv_id_base += n_conv
            print(f"  {written:,}/{args.rows:,} rows ({100*written/args.rows:4.1f}%)", file=sys.stderr)

    dt = time.time() - t0
    mb = os.path.getsize(args.out) / 1e6
    print(f"wrote {args.rows:,} rows to {args.out} ({mb:.0f} MB) in {dt:.1f}s "
          f"({args.rows/dt/1e6:.2f} Mrows/s)", file=sys.stderr)


if __name__ == "__main__":
    main()
