#!/bin/bash
# Setup DPDK device binding

set -e

echo "=== DPDK Device Setup ==="
echo "WARNING: This will bring down eth0 and disconnect network access!"
echo "Press Ctrl+C within 5 seconds to cancel..."
sleep 5

# Bring down the interface
echo "Bringing down eth0..."
ip link set eth0 down

# Load vfio-pci kernel module
echo "Loading vfio-pci module..."
modprobe vfio-pci

# Unbind from kernel driver and bind to DPDK driver
echo "Binding device 0000:01:00.0 to vfio-pci..."
dpdk-devbind.py --bind=vfio-pci 0000:01:00.0

# Show status
echo ""
echo "=== Device Status ==="
dpdk-devbind.py --status

echo ""
echo "✓ Setup complete. Device ready for DPDK."
