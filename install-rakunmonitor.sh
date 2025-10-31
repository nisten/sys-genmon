#!/bin/sh
# Install script for Raccoon Monitor XFCE Panel Plugin

set -e

if [ "$EUID" -ne 0 ] && ! command -v doas >/dev/null 2>&1; then
    echo "Please run with doas or as root"
    exit 1
fi

SUDO_CMD="doas"
if [ "$EUID" -eq 0 ]; then
    SUDO_CMD=""
fi

echo "=== Installing Raccoon Monitor Panel Plugin ==="

# Determine plugin directory
PLUGIN_DIR="/usr/lib/xfce4/panel/plugins"
DESKTOP_DIR="/usr/share/xfce4/panel/plugins"

echo "Installing library to $PLUGIN_DIR..."
$SUDO_CMD mkdir -p "$PLUGIN_DIR"
$SUDO_CMD install -m 755 librakunmonitor.so "$PLUGIN_DIR/"

echo "Installing desktop file to $DESKTOP_DIR..."
$SUDO_CMD mkdir -p "$DESKTOP_DIR"
$SUDO_CMD install -m 644 rakunmonitor.desktop.in "$DESKTOP_DIR/rakunmonitor.desktop"

echo "✓ Installation complete!"
echo ""
echo "Raccoon Monitor is now available in XFCE Panel!"
echo "To add it:"
echo "  1. Right-click your panel"
echo "  2. Panel → Add New Items..."
echo "  3. Find 'Raccoon Monitor' in the list"
echo "  4. Click 'Add'"
echo ""
echo "To remove old Generic Monitor:"
echo "  xfconf-query -c xfce4-panel -p /plugins/plugin-3 -r -R"
echo "  xfce4-panel -r"
