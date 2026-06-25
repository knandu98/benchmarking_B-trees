#!/usr/bin/env bash
# Build the prototype and run the micro-benchmark + correctness suite.
set -euo pipefail

cd "$(dirname "$0")/.."

mkdir -p results
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build --parallel >/dev/null

echo "== Correctness =="
ctest --test-dir build --output-on-failure

echo
echo "== Micro-benchmark =="
# Pass custom key counts as arguments, e.g. ./scripts/run.sh 1000 100000 1000000
./build/benchmark "$@"
