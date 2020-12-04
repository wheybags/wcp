#!/bin/bash

set -euo pipefail

SIZE_MB=512
FILE_COUNT=2

TEST_DATA="./test_data/$SIZE_MB""_$FILE_COUNT"
TEST_DEST="./test_dest"

generate_data() {
    local test_folder="$1"
    local size_in_mb="$2"
    local count="$3"

    for i in $(seq 1 $count); do
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

    echo -n "testing $@... "
    local seconds=$(/usr/bin/time -f "%e" $@ "$TEST_DATA" "$TEST_DEST" 2>&1)
    local mibps=$(echo "scale=2;($SIZE_MB*$FILE_COUNT)/$seconds" | bc)
    echo "finished in $seconds""s, avg $mibps MiB/s" 
}

if [ ! -e "$TEST_DATA/done_tag" ]; then
    echo "Generating test data..."
    rm -rf "$TEST_DATA"
    generate_data "$TEST_DATA" "$SIZE_MB" "$FILE_COUNT"
    touch "$TEST_DATA/done_tag"
fi

run_test "cp -r"
run_test "rsync -r"
