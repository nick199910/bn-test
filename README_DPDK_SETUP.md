# DPDK Network Device Setup

## Problem
Your virtio device `0000:01:00.0` (eth0) is currently bound to the kernel driver `virtio-pci` and DPDK cannot use it.

## ⚠️ CRITICAL WARNING
**eth0 appears to be your only network interface.** If you bind it to DPDK:
- You will **lose SSH access immediately**
- You will **lose all network connectivity** except DPDK packet processing
- You will need console/physical access to recover

## Solutions

### Option 1: Add a Second Network Interface (RECOMMENDED)
1. Add another network interface to your VM/server for management
2. Keep eth0 for SSH/management
3. Bind the new interface to DPDK for packet capture

### Option 2: Use DPDK with eth0 (RISKY - Will disconnect you)
If you have console access or are on the physical machine:

```bash
# Run the setup script
sudo ./setup_dpdk_device.sh

# Then run your DPDK app
sudo ./build/dpdk_capture/dpdk_capture -l 6-7 -n 2 --
```

### Option 3: Use AF_PACKET/libpcap Instead of DPDK
If you don't need DPDK's performance, use standard packet capture:
- Use libpcap or AF_PACKET in your application
- No driver rebinding required
- Network remains accessible

### Option 4: DPDK with KNI (Kernel Network Interface)
Use DPDK's KNI feature to keep network access while using DPDK:
- Requires kernel module `rte_kni`
- More complex setup
- Provides both DPDK performance and network access

## Manual Setup Steps (if choosing Option 2)

```bash
# 1. Bring down the interface
sudo ip link set eth0 down

# 2. Load VFIO driver
sudo modprobe vfio-pci

# 3. Bind to DPDK
sudo dpdk-devbind.py --bind=vfio-pci 0000:01:00.0

# 4. Verify
sudo dpdk-devbind.py --status

# 5. Run your DPDK app
sudo ./build/dpdk_capture/dpdk_capture -l 6-7 -n 2 --
```

## To Restore Network Access

```bash
# Bind back to kernel driver
sudo dpdk-devbind.py --bind=virtio-pci 0000:01:00.0

# Bring interface up
sudo ip link set eth0 up

# Restore network configuration (example)
sudo dhclient eth0
```
