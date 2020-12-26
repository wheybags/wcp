#!/bin/bash

set -euo pipefail

base_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
old_perf_file="$base_dir/perf_data/latest"

run_bench() {
  echo "running 1K test">&2; "$base_dir/bench.sh" --time-only --iterations 10 1K 20000
  echo "running 1M test">&2; "$base_dir/bench.sh" --time-only --iterations 10 1M 2000
  echo "running 512M test">&2; "$base_dir/bench.sh" --time-only --iterations 5 512M 20
}

new_perf_file="$(mktemp)"

finish() {
  if [ -f "$new_perf_file" ]; then
    rm "$new_perf_file"
  fi
}
trap finish EXIT


run_bench > "$new_perf_file"

{
  IFS= read -r new_1K
  IFS= read -r new_1M
  IFS= read -r new_512M
} <"$new_perf_file"

if [ -f "$old_perf_file" ]; then
  {
    IFS= read -r old_1K
    IFS= read -r old_1M
    IFS= read -r old_512M
  } <"$old_perf_file"

  compare() {
    local name="$1"
    local old="$2"
    local new="$3"

    local percent_diff
    percent_diff=$(echo "scale=2; (($new - $old) / $old) * 100" | bc)
    percent_diff=$(printf "% 6s\n" "$percent_diff") # left pad with spaces

    echo -e "$name\tOld: $old""s\tNew: $new""s\t$percent_diff%">&2
  }

  compare "1K" "$old_1K" "$new_1K"
  compare "1M" "$old_1M" "$new_1M"
  compare "512M" "$old_512M" "$new_512M"
else
  echo -e "1K\t$new_1K""s">&2
  echo -e "1M\t$new_1M""s">&2
  echo -e "512M\t$new_512M""s">&2
fi

do_save="false"
while true; do
  read -p "Save? [y/N]: " yn

  if [ "$yn" == "y" ] || [ "$yn" == "Y" ]; then
    do_save="true"
    break
  fi

  if [ "$yn" == "n" ] || [ "$yn" == "n" ] || [ "$yn" == "" ]; then
    break
  fi
done

if [ "$do_save" == "true" ]; then
  mkdir -p "$base_dir/perf_data/"
  mv "$new_perf_file" "$old_perf_file"
  cp "$old_perf_file" "$base_dir/perf_data/$(date '+%Y-%m-%d %k:%M')"
  echo "Saved!">&2
fi
