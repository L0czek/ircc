#!/bin/bash

# Hardware Emulator Test Script
# This script demonstrates using the hardware emulator with the TUI

set -e

echo "=========================================="
echo "  Hardware Emulator Integration Test"
echo "=========================================="
echo ""

# Build the test emulator
cd "$(dirname "$0")/.."

echo "Building emulator..."
cmake -B build
cmake --build build --target test_emulator

echo ""
echo "Starting emulator in background..."
echo "Packet log will be saved to /tmp/emulator_packet_log.txt"
echo ""

# Start the emulator with packet logging
./build/host/test_emulator --packet-log > /tmp/emulator_output.txt 2>&1 &
EMULATOR_PID=$!

echo "Emulator started with PID: $EMULATOR_PID"
echo ""

# Wait for emulator to initialize
sleep 2

echo "=========================================="
echo "  Emulator is running!"
echo "=========================================="
echo ""
echo "To test with the TUI, run:"
echo "  cd build/host"
echo "  ./hostctl --port /dev/ttyUSB0 --baud 115200"
echo ""
echo "Note: The TUI currently expects a real serial port."
echo "To use the emulator, you need to:"
echo "  1. Modify connector.cc to use EmulatorConnector, OR"
echo "  2. Create a virtual serial port pair:"
echo "     sudo modprobe veth"
echo "     ip link add veth0 type veth peer name veth1"
echo "  3. Or use a tool like socat:"
echo "     socat -d -d pty,raw,echo=0 /tmp/emulator_serial &"
echo ""

# Show emulator output
echo "Emulator output (showing last 20 lines):"
echo "----------------------------------------"
sleep 1
tail -n 20 /tmp/emulator_output.txt 2>/dev/null || echo "No output yet..."

echo ""
echo "=========================================="
echo "  Press Ctrl+C to stop"
echo "=========================================="

# Keep running
sleep 5

# Show packet log
if [ -f /tmp/emulator_output.txt ]; then
    echo ""
    echo "Packet log from emulator:"
    echo "----------------------------------------"
    grep -E "TX|RX" /tmp/emulator_output.txt | tail -n 20 || echo "No packets yet..."
fi

# Cleanup
echo ""
echo "Stopping emulator..."
kill $EMULATOR_PID 2>/dev/null || true
wait $EMULATOR_PID 2>/dev/null || true

echo "Done!"
