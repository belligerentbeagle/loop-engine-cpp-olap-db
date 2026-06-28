#!/usr/bin/env bash
# Configure + build everything, then print how to run it.
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build}"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j

echo
echo "built:"
echo "  $BUILD_DIR/strata                 CLI (describe / head / query / stream)"
echo "  $BUILD_DIR/strata_tests           correctness tests"
echo "  $BUILD_DIR/strata_bench[_o2|_o0]   benchmarks (O3 / O2 / O0)"
echo "  $BUILD_DIR/strata.*.so            Python module (import strata)"
echo
echo "next:"
echo "  (cd $BUILD_DIR && ctest --output-on-failure)"
echo "  python tools/gen_synthetic.py --rows 10_000_000 --out data/criteo_attribution.tsv"
echo "  ./$BUILD_DIR/strata query data/criteo_attribution.tsv --threads 8 --where click eq 1 --group campaign --sum cost"
echo "  python python/demo.py --data data/criteo_attribution.tsv --check"
echo "  streamlit run python/dashboard.py -- --data data/criteo_attribution.tsv"
