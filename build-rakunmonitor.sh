#!/bin/sh
# Build script for Raccoon Monitor XFCE Panel Plugin

set -e

echo "=== Building Raccoon Monitor Panel Plugin ==="

# Get compiler flags for dependencies
CFLAGS="$(pkg-config --cflags libxfce4panel-2.0 gtk+-3.0 cairo) -fPIC -Wall -Wextra -O2"
LIBS="$(pkg-config --libs libxfce4panel-2.0 gtk+-3.0 cairo) -lrt"

echo "Compiling rakunmonitor.c..."
cc $CFLAGS -c rakunmonitor.c -o rakunmonitor.o

echo "Linking librakunmonitor.so..."
cc -shared -o librakunmonitor.so rakunmonitor.o $LIBS

echo "✓ Build complete!"
echo ""
echo "To install:"
echo "  sudo ./install-rakunmonitor.sh"
