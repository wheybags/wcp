#!/bin/bash

set -euxo pipefail

base_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )/.."

cp "$base_dir/scripts/perf_regression_test.sh" "$base_dir/scripts/perf_regression_test2.sh"
cp "$base_dir/scripts/bench.sh" "$base_dir/scripts/bench2.sh"

for x in $(git log --format="%H" master | grep -v " Merge commit" | head -n 20 | tac); do
  git checkout "$x"
  git log --oneline -n 1
  "$base_dir/scripts/perf_regression_test2.sh" --csv --bench-script "$base_dir/scripts/bench2.sh" >> perf.csv
done

rm "$base_dir/scripts/perf_regression_test2.sh"
rm "$base_dir/scripts/bench2.sh"
