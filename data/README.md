# Datasets

Strata reads a tab-separated advertising event log with a header row. Two ways to get one:

## Option A — synthetic (no download, deterministic) ✅ recommended for dev/tests

```bash
# a small sample for tests / a quick look
python tools/gen_synthetic.py --rows 100000   --out data/sample_100k.tsv --seed 7

# a benchmark-scale file (near the real 16.5M-row dataset)
python tools/gen_synthetic.py --rows 10_000_000 --out data/criteo_attribution.tsv --seed 7
```

The generator emits the **exact Criteo Attribution schema** (same column names and types), so
the real file and a synthetic file are interchangeable. A fixed `--seed` makes aggregates
reproducible — which anonymized real data can't give you for exact-answer tests.

Schema (tab-separated, one row per impression):

| column | type | meaning |
|---|---|---|
| `timestamp` | int64 | impression time (seconds into the 30-day window) |
| `uid` | int64 | anonymized user id |
| `campaign` | dict | campaign id (group-by dimension) |
| `conversion` | int64 | 1 if a conversion followed within 30 days |
| `conversion_timestamp` | int64 | time of conversion, else `-1` |
| `conversion_id` | int64 | id to reconstruct timelines, else `-1` |
| `attribution` | int64 | 1 if the conversion was attributed to Criteo |
| `click` | int64 | 1 if the impression was clicked |
| `cost` | float64 | price paid for the display |
| `cpo` | float64 | cost-per-order, else `-1` |
| `time_since_last_click` | int64 | seconds since last click, else `-1` |
| `click_pos`, `click_nb` | int64 | click position / count |
| `cat1`..`cat9` | dict | contextual categorical features |

## Option B — the real Criteo dataset

~16.5M rows, 30 days of live Criteo traffic (45K conversions, 700 campaigns), CC BY-NC-SA 4.0.

- Source page: https://ailab.criteo.com/criteo-attribution-modeling-bidding-dataset/
- Kaggle mirror (needs a Kaggle account): https://www.kaggle.com/datasets/sharatsachin/criteo-attribution-modeling

> **Heads up:** the historical direct link `http://go.criteo.net/criteo-research-attribution-dataset.zip`
> currently returns **404**, and Kaggle requires authentication, so this can't be fetched
> non-interactively. `tools/fetch_criteo.sh` tries a few mirrors; if they fail, use the Kaggle
> CLI (`kaggle datasets download -d sharatsachin/criteo-attribution-modeling`) or Option A.

Once downloaded, decompress and drop it here as `data/criteo_attribution.tsv`. The file ships
with a header row, so it loads with no schema changes:

```bash
./build/strata describe data/criteo_attribution.tsv
python python/demo.py --data data/criteo_attribution.tsv --check
```

Data files in this directory are git-ignored (they're large) — see `.gitignore`.
