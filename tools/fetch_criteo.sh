#!/usr/bin/env bash
# Best-effort fetch of the real Criteo Attribution dataset.
# The official direct link currently 404s and the Kaggle mirror needs auth, so this may not
# succeed non-interactively — fall back to `tools/gen_synthetic.py` (see data/README.md).
set -euo pipefail
OUT_DIR="$(cd "$(dirname "$0")/.." && pwd)/data"
mkdir -p "$OUT_DIR"

CANDIDATES=(
  "http://go.criteo.net/criteo-research-attribution-dataset.zip"
  "https://go.criteo.net/criteo-research-attribution-dataset.zip"
)

for url in "${CANDIDATES[@]}"; do
  echo ">> trying $url"
  code=$(curl -sIL -o /dev/null -w "%{http_code}" --max-time 30 "$url" || echo 000)
  if [ "$code" = "200" ]; then
    echo ">> downloading…"
    curl -L --fail -o "$OUT_DIR/criteo_attribution_dataset.zip" "$url"
    ( cd "$OUT_DIR" && unzip -o criteo_attribution_dataset.zip && gunzip -f criteo_attribution_dataset.tsv.gz 2>/dev/null || true )
    echo ">> done. Rename/symlink to data/criteo_attribution.tsv if needed."
    exit 0
  fi
  echo "   HTTP $code — not available"
done

cat <<'EOF'
>> Could not fetch the dataset directly (official link 404s; Kaggle needs auth).
   Options:
     1) Kaggle CLI:
          pip install kaggle   # set up ~/.kaggle/kaggle.json
          kaggle datasets download -d sharatsachin/criteo-attribution-modeling -p data/ --unzip
     2) Synthetic (no download):
          python tools/gen_synthetic.py --rows 10_000_000 --out data/criteo_attribution.tsv --seed 7
EOF
exit 1
