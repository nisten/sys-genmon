#!/bin/sh

WARNING_FLAGS="-Wall -Wextra -Wpedantic"
OUTPUT_FILE="sys-genmon"

if [ "$1" = "debug" ]; then
    echo "Built in debug mode."
    cc sys-genmon.c -o $OUTPUT_FILE $WARNING_FLAGS -march=native -Og -g -fsanitize=address -fsanitize=undefined
    exit 0
else
    echo "Built in release mode."
    cc sys-genmon.c -o $OUTPUT_FILE $WARNING_FLAGS -march=native -O3
    exit 0
fi
