# Installation Guide

Quick setup guide for the Valkey Vector Search Benchmark environment.

## Quick Start

### Step 1: Install System Dependencies

```bash
sudo apt-get update
sudo apt-get install -y cmake gcc g++ make pkg-config python3 python3-pip python3-venv libhdf5-dev
```

### Step 2: Clone Repository

```bash
git clone --recursive https://github.com/zvi-code/valkey-search-bench.git
cd valkey-search-bench
```

> **Note**: The `--recursive` flag fetches required submodules. If you already cloned without it, run: `git submodule update --init --recursive`

### Step 3: Build valkey-benchmark

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Step 4: Verify Build

```bash
./bin/valkey-benchmark --version
```

### Step 5: Setup Python Environment

```bash
cd ..  # Back to project root
python3 -m venv venv
source venv/bin/activate
pip install --upgrade pip
pip install vectordb-bench==1.0.10 h5py pandas pyarrow numpy
```

### Step 6: Download a Dataset

```bash
./prep_datasets/dataset.sh get mnist
```

### Step 7: Run a Test Benchmark

```bash
# Test connectivity (replace with your server address)
./build/bin/valkey-benchmark -h localhost -p 6379 -t ping -n 1000 -q

# Run vector benchmark with dataset
./build/bin/valkey-benchmark -h localhost -p 6379 \
  --dataset datasets/mnist.bin \
  --search --search-name mnist-test \
  -t vec-load -n 1000
```

---

## Next Steps

- **Dataset Management**: See [DATASETS.md](DATASETS.md) for downloading and preparing datasets
- **Running Benchmarks**: See [BENCHMARKING.md](BENCHMARKING.md) for benchmark usage
- **Advanced Features**: See [ADVANCED.md](ADVANCED.md) for optimizer and metadata filtering

---

## Appendix A: System Requirements

### Minimum Requirements

- **OS**: Ubuntu 20.04+ or compatible Linux
- **CPU**: x86_64 or ARM64 (aarch64)
- **RAM**: 8GB minimum, 16GB+ recommended
- **Storage**: 50GB+ for datasets

### Recommended for Production Benchmarks

- **RAM**: 32GB+ for large datasets
- **Storage**: 200GB+ SSD/NVMe
- **Network**: Good bandwidth for downloading datasets (up to 40GB+)

### ARM64 Support

This benchmark is optimized for ARM64 servers, particularly AWS Graviton:

| Use Case | Recommended Instance |
|----------|---------------------|
| Development & Testing | c7g.2xlarge (8 vCPU, 16GB) |
| Production Benchmarks | c7g.8xlarge (32 vCPU, 64GB) |
| Large Datasets | c7gd.8xlarge (+ 950GB NVMe) |
| Memory-Intensive | r7g.8xlarge (32 vCPU, 256GB) |

---

## Appendix B: NVMe Storage Setup (Large Datasets)

For datasets larger than 10GB, using NVMe instance storage is recommended on AWS.

### Format and Mount NVMe Drive

```bash
# Identify the NVMe drive
lsblk

# Create mount point and format
sudo mkdir -p /mnt/data
sudo mkfs.ext4 /dev/nvme1n1
sudo mount -o defaults,noatime,discard /dev/nvme1n1 /mnt/data
sudo chown -R $USER:$USER /mnt/data

# Create dataset directories
mkdir -p /mnt/data/datasets /mnt/data/build-datasets
```

### Configure Dataset Paths

Set environment variables to use NVMe storage:

```bash
export DATASET_PATH=/mnt/data/datasets
export BUILD_DATASET_PATH=/mnt/data/build-datasets
```

Add to `~/.bashrc` for persistence.

> **Note**: If `/mnt/data` is not available, the dataset manager automatically uses local project directories (`datasets/raw/` and `datasets/`).

---

## Appendix C: Build Options

### Debug Build

```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
```

### Memory Allocator Options

```bash
# jemalloc (default, recommended)
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_MALLOC=jemalloc ..

# libc malloc (NOT recommended)
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_MALLOC=libc ..
```

### Custom Install Prefix

```bash
cmake -DCMAKE_INSTALL_PREFIX=/usr/local ..
```

---

## Appendix D: Troubleshooting

### Build Issues

**CMake not found**
```bash
sudo apt-get install cmake
```

**HDF5 not found**
```bash
sudo apt-get install libhdf5-dev libhdf5-serial-dev
```

**Missing headers during compilation**
```bash
sudo apt-get install build-essential
```

**jemalloc library not found**
```bash
# Ensure submodules are initialized
git submodule update --init --recursive

# Clean rebuild
cd build && rm -rf * && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)
```

**Undefined reference to je_malloc**
```bash
cd build && rm -rf * && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)
```

### Runtime Issues

**Segmentation fault when running**
```bash
# Verify jemalloc is linked
nm bin/valkey-benchmark | grep je_malloc
# Should show jemalloc symbols
```

### Python Issues

**ModuleNotFoundError: No module named 'vectordb_bench'**
```bash
# Ensure venv is activated
source venv/bin/activate
pip install vectordb-bench==1.0.10
```

**ImportError: libhdf5.so not found**
```bash
sudo apt-get install libhdf5-dev libhdf5-serial-dev
pip uninstall h5py && pip install h5py --no-binary h5py
```

**PyArrow installation fails**
```bash
pip install pyarrow==14.0.0
```

### Storage Issues

**No space left on device**
- Use NVMe storage (see Appendix B)
- Clean build artifacts: `cd build && make clean`
- Remove old datasets: `rm datasets/*.bin`

---

## Appendix E: Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `DATASET_PATH` | Raw downloads and HDF5 cache | `./datasets/raw/` |
| `BUILD_DATASET_PATH` | Final binary datasets | `./datasets/` |
| `VENV_PATH` | Python virtual environment | `./venv/` |

---

## Appendix F: Clean Uninstall

```bash
# Remove build directory
rm -rf build

# Remove Python environment
rm -rf venv

# Remove datasets
rm -rf datasets

# If using NVMe storage
rm -rf /mnt/data/datasets /mnt/data/build-datasets
sudo umount /mnt/data
```
