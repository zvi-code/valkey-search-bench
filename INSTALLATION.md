# Installation Guide

Complete guide for setting up the Valkey Vector Search Benchmark environment.

## Prerequisites

### System Requirements

- **OS**: Ubuntu/Linux (tested on Ubuntu 20.04+, 22.04+)
- **CPU**: Modern processor - **ARM64/aarch64** (AWS Graviton2/3/4) or x86_64
- **RAM**: 16GB+ recommended for large datasets (32GB+ for ARM Graviton instances)
- **Storage**: 200GB+ SSD/NVMe recommended for large datasets
- **Network**: Good bandwidth for downloading datasets (up to 40GB+)

### ARM64 Focus

This benchmark is optimized for ARM64 servers with examples focused on AWS Graviton:

- ✅ **Primary Platform**: AWS Graviton2, Graviton3, Graviton4 (ARM64)
- ✅ **Also Supports**: x86_64 (Intel/AMD)
- ✅ **Performance**: Native ARM builds deliver excellent performance
- ✅ **All Dependencies**: jemalloc, HDF5, Python packages work natively on ARM64
- � **Recommended**: AWS c7g/r7g instances (Graviton3) or c8g (Graviton4)

**Example ARM64 Instance Types:**
```bash
# AWS Graviton3 - Best price/performance
c7g.2xlarge   # 8 vCPU, 16GB RAM  - Good for testing
c7g.8xlarge   # 32 vCPU, 64GB RAM - Production benchmarks
r7g.4xlarge   # 16 vCPU, 128GB RAM - Large datasets

# AWS Graviton4 - Latest generation
c8g.4xlarge   # 16 vCPU, 32GB RAM
```

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

# Verify architecture (should show: arm64 or aarch64 on ARM systems)
dpkg --print-architecture
uname -m
```

## Building valkey-benchmark

### 1. Clone Repository

```bash
git clone --recursive https://github.com/your-org/valkey-search-benchmark.git
cd valkey-search-benchmark
```

**Note**: The `--recursive` flag is important - it fetches the Valkey submodule which provides core utilities and jemalloc.

If you already cloned without `--recursive`:
```bash
git submodule update --init --recursive
```

### 2. Build valkey-benchmark

The build system automatically fetches all dependencies from the Valkey submodule, including jemalloc.

**Example: AWS Graviton3 ARM64 instance**

```bash
cd ~/valkey-search-benchmark

# Create and enter build directory
mkdir -p build && cd build

# Configure - jemalloc is built automatically from the Valkey submodule
cmake -DCMAKE_BUILD_TYPE=Release ..

# Build valkey-benchmark
# On Graviton3 c7g.2xlarge (8 vCPU): ~2-3 minutes
# On Graviton4 c8g.4xlarge (16 vCPU): ~1-2 minutes
make -j$(nproc)

# Verify build and jemalloc linkage
./bin/valkey-benchmark --version
nm bin/valkey-benchmark | grep je_malloc
# Should show jemalloc symbols like je_malloc, je_free, etc.

# Verify ARM64 binary (on ARM systems)
file bin/valkey-benchmark
# Output: ELF 64-bit LSB executable, ARM aarch64, version 1 (SYSV)...
```

**Build options:**

```bash
# Debug build with symbols (useful for development)
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Force specific memory allocator (jemalloc is default on Linux)
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_MALLOC=jemalloc ..

# Use libc malloc (NOT RECOMMENDED - will likely crash)
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_MALLOC=libc ..

# Custom install prefix
cmake -DCMAKE_INSTALL_PREFIX=/usr/local ..

# Build all targets
make -j$(nproc)
```

### 4. Verify Installation

```bash
# Check version
./bin/valkey-benchmark --version
# Output: valkey-benchmark 255.255.255 (git:...)

# Verify jemalloc symbols are present
nm bin/valkey-benchmark | grep je_ | head -5
# Should show: je_malloc, je_free, je_calloc, je_realloc, etc.

# Quick test (requires running Valkey/Redis instance)
./bin/valkey-benchmark -h localhost -t ping -n 1000 -q
# Should complete without crashes
```

### Troubleshooting Build Issues

**Error: "Cannot find jemalloc library"**
```bash
# Make sure jemalloc-build directory exists in build/
ls build/jemalloc-build/lib/libjemalloc.a

# If missing, ensure submodules are initialized and rebuild:
git submodule update --init --recursive
cd build && rm -rf * && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)
```

**Error: "Undefined reference to je_malloc"**
```bash
# Clean and rebuild
cd build
rm -rf *
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

**Segmentation fault when running**
```bash
# Verify jemalloc is linked
ldd bin/valkey-benchmark | grep jemalloc
# Or check symbols:
nm bin/valkey-benchmark | grep je_malloc

# If jemalloc symbols missing, rebuild with jemalloc
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

**For ARM64 AWS instances, this is STRONGLY RECOMMENDED** as instance store NVMe drives provide:
- High-speed local storage (up to 7.5GB/s sequential read on Graviton3)
- No EBS costs for temporary benchmark data
- Ideal for multi-GB dataset downloads and conversions

**Example: AWS Graviton3 c7gd.4xlarge with 950GB NVMe**

#### Step 1: Identify NVMe Drive

```bash
# List block devices
lsblk

