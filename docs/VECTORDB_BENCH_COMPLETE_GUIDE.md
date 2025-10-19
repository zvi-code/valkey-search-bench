# Complete Guide: VectorDB-Bench Dataset Integration

**Comprehensive step-by-step guide for downloading, converting, and using vectordb-bench datasets with Valkey**

---

## Table of Contents

1. [Overview](#overview)
2. [Prerequisites](#prerequisites)
3. [Storage Setup (NVMe)](#storage-setup-nvme)
4. [Python Environment Setup](#python-environment-setup)
5. [Installing vectordb-bench](#installing-vectordb-bench)
6. [Downloading Datasets](#downloading-datasets)
7. [Converting Datasets](#converting-datasets)
8. [Using Datasets with Valkey](#using-datasets-with-valkey)
9. [**CRITICAL: ID Shuffling Bug Fix**](#critical-id-shuffling-bug-fix)
10. [Troubleshooting](#troubleshooting)
11. [Dataset Reference](#dataset-reference)

---

## Overview

This guide covers the complete workflow for:
- Setting up vectordb-bench library
- Downloading large-scale vector datasets (COHERE, OPENAI, SIFT, GIST)
- Converting from parquet → HDF5 → Valkey binary format
- Running benchmarks with test_multi_dataset.sh

**Total time**: ~30-60 minutes (depending on dataset size and network speed)

---

## Prerequisites

- **Ubuntu/Linux system** with root or sudo access
- **NVMe drive** mounted (recommended for large datasets)
- **Python 3.8+** installed
- **Storage space**: 100GB+ recommended for multiple large datasets
- **RAM**: 16GB+ recommended (optimized scripts use <5GB)
- **Network**: Good bandwidth for downloading 40GB+ datasets

---

## Storage Setup (NVMe)

### Why Use NVMe?

Large datasets (10M vectors) can be 30-40GB+. Using NVMe:
- ✅ Avoids filling up root filesystem
- ✅ Faster I/O for conversions
- ✅ Better performance for memory-mapped binary files

### Step 1: Check Available Storage

```bash
# List block devices
lsblk

# Example output:
# NAME        MAJ:MIN RM   SIZE RO TYPE MOUNTPOINT
# nvme0n1     259:0    0   200G  0 disk /
# nvme1n1     259:1    0   512G  0 disk
```

### Step 2: Mount NVMe Drive

If your NVMe drive is not mounted:

```bash
# Create mount point
sudo mkdir -p /mnt/data

# Format the drive (⚠️ WARNING: This erases all data!)
sudo mkfs.ext4 /dev/nvme1n1

# Mount the drive
sudo mount /dev/nvme1n1 /mnt/data

# Set ownership
sudo chown -R $USER:$USER /mnt/data

# Make mount persistent (add to /etc/fstab)
echo "/dev/nvme1n1 /mnt/data ext4 defaults 0 2" | sudo tee -a /etc/fstab
```

### Step 3: Create Directory Structure

```bash
# Create directories for datasets
mkdir -p /mnt/data/datasets
mkdir -p /mnt/data/build-datasets

# Verify
df -h /mnt/data
```

**Expected output:**
```
Filesystem      Size  Used Avail Use% Mounted on
/dev/nvme1n1    503G   XX    XX   XX% /mnt/data
```

---

## Python Environment Setup

### Step 1: Create Virtual Environment on NVMe

```bash
# Navigate to NVMe storage
cd /mnt/data

# Create virtual environment
python3 -m venv vectordb-bench-env

# Activate the environment
source vectordb-bench-env/bin/activate

# Upgrade pip
pip install --upgrade pip
```

### Step 2: Verify Python Version

```bash
python --version
# Should be Python 3.8 or higher
```

---

## Installing vectordb-bench

### Step 1: Install vectordb-bench Library

```bash
# Make sure virtual environment is activated
source /mnt/data/vectordb-bench-env/bin/activate

# Install vectordb-bench
pip install vectordb-bench==1.0.10

# Verify installation
python -c "import vectordb_bench; print(vectordb_bench.__version__)"
# Should print: 1.0.10
```

### Step 2: Install Additional Dependencies

The conversion scripts require additional Python packages:

```bash
# Install required packages
pip install h5py pandas pyarrow numpy

# Verify installations
python -c "import h5py; import pandas; import pyarrow; import numpy; print('✓ All packages installed')"
```

**Common Issues and Solutions:**

#### Issue 1: `ModuleNotFoundError: No module named 'vectordb_bench'`

**Solution:**
```bash
# Ensure you're using the correct Python environment
which python
# Should show: /mnt/data/vectordb-bench-env/bin/python

# Reinstall if needed
pip uninstall vectordb-bench
pip install vectordb-bench==1.0.10
```

#### Issue 2: `ImportError: libhdf5.so.103: cannot open shared object file`

**Solution:**
```bash
# Install HDF5 system libraries
sudo apt-get update
sudo apt-get install -y libhdf5-dev libhdf5-serial-dev

# Reinstall h5py
pip uninstall h5py
pip install h5py --no-binary h5py
```

#### Issue 3: PyArrow Installation Fails

**Solution:**
```bash
# Install system dependencies
sudo apt-get install -y libarrow-dev

# Use specific version
pip install pyarrow==14.0.0
```

---

## Downloading Datasets

### Step 1: Create Download Script

Create `/mnt/data/download_any_dataset.py`:

```python
#!/usr/bin/env python3
"""
Flexible dataset downloader for vectordb-bench
Downloads directly to NVMe at /mnt/data/datasets/

Usage examples:
  python download_any_dataset.py COHERE 1000000
  python download_any_dataset.py SIFT 5000000
  python download_any_dataset.py OPENAI 5000000
"""

import sys
import os
from vectordb_bench import config
from vectordb_bench.backend.dataset import Dataset

# Override the default dataset directory to use NVMe
config.DATASET_LOCAL_DIR = "/mnt/data/datasets"

def download_dataset(dataset_name, size):
    """Download a specific dataset with given size"""
    
    print("=" * 80)
    print(f"Downloading {dataset_name} dataset with {size:,} vectors")
    print("=" * 80)
    print(f"Target directory: {config.DATASET_LOCAL_DIR}")
    print()
    
    try:
        # Get the dataset enum
        dataset = Dataset[dataset_name.upper()]
        
        # Create manager with specified size
        manager = dataset.manager(size=size)
        
        # Print dataset info
        print(f"Dataset: {manager.data.name}")
        print(f"Size: {manager.data.size:,} vectors")
        print(f"Dimensions: {manager.data.dim}")
        print(f"Metric: {manager.data.metric_type}")
        print(f"\nStarting download...")
        print()
        
        # Download the dataset
        manager.prepare()
        
        print()
        print("=" * 80)
        print("✓ Download complete!")
        print("=" * 80)
        print(f"Location: {config.DATASET_LOCAL_DIR}/{manager.data.name.lower()}/")
        
        # Show disk usage
        os.system(f"df -h /mnt/data")
        
    except KeyError:
        print(f"Error: Unknown dataset '{dataset_name}'")
        print(f"Available datasets: {', '.join([d.name for d in Dataset])}")
        sys.exit(1)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python download_any_dataset.py <DATASET_NAME> <SIZE>")
        print()
        print("Available datasets and sizes:")
        print("  LAION:  100000000")
        print("  GIST:   100000, 1000000")
        print("  COHERE: 100000, 1000000, 10000000")
        print("  BIOASQ: 1000000, 10000000")
        print("  GLOVE:  1000000")
        print("  SIFT:   500000, 5000000")
        print("  OPENAI: 500000, 5000000")
        print()
        print("Examples:")
        print("  python download_any_dataset.py COHERE 1000000")
        print("  python download_any_dataset.py SIFT 5000000")
        sys.exit(1)
    
    dataset_name = sys.argv[1]
    size = int(sys.argv[2])
    
    download_dataset(dataset_name, size)
```

Make it executable:
```bash
chmod +x /mnt/data/download_any_dataset.py
```

### Step 2: Download Datasets

**Download COHERE datasets:**

```bash
# Activate environment
source /mnt/data/vectordb-bench-env/bin/activate

# Small dataset (100K vectors, ~300MB, ~30 seconds)
/mnt/data/vectordb-bench-env/bin/python /mnt/data/download_any_dataset.py COHERE 100000

# Medium dataset (1M vectors, ~3GB, ~2-3 minutes)
/mnt/data/vectordb-bench-env/bin/python /mnt/data/download_any_dataset.py COHERE 1000000

# Large dataset (10M vectors, ~42GB, ~15-20 minutes)
/mnt/data/vectordb-bench-env/bin/python /mnt/data/download_any_dataset.py COHERE 10000000
```

**Download OPENAI datasets:**

```bash
# Large dataset (5M vectors, ~42GB, ~15-20 minutes)
/mnt/data/vectordb-bench-env/bin/python /mnt/data/download_any_dataset.py OPENAI 5000000
```

**Download SIFT datasets:**

```bash
# Medium dataset (500K vectors, ~256MB)
/mnt/data/vectordb-bench-env/bin/python /mnt/data/download_any_dataset.py SIFT 500000

# Large dataset (5M vectors, ~2.5GB)
/mnt/data/vectordb-bench-env/bin/python /mnt/data/download_any_dataset.py SIFT 5000000
```

### Step 3: Verify Downloads

```bash
# Check downloaded files
ls -lh /mnt/data/datasets/

# Example output:
# drwxrwxr-x 3 ubuntu ubuntu 4.0K cohere/
# drwxrwxr-x 2 ubuntu ubuntu 4.0K openai_large_5m/

# Check specific dataset
ls -lh /mnt/data/datasets/cohere/cohere_large_10m/

# Expected files:
# - shuffle_train-00-of-10.parquet ... shuffle_train-09-of-10.parquet
# - test.parquet
# - neighbors.parquet
# - scalar_labels.parquet (optional)
```

---

## Converting Datasets

### Overview of Conversion Pipeline

```
Parquet (vectordb-bench) → HDF5 (intermediate) → Binary (Valkey format)
     42GB                      29GB                    29GB
```

### Step 1: Create Optimized Conversion Script

Copy the optimized conversion script to your Valkey directory:

**File: `/home/ubuntu/valkey/convert_parquet_to_hdf5_fast.py`**

This script is already in your repository with these optimizations:
- ✅ Batch processing (5000 vectors at a time)
- ✅ Direct PyArrow→NumPy conversion (no intermediate lists)
- ✅ Memory usage: <5GB regardless of dataset size
- ✅ Real-time progress logging

### Step 2: Create Wrapper Script

**File: `/home/ubuntu/valkey/convert_vectordb_dataset.sh`**

This all-in-one script handles both conversion steps.

### Step 3: Convert Datasets

**Convert COHERE 100K (fastest, for testing):**

```bash
cd /home/ubuntu/valkey

./convert_vectordb_dataset.sh \
    cohere/cohere_small_100k \
    cohere-small-100k \
    COSINE
```

**Expected output:**
```
======================================================================
Converting vectordb-bench dataset to Valkey binary format
======================================================================
Input:  /mnt/data/datasets/cohere/cohere_small_100k
HDF5:   /mnt/data/datasets/cohere-small-100k.hdf5
Binary: /mnt/data/build-datasets/cohere-small-100k.bin
Metric: COSINE

Step 1: Converting parquet to HDF5...
  Total time: ~15-20 seconds
  Output: 300MB HDF5 file

Step 2: Converting HDF5 to binary format...
  ✓ Dataset written to /mnt/data/build-datasets/cohere-small-100k.bin
  ✓ Verification passed

Step 3: Creating symlink in build-debug...
  ✓ Created: /home/ubuntu/valkey/build-debug/cohere-small-100k.bin

✓ Conversion complete!
```

**Convert COHERE 1M:**

```bash
./convert_vectordb_dataset.sh \
    cohere/cohere_medium_1m \
    cohere-medium-1m \
    COSINE
```

**Time:** ~1-2 minutes  
**Output:** 2.9GB binary file

**Convert COHERE 10M (large dataset):**

```bash
# Run in background with logging
nohup /mnt/data/vectordb-bench-env/bin/python3 -u \
    /home/ubuntu/valkey/convert_parquet_to_hdf5_fast.py \
    /mnt/data/datasets/cohere/cohere_large_10m \
    /mnt/data/datasets/cohere-large-10m.hdf5 \
    --name cohere-large-10m \
    > /tmp/cohere_10m_conversion.log 2>&1 &

# Monitor progress
tail -f /tmp/cohere_10m_conversion.log

# After parquet→HDF5 completes, convert to binary
python3 utils/datasets/prepare_binary.py \
    /mnt/data/datasets/cohere-large-10m.hdf5 \
    /mnt/data/build-datasets/cohere-large-10m.bin \
    --name cohere-large-10m \
    --metric COSINE \
    --max-neighbors 100

# Create symlink
ln -sf /mnt/data/build-datasets/cohere-large-10m.bin \
    /home/ubuntu/valkey/build-debug/cohere-large-10m.bin
```

**Time:** ~3-5 minutes  
**Output:** 29GB binary file  
**Memory:** <5GB peak usage

**Convert OPENAI 5M:**

```bash
# Run in background
nohup /mnt/data/vectordb-bench-env/bin/python3 -u \
    /home/ubuntu/valkey/convert_parquet_to_hdf5_fast.py \
    /mnt/data/datasets/openai_large_5m \
    /mnt/data/datasets/openai-large-5m.hdf5 \
    --name openai-large-5m \
    > /tmp/openai_5m_conversion.log 2>&1 &

# Monitor
tail -f /tmp/openai_5m_conversion.log

# Convert to binary
python3 utils/datasets/prepare_binary.py \
    /mnt/data/datasets/openai-large-5m.hdf5 \
    /mnt/data/build-datasets/openai-large-5m.bin \
    --name openai-large-5m \
    --metric COSINE \
    --max-neighbors 100

# Create symlink
ln -sf /mnt/data/build-datasets/openai-large-5m.bin \
    /home/ubuntu/valkey/build-debug/openai-large-5m.bin
```

**Time:** ~5-7 minutes  
**Output:** 28.6GB binary file  
**Memory:** <5GB peak usage

### Step 4: Verify Converted Datasets

```bash
# List all binary files
ls -lh /mnt/data/build-datasets/

# Check symlinks
ls -lh /home/ubuntu/valkey/build-debug/*.bin

# Verify binary format
hexdump -C /mnt/data/build-datasets/cohere-small-100k.bin | head -2
# Should show magic: 01 b0 cd de (0xDECDB001 in little-endian)
```

---

## Using Datasets with Valkey

### Step 1: Configure test_multi_dataset.sh

The script already includes configurations for all datasets:

```bash
# Location: /home/ubuntu/valkey/vector-testing/test_multi_dataset.sh

# Dataset configurations (already in script):
DATASET_CONFIG["cohere-small-100k"]="zvec_cohere100k:,768,100000,100"
DATASET_CONFIG["cohere-medium-1m"]="zvec_cohere1m:,768,1000000,100"
DATASET_CONFIG["cohere-large-10m"]="zvec_cohere10m:,768,10000000,100"
DATASET_CONFIG["openai-large-5m"]="zvec_openai5m:,1536,5000000,100"
```

### Step 2: Set Environment Variables

```bash
# Set your Valkey/Redis cluster endpoint
export HOST=ec-search-zvi-ec-cmd.ajfdds.ng.0001.euw1devo.cache.amazonaws.com

# Or for localhost testing
export HOST=localhost
```

### Step 3: Run Benchmark Tests

**Test small dataset (quick validation):**

```bash
cd /home/ubuntu/valkey/vector-testing

./test_multi_dataset.sh --dataset cohere-small-100k --ef-search 200
```

**Test medium dataset:**

```bash
./test_multi_dataset.sh --dataset cohere-medium-1m --ef-search 150,250,500
```

**Test large datasets:**

```bash
# COHERE 10M (768 dimensions)
./test_multi_dataset.sh --dataset cohere-large-10m --ef-search 150,250,500

# OPENAI 5M (1536 dimensions)
./test_multi_dataset.sh --dataset openai-large-5m --ef-search 150,250,500
```

**Run multiple datasets:**

```bash
./test_multi_dataset.sh \
    --dataset cohere-small-100k,cohere-medium-1m,cohere-large-10m \
    --ef-search 150,250,500
```

### Step 4: View Results

```bash
# Results are saved in ef_search_results/ directory
ls -lh /home/ubuntu/valkey/ef_search_results/

# View specific results
cat ef_search_results_cohere-large-10m_*.csv
```

**Example output:**
```csv
ef_search,recall@100,avg_latency_ms,p50_ms,p95_ms,p99_ms,qps
150,0.9524,12.34,11.2,18.5,25.3,81.0
250,0.9756,18.67,17.1,27.8,35.2,53.5
500,0.9912,35.42,33.4,51.2,67.8,28.2
```

---

## CRITICAL: ID Shuffling Bug Fix

### Background

**Issue Discovered**: October 19, 2025

VectorDB-bench datasets store training vectors in **shuffled order** in parquet files with an `id` column indicating the original vector ID. The ground truth `neighbors_id` references these **original IDs**, not row indices.

**The Bug**: The initial version of `convert_parquet_to_hdf5_fast.py` ignored the `id` column and stored vectors by row index, causing a complete mismatch between vector IDs and their actual data. This resulted in:
- ❌ **Incorrect recall measurements** (ground truth pointed to wrong vectors)
- ❌ **Invalid benchmark results** (comparing against mismatched neighbors)
- ❌ **Database corruption** (wrong vectors inserted at each ID)

### How the Bug Was Discovered

During benchmarking, recall@100 results were **better than the ground truth**, which is impossible. Investigation revealed:

1. **Query 503** ground truth said closest neighbor was vector ID **16696** with distance **0.212**
2. **Computing distance directly** showed vector ID **97737** had distance **0.074** (3x closer!)
3. **Vector ID 97737 wasn't even in the ground truth** top 1000 neighbors

This led to discovering that:
- In the **shuffled parquet**: Vector ID 16696 is at **row index 97737**
- Our conversion was storing vectors by row index, losing the ID mapping
- Ground truth neighbor "ID 16696" was pointing to the wrong vector

### The Fix

**File**: `convert_parquet_to_hdf5_fast.py`

**Changed**: Phase 3 conversion logic

**Before (WRONG)**:
```python
# Streamed vectors in shuffled order, ignoring IDs
for batch in parquet_file.iter_batches(columns=['emb']):
    vectors = batch['emb'].to_numpy()
    train_dset[offset:offset + len(vectors)] = vectors
```

**After (CORRECT)**:
```python
# Load vectors with their IDs, then sort by ID to restore original order
table = pq.read_table(train_file, columns=['id', 'emb'])
ids = table['id'].to_numpy()
vectors = table['emb'].to_numpy()

# Sort by ID to restore sequential order
sort_indices = np.argsort(ids)
sorted_vectors = vectors[sort_indices]

# Now vector at index 0 has ID 0, index 16696 has ID 16696, etc.
train_dset[:] = sorted_vectors
```

### Verification

After the fix, distances are correct:

```python
# Query 503 ground truth neighbors (after fix):
Rank  1: Vector ID  16696, distance = 0.074447  ✓ (was 0.212477)
Rank  2: Vector ID  92575, distance = 0.084238  ✓ (was 0.276911)
Rank  3: Vector ID  27903, distance = 0.084536  ✓ (was 0.287371)
```

All distances are now in the expected **0.07-0.11** range for top neighbors.

### Required Actions After Update

If you converted datasets **before October 19, 2025**, you **MUST reconvert ALL datasets**:

#### Step 1: Update Conversion Script

```bash
cd /home/ubuntu/valkey
git pull  # Get the fixed version
```

Or manually update `convert_parquet_to_hdf5_fast.py` with the fix above.

#### Step 2: Reconvert HDF5 Files

```bash
cd /home/ubuntu/valkey

# Small dataset (100K) - ~30 seconds
rm -f /mnt/data/datasets/cohere-small-100k.hdf5
/mnt/data/vectordb-bench-env/bin/python3 -u convert_parquet_to_hdf5_fast.py \
    /mnt/data/datasets/cohere/cohere_small_100k \
    /mnt/data/datasets/cohere-small-100k.hdf5 \
    --name cohere-small-100k

# Medium dataset (1M) - ~1-2 minutes
rm -f /mnt/data/datasets/cohere-medium-1m.hdf5
/mnt/data/vectordb-bench-env/bin/python3 -u convert_parquet_to_hdf5_fast.py \
    /mnt/data/datasets/cohere/cohere_medium_1m \
    /mnt/data/datasets/cohere-medium-1m.hdf5 \
    --name cohere-medium-1m

# Large dataset (10M) - ~5-7 minutes
rm -f /mnt/data/datasets/cohere-large-10m.hdf5
/mnt/data/vectordb-bench-env/bin/python3 -u convert_parquet_to_hdf5_fast.py \
    /mnt/data/datasets/cohere/cohere_large_10m \
    /mnt/data/datasets/cohere-large-10m.hdf5 \
    --name cohere-large-10m

# OpenAI 5M - ~6-8 minutes
rm -f /mnt/data/datasets/openai-large-5m.hdf5
/mnt/data/vectordb-bench-env/bin/python3 -u convert_parquet_to_hdf5_fast.py \
    /mnt/data/datasets/openai_large_5m \
    /mnt/data/datasets/openai-large-5m.hdf5 \
    --name openai-large-5m
```

#### Step 3: Recreate Binary Files

Binary files will be **automatically overwritten** (no need to delete):

```bash
cd /home/ubuntu/valkey

# Cohere Small 100K
python3 utils/datasets/prepare_binary.py \
    /mnt/data/datasets/cohere-small-100k.hdf5 \
    /mnt/data/build-datasets/cohere-small-100k.bin \
    --name cohere-small-100k \
    --metric COSINE \
    --max-neighbors 100

# Cohere Medium 1M
python3 utils/datasets/prepare_binary.py \
    /mnt/data/datasets/cohere-medium-1m.hdf5 \
    /mnt/data/build-datasets/cohere-medium-1m.bin \
    --name cohere-medium-1m \
    --metric COSINE \
    --max-neighbors 100

# Cohere Large 10M
python3 utils/datasets/prepare_binary.py \
    /mnt/data/datasets/cohere-large-10m.hdf5 \
    /mnt/data/build-datasets/cohere-large-10m.bin \
    --name cohere-large-10m \
    --metric COSINE \
    --max-neighbors 100

# OpenAI Large 5M
python3 utils/datasets/prepare_binary.py \
    /mnt/data/datasets/openai-large-5m.hdf5 \
    /mnt/data/build-datasets/openai-large-5m.bin \
    --name openai-large-5m \
    --metric COSINE \
    --max-neighbors 100
```

**Note**: Symlinks in `build-debug/` automatically point to the new files.

#### Step 4: Flush Database and Re-insert

The database contains **wrong vectors** from the buggy conversion. You MUST:

1. **Drop all indexes** (test_multi_dataset.sh does this automatically)
2. **Re-run benchmarks** to insert correct vectors

```bash
# The script will automatically:
# 1. Drop old indexes (FT.DROPINDEX)
# 2. Insert vectors from corrected binary files
# 3. Run fresh benchmarks with valid ground truth

export HOST=your-valkey-endpoint.amazonaws.com
cd /home/ubuntu/valkey/vector-testing
./test_multi_dataset.sh --dataset cohere-small-100k --ef-search 150,250,500
```

### Validation

Verify the fix worked by checking ground truth distances:

```python
import h5py
import numpy as np

with h5py.File('/mnt/data/datasets/cohere-small-100k.hdf5', 'r') as f:
    vectors = f['train'][:]
    queries = f['test'][:]
    neighbors = f['neighbors'][:]
    
    # Pick any query
    query_idx = 503
    query = queries[query_idx]
    
    # Check first neighbor distance
    neighbor_id = neighbors[query_idx, 0]
    neighbor_vec = vectors[neighbor_id]
    
    # Calculate cosine distance
    cos_sim = np.dot(query, neighbor_vec) / (np.linalg.norm(query) * np.linalg.norm(neighbor_vec))
    cos_dist = 1 - cos_sim
    
    print(f"First neighbor distance: {cos_dist:.6f}")
    # Should be < 0.15 for COHERE datasets
    # If > 0.20, the bug still exists!
```

### Impact Assessment

**Before Fix**:
- ❌ All benchmark results were **invalid**
- ❌ Recall measurements were **meaningless**
- ❌ Database had **wrong vectors** at each ID
- ❌ Ground truth comparisons were **completely broken**

**After Fix**:
- ✅ Vectors stored in **correct ID order**
- ✅ Ground truth neighbors **correctly mapped**
- ✅ Recall measurements are **valid**
- ✅ Benchmark results are **trustworthy**

---

## Troubleshooting

### Issue 1: Out of Memory During Conversion

**Symptoms:**
```
Killed
Process terminated
```

**Solutions:**

1. **Check available RAM:**
```bash
free -h
```

2. **Monitor memory during conversion:**
```bash
watch -n 1 'ps aux | grep convert_parquet | awk "{print \$6/1024/1024 \" GB\"}"'
```

3. **Reduce batch size** in `convert_parquet_to_hdf5_fast.py`:
```python
# Change from 5000 to 2500
BATCH_SIZE = 2500
```

4. **Use swap if needed:**
```bash
# Create 32GB swap file
sudo fallocate -l 32G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
```

### Issue 2: Disk Space Issues

**Check available space:**
```bash
df -h /mnt/data
```

**Free up space:**
```bash
# Remove HDF5 intermediate files after conversion
rm /mnt/data/datasets/*.hdf5

# Keep only binary files
ls -lh /mnt/data/build-datasets/
```

**Storage requirements:**
- Parquet files: ~42GB (COHERE 10M or OPENAI 5M)
- HDF5 intermediate: ~29GB (can be deleted after)
- Binary final: ~29GB
- **Total during conversion**: ~100GB
- **Total after cleanup**: ~71GB

### Issue 3: Conversion Hangs or Crashes

**Check process status:**
```bash
ps aux | grep convert_parquet
```

**Check logs:**
```bash
tail -f /tmp/*_conversion.log
```

**Kill and restart:**
```bash
# Kill process
pkill -f convert_parquet_to_hdf5_fast

# Remove partial HDF5 file
rm -f /mnt/data/datasets/cohere-large-10m.hdf5

# Restart conversion
nohup /mnt/data/vectordb-bench-env/bin/python3 -u \
    /home/ubuntu/valkey/convert_parquet_to_hdf5_fast.py \
    /mnt/data/datasets/cohere/cohere_large_10m \
    /mnt/data/datasets/cohere-large-10m.hdf5 \
    --name cohere-large-10m \
    > /tmp/cohere_10m_conversion.log 2>&1 &
```

### Issue 4: "HOST environment variable not set"

**Solution:**
```bash
# Set the HOST variable
export HOST=your-valkey-endpoint.amazonaws.com

# Or add to your ~/.bashrc
echo 'export HOST=your-valkey-endpoint.amazonaws.com' >> ~/.bashrc
source ~/.bashrc
```

### Issue 5: Dataset Binary Not Found

**Check symlinks:**
```bash
ls -lh /home/ubuntu/valkey/build-debug/*.bin
```

**Recreate symlink:**
```bash
ln -sf /mnt/data/build-datasets/cohere-large-10m.bin \
    /home/ubuntu/valkey/build-debug/cohere-large-10m.bin
```

### Issue 6: Python Package Import Errors

**Reinstall all dependencies:**
```bash
source /mnt/data/vectordb-bench-env/bin/activate
pip uninstall -y vectordb-bench h5py pandas pyarrow numpy
pip install vectordb-bench==1.0.10 h5py pandas pyarrow numpy
```

### Issue 7: Slow Download Speed

**Use wget/curl for manual download** (if available):
```bash
# Some datasets may have direct download URLs
# Check vectordb-bench documentation for alternatives
```

**Check network:**
```bash
# Test download speed
wget -O /dev/null http://speedtest.wdc01.softlayer.com/downloads/test100.zip
```

---

## Dataset Reference

### Available Datasets

| Dataset | Size | Vectors | Dimensions | Metric | Parquet | Binary | Download Time | Convert Time |
|---------|------|---------|------------|--------|---------|--------|---------------|--------------|
| **COHERE Small** | 100K | 100,000 | 768 | COSINE | ~300MB | 297MB | ~30s | ~20s |
| **COHERE Medium** | 1M | 1,000,000 | 768 | COSINE | ~3GB | 2.9GB | ~2min | ~1min |
| **COHERE Large** | 10M | 10,000,000 | 768 | COSINE | ~42GB | 29GB | ~15min | ~4min |
| **OPENAI Large** | 5M | 5,000,000 | 1536 | COSINE | ~42GB | 28.6GB | ~15min | ~6min |
| **SIFT Medium** | 500K | 500,000 | 128 | L2 | ~256MB | ~245MB | ~1min | ~30s |
| **SIFT Large** | 5M | 5,000,000 | 128 | L2 | ~2.5GB | ~2.4GB | ~5min | ~2min |
| **GIST Small** | 100K | 100,000 | 960 | L2 | ~400MB | ~380MB | ~1min | ~30s |
| **GIST Large** | 1M | 1,000,000 | 960 | L2 | ~3.8GB | ~3.6GB | ~5min | ~2min |

### File Locations

```
/mnt/data/
├── datasets/                           # Downloaded parquet files
│   ├── cohere/
│   │   ├── cohere_small_100k/
│   │   ├── cohere_medium_1m/
│   │   └── cohere_large_10m/
│   ├── openai_large_5m/
│   ├── cohere-small-100k.hdf5         # Intermediate HDF5
│   ├── cohere-medium-1m.hdf5
│   ├── cohere-large-10m.hdf5
│   └── openai-large-5m.hdf5
│
├── build-datasets/                     # Final binary files
│   ├── cohere-small-100k.bin
│   ├── cohere-medium-1m.bin
│   ├── cohere-large-10m.bin
│   └── openai-large-5m.bin
│
└── vectordb-bench-env/                 # Python environment
    └── bin/python

/home/ubuntu/valkey/
├── build-debug/                        # Symlinks to binaries
│   ├── cohere-small-100k.bin -> /mnt/data/build-datasets/cohere-small-100k.bin
│   ├── cohere-medium-1m.bin -> /mnt/data/build-datasets/cohere-medium-1m.bin
│   ├── cohere-large-10m.bin -> /mnt/data/build-datasets/cohere-large-10m.bin
│   └── openai-large-5m.bin -> /mnt/data/build-datasets/openai-large-5m.bin
│
├── vector-testing/
│   └── test_multi_dataset.sh          # Benchmark runner
│
├── convert_parquet_to_hdf5_fast.py    # Optimized converter
└── convert_vectordb_dataset.sh        # Wrapper script
```

### Binary Format Specification

Valkey uses a custom binary format defined in `src/dataset_api.h`:

```c
#define DATASET_MAGIC 0xDECDB001

typedef struct {
    uint32_t magic;                      // 0xDECDB001
    uint32_t version;                    // 1
    char name[256];                      // Dataset name
    uint32_t dim;                        // Vector dimensions
    uint32_t metric;                     // Distance metric (0=L2, 1=COSINE, 2=IP)
    uint64_t num_vectors;                // Training vectors count
    uint64_t num_queries;                // Test queries count
    uint32_t num_neighbors;              // Ground truth neighbors per query
    uint64_t vectors_offset;             // Offset to training vectors
    uint64_t queries_offset;             // Offset to test queries
    uint64_t ground_truth_offset;        // Offset to ground truth neighbors
} dataset_header_t;
```

**File layout:**
1. Header (4KB aligned)
2. Training vectors (num_vectors × dim × 4 bytes)
3. Test queries (num_queries × dim × 4 bytes)
4. Ground truth (num_queries × num_neighbors × 8 bytes)

---

## Quick Reference Commands

### Download Dataset
```bash
source /mnt/data/vectordb-bench-env/bin/activate
/mnt/data/vectordb-bench-env/bin/python /mnt/data/download_any_dataset.py COHERE 1000000
```

### Convert Dataset
```bash
cd /home/ubuntu/valkey
./convert_vectordb_dataset.sh cohere/cohere_medium_1m cohere-medium-1m COSINE
```

### Run Benchmark
```bash
export HOST=your-endpoint.amazonaws.com
cd /home/ubuntu/valkey/vector-testing
./test_multi_dataset.sh --dataset cohere-medium-1m --ef-search 150,250,500
```

### Check Status
```bash
# Disk usage
df -h /mnt/data

# Binary files
ls -lh /mnt/data/build-datasets/

# Symlinks
ls -lh /home/ubuntu/valkey/build-debug/*.bin

# Conversion logs
tail -f /tmp/*_conversion.log
```

---

## Performance Tips

1. **Use NVMe storage** for better I/O performance
2. **Run large conversions in background** with nohup
3. **Monitor memory usage** during conversion
4. **Delete HDF5 files** after binary conversion to save space
5. **Use multiple terminals** to monitor progress while working
6. **Start with small datasets** (100K) to validate workflow
7. **Keep parquet files** - they can be reconverted if needed

---

## Summary Checklist

- [ ] NVMe drive mounted at `/mnt/data`
- [ ] Python virtual environment created
- [ ] vectordb-bench installed (`pip install vectordb-bench==1.0.10`)
- [ ] Dependencies installed (h5py, pandas, pyarrow, numpy)
- [ ] Download script created and tested
- [ ] Conversion scripts in place
- [ ] At least one dataset downloaded
- [ ] Dataset converted to binary format
- [ ] Symlink created in build-debug/
- [ ] HOST environment variable set
- [ ] test_multi_dataset.sh tested successfully

---

## Support and Resources

- **Valkey Repository**: https://github.com/valkey-io/valkey
- **VectorDB-Bench**: https://github.com/zilliztech/VectorDBBench
- **HDF5 Documentation**: https://docs.h5py.org/
- **PyArrow Documentation**: https://arrow.apache.org/docs/python/

---

**Last Updated**: October 18, 2025  
**Version**: 1.0  
**Tested On**: Ubuntu 22.04, Python 3.10, vectordb-bench 1.0.10
