#!/bin/bash

set -euo pipefail

base_dir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"

only_wcp="false"
show_wcp_progress="false"
time_only="false"
iterations=1
bad_arg="false"

while [[ $# -gt 0 ]]; do
    arg="$1"

    # Options all start with "-", and are always at the start of the params
    if [ "${arg:0:1}" != "-" ]; then
        break
    fi

    case "$arg" in
        -w|--only-wcp)
            only_wcp="true"
            shift
        ;;

        --show-wcp-progress)
          show_wcp_progress="true"
          shift
        ;;

        --time-only)
          time_only="true"
          only_wcp="true"
          shift
        ;;

        -i|--iterations)
          shift
          iterations="$1"
          shift
        ;;

        *)
            bad_arg="true"
            echo "Unrecognised option: $1" >&2
            echo >&2
            break
        ;;
    esac
done

if [ "$#" -ne 2 ] || [ "$bad_arg" == "true" ]; then
    echo "Usage $0 [OPTION]... [FILE_SIZE] [FILE_COUNT]" >&2
    echo "Options:" >&2
    echo "  -w|--only-wcp         Don't run other programs for comparison" >&2
    echo "  --show-wcp-progress   Show the wcp progress bar. Hidden by default." >&2
    echo "  --time-only           Output only the time of wcp. Implies -w." >&2
    echo "  -i|--iterations N     Run each test N times, and report the minimum result." >&2
    exit 1
fi

FILE_SIZE=$1
FILE_COUNT=$2

TEST_DATA="$base_dir/test_data/$FILE_SIZE""_$FILE_COUNT"
TEST_DEST="$base_dir/test_dest"

message() {
    if [ "$time_only" == "false" ]; then
      echo $@
    fi
}

build() {
    pushd "$base_dir" > /dev/null

    mkdir -p build_test
    cd build_test
    chronic cmake .. -DCMAKE_BUILD_TYPE=Release
    chronic make -j "$(grep -c processor /proc/cpuinfo)"

    popd > /dev/null
}

# This function should do the same thing as getTestDataFolder in tests.cpp. They share cached data.
generate_data() {
    local test_folder="$1"
    local file_size="$2"
    local count="$3"

    local pids=()
    for i in $(seq 1 "$count"); do
        message "    Generating file $i/$count"
        mkdir -p "$test_folder/$file_size"
        dd if=/dev/urandom of="$test_folder/$file_size/$i" bs="$file_size" count=1 2>/dev/null &
        pids[${i}]=$!
    done

    for pid in ${pids[*]}; do
        wait $pid
    done
}

run_test() {
    local seconds=99999999

    for ((i=1; i<=iterations; i++)); do
      message -n "Preparing for test... "

      if [ -e "$TEST_DEST" ]; then
          rm -rf "$TEST_DEST"
      fi

      sync
      sudo bash -c "echo 3 > /proc/sys/vm/drop_caches"
      message "done"

      message -n "testing $*... "
      /usr/bin/time -o /tmp/wcp_bench_ts_temp -f "%e" $@ "$TEST_DATA"/ "$TEST_DEST"/

      if [ "$(echo "$(cat /tmp/wcp_bench_ts_temp) < $seconds" | bc)" == 1 ]; then
          seconds="$(cat /tmp/wcp_bench_ts_temp)"
      fi
      message "done in $(cat /tmp/wcp_bench_ts_temp)s"
    done

    local mibps
    local filesps
    if [ "$seconds" == "0.00" ]; then
      mibps="NaN"
      filesps="NaN"
    else
      mibps=$(echo "scale=2;($TOTAL_SIZE)/$seconds" | bc)
      filesps=$(echo "scale=2;($TOTAL_FILE_COUNT)/$seconds" | bc)
    fi

    if [ "$time_only" == "false" ]; then
      echo -e "    $seconds""s\t$mibps MiB/s\t$filesps files/s"
    else
      echo -e "$seconds"
    fi
}

if [ ! -e "$TEST_DATA/done_tag" ]; then
    message "Generating test data..."
    rm -rf "$TEST_DATA"
    generate_data "$TEST_DATA" "$FILE_SIZE" "$FILE_COUNT"
    touch "$TEST_DATA/done_tag"
fi

TOTAL_SIZE=$(du -s -BM "$TEST_DATA" | sed 's/M.*//')
TOTAL_FILE_COUNT=$(find "$TEST_DATA" -type f | wc -l)

build

if [ "$only_wcp" == "false" ]; then
  run_test "cp -r"
  run_test "rsync -r --inplace -W --no-compress"
fi

if [ "$show_wcp_progress" == "false" ]; then
  run_test "./build_test/wcp" 2>/dev/null
else
  run_test "./build_test/wcp"
fi

diff -r "$TEST_DATA" "$TEST_DEST"
