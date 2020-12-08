#!/bin/bash

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage $0 <SIZE_MB> <FILE_COUNT>" >&2
    exit 1
fi

SIZE_MB=$1
FILE_COUNT=$2

TEST_DATA="./test_data/$SIZE_MB""_$FILE_COUNT"
TEST_DEST="./test_dest"

build() {
    mkdir -p build_test
    cd build_test
    chronic cmake .. -DCMAKE_BUILD_TYPE=Release
    chronic make -j "$(grep -c processor /proc/cpuinfo)"
    cd ..
}

generate_data() {
    local test_folder="$1"
    local size_in_mb="$2"
    local count="$3"

    for i in $(seq 1 "$count"); do
        echo "    Generating file $i/$count"
        mkdir -p "$test_folder/$size_in_mb"
        chronic dd if=/dev/urandom of="$test_folder/$size_in_mb/$i" bs=1M count="$size_in_mb" 2>&1
    done
}

run_test() {
    echo -n "Preparing for test... "

    if [ -e "$TEST_DEST" ]; then
        rm -rf "$TEST_DEST"
    fi
    
    sync
    sudo bash -c "echo 3 > /proc/sys/vm/drop_caches"
    echo "done"

    echo -n "testing $*... "
    /usr/bin/time -o /tmp/wcp_bench_ts_temp -f "%e" $@ "$TEST_DATA"/ "$TEST_DEST"/
    echo "done"

    local seconds
    local mibps
    local filesps
    seconds=$(cat /tmp/wcp_bench_ts_temp)
    mibps=$(echo "scale=2;($TOTAL_SIZE)/$seconds" | bc)
    filesps=$(echo "scale=2;($TOTAL_FILE_COUNT)/$seconds" | bc)
    echo "    finished in $seconds""s"
    echo "    avg $mibps MiB/s"
    echo "    avg $filesps files/s"
}

if [ ! -e "$TEST_DATA/done_tag" ]; then
    echo "Generating test data..."
    rm -rf "$TEST_DATA"
    generate_data "$TEST_DATA" "$SIZE_MB" "$FILE_COUNT"
    touch "$TEST_DATA/done_tag"
fi

TOTAL_SIZE=$(du -s -BM "$TEST_DATA" | sed 's/M.*//')
TOTAL_FILE_COUNT=$(find "$TEST_DATA" -type f | wc -l)

build

run_test "cp -r"
run_test "rsync -r --inplace -W --no-compress"

run_test "./build_test/wcp"
diff -r "$TEST_DATA" "$TEST_DEST"
