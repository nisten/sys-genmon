#!/bin/sh

DEBUG_FLAGS="-Og -g -fsanitize=address -fsanitize=undefined"
PERF_FLAGS="-march=native -O3"
FEATURE_FLAGS=""

LIB_FLAGS="-lrt"
WARNING_FLAGS="-Wall -Wextra -Wpedantic"
OUTPUT_FILE="sys-genmon"

if [ "$1" = "debug" ]; then
    echo "Built in debug mode."
    cc sys-genmon.c -o $OUTPUT_FILE $FEATURE_FLAGS $LIB_FLAGS $WARNING_FLAGS $DEBUG_FLAGS
    exit 0

elif [ "$1" = "cuda" ]; then
    echo "Built in CUDA MODE (this is a pun)."

    CUDA_PATH="/usr/local/cuda"
    CUDA_INCLUDE_FLAGS="-I$CUDA_PATH/include"
    CUDA_LIB_FLAGS="-L$CUDA_PATH/lib64 -lcuda -lcupti"

    FEATURE_FLAGS="$FEATURE_FLAGS -DCUDA_MODE $CUDA_INCLUDE_FLAGS $CUDA_LIB_FLAGS"


    if [ ! -d "$CUDA_PATH" ]; then
        echo "Could not find your cuda-toolkit installation:"
        echo "$CUDA_PATH"
        exit 1
    fi

    cc sys-genmon.c -o $OUTPUT_FILE $FEATURE_FLAGS $LIB_FLAGS $WARNING_FLAGS $PERF_FLAGS
    exit 0

else
    echo "Built in release mode."
    cc sys-genmon.c -o $OUTPUT_FILE $FEATURE_FLAGS $LIB_FLAGS $WARNING_FLAGS $PERF_FLAGS
    exit 0
fi
