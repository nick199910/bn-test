#!/bin/bash
# Start ZeroMQ Subscriber for MSK message testing
# This subscribes to messages published by ws_client_delay

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== ZeroMQ Subscriber Starter ==="

# Check if ZMQ is installed
if ! pkg-config --exists libzmq; then
    echo " Error: ZeroMQ not installed"
    echo "   Install: sudo apt-get install -y libzmq3-dev pkg-config"
    exit 1
fi

# Compile subscriber if needed
if [ ! -f zmq_sub ] || [ zmq_subscriber_test.cpp -nt zmq_sub ]; then
    echo "Compiling ZMQ subscriber..."
    g++ -o zmq_sub zmq_subscriber_test.cpp -lzmq -std=c++11
    if [ $? -ne 0 ]; then
        echo " Compilation failed"
        exit 1
    fi
    echo " Compilation successful"
fi

# Run subscriber
echo ""
echo "Starting ZMQ Subscriber on tcp://localhost:5555"
echo "Press Ctrl+C to stop"
echo "================================================"
echo ""

./zmq_sub
