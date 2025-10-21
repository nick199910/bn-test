#!/bin/bash
# run_profiler.sh - Launch all components of the WebSocket latency profiler

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
BINANCE_WS_URI="wss://fstream.binance.com:443/ws/stream"
DPDK_PORT=0
DPDK_EAL_ARGS="-l 0-1 -n 4 --"

echo -e "${GREEN}WebSocket Latency Profiler${NC}"
echo "=============================="
echo ""

# Check if running as root (required for eBPF and DPDK)
if [ "$EUID" -ne 0 ]; then
  echo -e "${RED}Error: This script must be run as root${NC}"
  echo "Reason: eBPF and DPDK require root privileges"
  exit 1
fi

# Check if binaries exist
if [ ! -f "./websocket_client" ] || [ ! -f "./ebpf_loader" ] || 
   [ ! -f "./dpdk_capture" ] || [ ! -f "./event_correlator" ]; then
  echo -e "${RED}Error: Binaries not found. Please build first:${NC}"
  echo "  mkdir build && cd build"
  echo "  cmake .."
  echo "  make"
  exit 1
fi

# Check if BPF program exists
if [ ! -f "./bpf_program_fixed.o" ]; then
  echo -e "${RED}Error: BPF program not found${NC}"
  exit 1
fi

# Cleanup function
cleanup() {
  echo -e "\n${YELLOW}Shutting down...${NC}"
  
  # Kill all child processes
  pkill -P $$ 2>/dev/null || true
  
  # Remove shared memory
  rm -f /dev/shm/ws_latency_queue 2>/dev/null || true
  rm -f /dev/shm/ws_sockfd_map 2>/dev/null || true
  
  echo -e "${GREEN}Cleanup complete${NC}"
}

trap cleanup EXIT INT TERM

# Step 1: Start WebSocket client (creates shared queue)
echo -e "${GREEN}[1/4]${NC} Starting WebSocket client..."
./websocket_client "$BINANCE_WS_URI" > websocket.log 2>&1 &
WS_PID=$!
echo "  PID: $WS_PID"
sleep 2

# Check if WebSocket client is running
if ! kill -0 $WS_PID 2>/dev/null; then
  echo -e "${RED}Error: WebSocket client failed to start${NC}"
  cat websocket.log
  exit 1
fi

# Step 2: Start eBPF loader
echo -e "${GREEN}[2/4]${NC} Starting eBPF loader..."
./ebpf_loader ./bpf_program_fixed.o > ebpf.log 2>&1 &
EBPF_PID=$!
echo "  PID: $EBPF_PID"
sleep 1

# Check if eBPF loader is running
if ! kill -0 $EBPF_PID 2>/dev/null; then
  echo -e "${RED}Error: eBPF loader failed to start${NC}"
  cat ebpf.log
  exit 1
fi

# Step 3: Start DPDK capture (optional - comment out if no DPDK NIC available)
# echo -e "${GREEN}[3/4]${NC} Starting DPDK capture..."
# echo -e "${YELLOW}  Note: This requires DPDK-compatible NIC and proper configuration${NC}"
# ./dpdk_capture $DPDK_EAL_ARGS > dpdk.log 2>&1 &

#
# DPDK_PID=$!
# echo "  PID: $DPDK_PID (may fail if no DPDK NIC available)"
# sleep 1

# # Don't fail if DPDK doesn't start (it's optional)
# if ! kill -0 $DPDK_PID 2>/dev/null; then
#   echo -e "${YELLOW}  Warning: DPDK capture not running (likely no DPDK NIC)${NC}"
#   DPDK_PID=""
# fi

# Step 4: Start event correlator
echo -e "${GREEN}[4/4]${NC} Starting event correlator..."
./event_correlator > correlator.log 2>&1 &
CORR_PID=$!
echo "  PID: $CORR_PID"
sleep 1

# Check if correlator is running
if ! kill -0 $CORR_PID 2>/dev/null; then
  echo -e "${RED}Error: Event correlator failed to start${NC}"
  cat correlator.log
  exit 1
fi

echo ""
echo -e "${GREEN}All components started successfully!${NC}"
echo "=============================="
echo "Process IDs:"
echo "  WebSocket client: $WS_PID"
echo "  eBPF loader:      $EBPF_PID"
[ -n "$DPDK_PID" ] && echo "  DPDK capture:     $DPDK_PID"
echo "  Event correlator: $CORR_PID"
echo ""
echo "Log files:"
echo "  websocket.log"
echo "  ebpf.log"
echo "  dpdk.log"
echo "  correlator.log"
echo ""
echo -e "${YELLOW}Press Ctrl+C to stop all components${NC}"
echo ""

# Tail the correlator log to show real-time output
tail -f correlator.log &
TAIL_PID=$!

# Wait for all processes
wait $WS_PID $EBPF_PID $CORR_PID $DPDK_PID 2>/dev/null || true

kill $TAIL_PID 2>/dev/null || true