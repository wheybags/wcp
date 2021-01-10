#!/bin/bash

set -euxo pipefail

cp perf_regression_test.sh perf_regression_test2.sh
cp bench.sh bench2.sh

for x in $(git log --format="%H" master | grep -v " Merge commit" | head -n 20 | tac); do
  git checkout "$x"
  git log --oneline -n 1
  ./perf_regression_test2.sh --csv --bench-script ./bench2.sh >> perf.csv
done
