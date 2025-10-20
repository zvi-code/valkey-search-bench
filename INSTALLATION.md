# Installation Guide

Complete guide for setting up the Valkey Vector Search Benchmark environment.

## Prerequisites

### System Requirements

- **OS**: Ubuntu/Linux (tested on Ubuntu 20.04+)
- **CPU**: Modern x86_64 processor
- **RAM**: 16GB+ recommended for large datasets
- **Storage**: 100GB+ recommended (SSD/NVMe preferred)
- **Network**: Good bandwidth for downloading datasets (up to 40GB+)

### Software Dependencies

```bash
# Build tools
sudo apt-get update
sudo apt-get install -y \
  cmake \
  gcc \
  g++ \
  make \
  pkg-config

# Python 3.8+ for dataset management
sudo apt-get install -y \
  python3 \
  python3-pip \
  python3-venv

# HDF5 libraries (for dataset conversion)
sudo apt-get install -y \
  libhdf5-dev \
  libhdf5-serial-dev
```

## Building valkey-benchmark

### 1. Clone Repository

```bash
git clone https://github.com/your-org/valkey-search-benchmark.git
cd valkey-search-benchmark
```

### 2. Build with CMake

```bash
# Create build directory
mkdir build && cd build

# Configure (Release build for performance)
cmake -DCMAKE_BUILD_TYPE=Release ..

# Build valkey-benchmark
make valkey-benchmark

# Verify build
./bin/valkey-benchmark --version
```

**Build options:**

```bash
# Debug build with symbols
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Custom install prefix
cmake -DCMAKE_INSTALL_PREFIX=/usr/local ..

# Build all targets
make -j$(nproc)
```

## Python Environment Setup

The dataset management tools require Python packages. It's recommended to use a virtual environment.

### Option 1: Standard Setup (Root Filesystem)

```bash
# Create virtual environment
python3 -m venv venv

# Activate
source venv/bin/activate

# Install dependencies
pip install --upgrade pip
pip install vectordb-bench==1.0.10 h5py pandas pyarrow numpy
```

### Option 2: NVMe Storage Setup (Recommended for Large Datasets)

If you're working with large datasets (>10GB), using NVMe storage avoids filling the root filesystem.

#### Step 1: Identify NVMe Drive

```bash
# List block devices
lsblk

# Example output:
# NAME        MAJ:MIN RM   SIZE RO TYPE MOUNTPOINT
# nvme0n1     259:0    0   200G  0 disk /
# nvme1n1     259:1    0   512G  0 disk        <-- Use this
```

#### Step 2: Mount NVMe Drive

```bash
# Create mount point
sudo mkdir -p /mnt/data

# Format (⚠️ WARNING: Erases all data!)
sudo mkfs.ext4 /dev/nvme1n1

# Mount
sudo mount /dev/nvme1n1 /mnt/data

# Set ownership
sudo chown -R $USER:$USER /mnt/data

# Make persistent (add to /etc/fstab)
echo "/dev/nvme1n1 /mnt/data ext4 defaults 0 2" | sudo tee -a /etc/fstab

# Verify
df -h /mnt/data
```

#### Step 3: Setup Python Environment on NVMe

```bash
# Create directory structure
mkdir -p /mnt/data/datasets
mkdir -p /mnt/data/build-datasets

# Create virtual environment on NVMe
cd /mnt/data
python3 -m venv vectordb-bench-env

# Activate
source /mnt/data/vectordb-bench-env/bin/activate

# Install dependencies
pip install --upgrade pip
pip install vectordb-bench==1.0.10 h5py pandas pyarrow numpy

# Verify installation
python -c "import vectordb_bench; print('✓ vectordb-bench installed')"
python -c "import h5py; import pandas; import pyarrow; print('✓ All packages ready')"
```

## Verification

### Test Build

```bash
cd build

# Show help
./bin/valkey-benchmark --help

# Test with dummy dataset (if available)
./bin/valkey-benchmark --dataset test_mini.bin -n 100 -t ping
```

### Test Python Environment

```bash
# Activate environment
source venv/bin/activate  # or /mnt/data/vectordb-bench-env/bin/activate

# Test imports
python3 << EOF
import vectordb_bench
import h5py
import pandas
import pyarrow
import numpy
print("✓ All Python dependencies installed successfully")
EOF
```

## Troubleshooting

### Build Issues

**Error: `CMake not found`**
```bash
sudo apt-get install cmake
```

**Error: `Could not find HDF5`**
```bash
sudo apt-get install libhdf5-dev
```

**Error: Compilation fails with missing headers**
```bash
# Install development packages
sudo apt-get install build-essential
```

### Python Issues

**Error: `ModuleNotFoundError: No module named 'vectordb_bench'`**

Solution:
```bash
# Ensure virtual environment is activated
which python  # Should point to venv/bin/python

# Reinstall
pip uninstall vectordb-bench
pip install vectordb-bench==1.0.10
```

**Error: `ImportError: libhdf5.so.103: cannot open shared object file`**

Solution:
```bash
# Install system HDF5 libraries
sudo apt-get install libhdf5-dev libhdf5-serial-dev

# Reinstall h5py from source
pip uninstall h5py
pip install h5py --no-binary h5py
```

**Error: PyArrow installation fails**

Solution:
```bash
# Install system dependencies
sudo apt-get install libarrow-dev

# Or use specific version
pip install pyarrow==14.0.0
```

### Storage Issues

**Error: No space left on device**

Solutions:
1. Use NVMe mount (see Option 2 above)
2. Clean build artifacts:
   ```bash
   cd build
   make clean
   rm -rf CMakeFiles CMakeCache.txt
   ```
3. Remove old dataset files:
   ```bash
   rm datasets/*.bin
   rm /mnt/data/datasets/*.hdf5
   ```

## Next Steps

- **Dataset Management**: See [DATASETS.md](DATASETS.md) for downloading and preparing datasets
- **Running Benchmarks**: See [BENCHMARKING.md](BENCHMARKING.md) for benchmark usage
- **Advanced Features**: See [ADVANCED.md](ADVANCED.md) for optimizer and metadata filtering

## Environment Variables

Optional environment variables for customization:

```bash
# Dataset search paths
export DATASET_PATH=/mnt/data/build-datasets

# Python environment activation
export VENV_PATH=/mnt/data/vectordb-bench-env

# Add to ~/.bashrc for persistence
echo 'source /mnt/data/vectordb-bench-env/bin/activate' >> ~/.bashrc
```

## Clean Uninstall

```bash
# Remove build directory
rm -rf build

# Remove Python environment
rm -rf venv
# or
rm -rf /mnt/data/vectordb-bench-env

# Remove datasets (if desired)
rm -rf datasets
rm -rf /mnt/data/datasets
rm -rf /mnt/data/build-datasets

# Unmount NVMe (if mounted)
sudo umount /mnt/data
```