# Typical output on AWS Graviton c7gd instance:
# NAME         MAJ:MIN RM   SIZE RO TYPE MOUNTPOINT
# nvme0n1      259:0    0    40G  0 disk /              (root EBS)
# nvme1n1      259:1    0   950G  0 disk                (instance store NVMe)
#
# On r7gd instances:
# nvme1n1      259:1    0   1.9T  0 disk                (larger instance store)
```

#### Step 2: Format and Mount NVMe Drive

```bash
# Create mount point
sudo mkdir -p /mnt/data

# Format (⚠️ WARNING: Erases all data on the drive!)
sudo mkfs.ext4 /dev/nvme1n1

# Mount with optimal settings for large files
sudo mount -o defaults,noatime,discard /dev/nvme1n1 /mnt/data

# Set ownership
sudo chown -R $USER:$USER /mnt/data

# Make persistent across reboots (optional for instance store)
# Note: Instance store is ephemeral on AWS, data lost on stop/start
echo "/dev/nvme1n1 /mnt/data ext4 defaults,noatime,discard 0 2" | sudo tee -a /etc/fstab

# Verify mount and performance
df -h /mnt/data
# Should show ~900GB available on c7gd.4xlarge

# Test write performance (optional)
dd if=/dev/zero of=/mnt/data/testfile bs=1G count=1 oflag=direct
# Should show ~1-2 GB/s write speed on Graviton3 NVMe
rm /mnt/data/testfile
```

#### Step 3: Setup Python Environment on NVMe

```bash
# Create directory structure for large datasets
mkdir -p /mnt/data/datasets           # Raw HDF5/binary datasets
mkdir -p /mnt/data/build-datasets     # Converted datasets
mkdir -p /mnt/data/downloads          # Temporary downloads

# Create virtual environment on NVMe (saves root filesystem space)
cd /mnt/data
python3 -m venv vectordb-bench-env

# Activate
source /mnt/data/vectordb-bench-env/bin/activate

# Install dependencies (ARM64-native builds from PyPI)
pip install --upgrade pip
pip install vectordb-bench==1.0.10 h5py pandas pyarrow numpy

# Verify installation
python -c "import vectordb_bench; print('✓ vectordb-bench installed')"
python -c "import h5py; import pandas; import pyarrow; print('✓ All packages ready')"

# Check installed package architectures (optional)
python -c "import numpy; numpy.show_config()"
# Should show ARM NEON optimizations on ARM64 systems
```

**Storage Planning for ARM64 Instances:**

| Dataset Size | Recommended Instance Type | NVMe Size |
|--------------|--------------------------|-----------|
| < 10GB | c7g.2xlarge | No NVMe needed (use EBS) |
| 10-100GB | c7gd.2xlarge | 237GB NVMe |
| 100-500GB | c7gd.8xlarge | 950GB NVMe |
| 500GB-1TB | c7gd.16xlarge | 1900GB NVMe |
| 1TB+ | r7gd.16xlarge | 3800GB NVMe |

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

## ARM64-Specific Tips

### Performance Optimization

ARM64 Graviton processors offer excellent performance for vector workloads:

```bash
# Check CPU features (ARM64 SIMD extensions)
lscpu | grep -i neon
# NEON (ARM SIMD) accelerates vector operations

# Check memory bandwidth
sudo apt-get install -y sysbench
sysbench memory --memory-oper=read run | grep 'transferred'
# Graviton3: ~100-200 GB/s memory bandwidth
```

### Multi-core Utilization

```bash
# Graviton instances have many cores - use them!
# c7g.16xlarge has 64 vCPUs

# Build faster with all cores
make -j$(nproc)

# Dataset conversion with parallel processing
python prep_datasets/convert_parquet_to_hdf5.py \
  --input /mnt/data/downloads/dataset \
  --output /mnt/data/datasets/dataset.hdf5 \
  --workers $(nproc)
```

### Cost Optimization

ARM64 Graviton instances offer better price/performance:

```bash
# Example cost comparison (us-east-1, October 2025):
# c7g.4xlarge (ARM64):  $0.58/hour  - 16 vCPU, 32GB RAM, Graviton3
# c6i.4xlarge (x86_64): $0.68/hour  - 16 vCPU, 32GB RAM, Intel

# ~17% cost savings with similar or better performance
# For long-running benchmarks, this adds up!
```

### Recommended ARM64 Instance Types

**Development & Testing:**
- `c7g.2xlarge` - 8 vCPU, 16GB RAM - Good for quick tests
- `c7gd.2xlarge` - Same + 237GB NVMe - For medium datasets

**Production Benchmarks:**
- `c7g.8xlarge` - 32 vCPU, 64GB RAM - Parallel workloads
- `c7gd.8xlarge` - Same + 950GB NVMe - Large datasets
- `r7g.8xlarge` - 32 vCPU, 256GB RAM - Memory-intensive

**Large-Scale Testing:**
- `c8g.12xlarge` - 48 vCPU, 96GB RAM, Graviton4 - Latest gen
- `r7gd.16xlarge` - 64 vCPU, 512GB RAM, 3800GB NVMe - Massive datasets

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
