#!/bin/bash

set -euo pipefail

only_wcp="false"
show_wcp_progress="false"
output_csv="false"
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

        --csv)
          output_csv="true"
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
    echo "  --csv                 Output csv format instead of normal text." >&2
    exit 1
fi

FILE_SIZE=$1
FILE_COUNT=$2

TEST_DATA="./test_data/$FILE_SIZE""_$FILE_COUNT"
TEST_DEST="./test_dest"

message() {
    if [ "$output_csv" == "false" ]; then
      echo $@
    fi
}

build() {
    mkdir -p build_test
    cd build_test
    chronic cmake .. -DCMAKE_BUILD_TYPE=Release
    chronic make -j "$(grep -c processor /proc/cpuinfo)"
    cd ..
}

# This function should do the same thing as getTestDataFolder in tests.cpp. They share cached data.
generate_data() {
    local test_folder="$1"
    local file_size="$2"
    local count="$3"

    for i in $(seq 1 "$count"); do
        message "    Generating file $i/$count"
        mkdir -p "$test_folder/$file_size"
        chronic dd if=/dev/urandom of="$test_folder/$file_size/$i" bs="$file_size" count=1 2>&1
    done
}

run_test() {
    message -n "Preparing for test... "

    if [ -e "$TEST_DEST" ]; then
        rm -rf "$TEST_DEST"
    fi
    
    sync
    sudo bash -c "echo 3 > /proc/sys/vm/drop_caches"
    message "done"

    message -n "testing $*... "
    /usr/bin/time -o /tmp/wcp_bench_ts_temp -f "%e" $@ "$TEST_DATA"/ "$TEST_DEST"/
    message "done"

    local seconds
    local mibps
    local filesps
    seconds=$(cat /tmp/wcp_bench_ts_temp)
    if [ "$seconds" == "0.00" ]; then
      mibps="NaN"
      filesps="NaN"
    else
      mibps=$(echo "scale=2;($TOTAL_SIZE)/$seconds" | bc)
      filesps=$(echo "scale=2;($TOTAL_FILE_COUNT)/$seconds" | bc)
    fi

    if [ "$output_csv" == "false" ]; then
      echo -e "    $seconds""s\t$mibps MiB/s\t$filesps files/s"
    else
      echo -e $@",$seconds,$mibps,$filesps"
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
