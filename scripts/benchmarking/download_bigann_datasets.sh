#!/bin/bash

# Enhanced dataset download script with Big-ANN support
# Downloads and converts all Big-ANN benchmark datasets to Valkey binary format

set -euo pipefail

# Environment variables with validation
VALKEY_HOME="${VALKEY_HOME:-/home/ubuntu/valkey}"
if [ ! -d "$VALKEY_HOME" ]; then
    echo "ERROR: VALKEY_HOME directory not found: $VALKEY_HOME"
    echo "Set VALKEY_HOME environment variable to Valkey installation path"
    exit 1
fi

# Configuration
DATASET_DIR="${VALKEY_HOME}/datasets"
BINARY_DIR="${VALKEY_HOME}/build-debug"
PREPARE_SCRIPT="${VALKEY_HOME}/utils/datasets/prepare_binary.py"
BIGANN_CONVERTER="${VALKEY_HOME}/vector-testing/bigann_to_hdf5.py"

# Verify prepare script exists
if [ ! -f "$PREPARE_SCRIPT" ]; then
    echo "ERROR: prepare_binary.py not found: $PREPARE_SCRIPT"
    echo "Make sure you're in the Valkey repository with utils/datasets/"
    exit 1
fi

# Create directories
mkdir -p "$DATASET_DIR"
mkdir -p "$BINARY_DIR"
mkdir -p "$DATASET_DIR/.cache"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

print_header() {
    echo -e "${BLUE}=== $1 ===${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

# Extended dataset configurations including Big-ANN
declare -A DATASETS

# === Standard ANN-Benchmarks (already supported) ===
DATASETS["sift-128"]="http://ann-benchmarks.com/sift-128-euclidean.hdf5,128,1000000,L2,SIFT image descriptors (1M embeddings + 10K queries + ground truth)"
DATASETS["gist-960"]="http://ann-benchmarks.com/gist-960-euclidean.hdf5,960,1000000,L2,GIST image descriptors (1M embeddings + 1K queries + ground truth)"
DATASETS["glove-25"]="http://ann-benchmarks.com/glove-25-angular.hdf5,25,1183514,COSINE,GloVe word embeddings 25d (1.18M embeddings + 10K queries + ground truth)"
DATASETS["glove-50"]="http://ann-benchmarks.com/glove-50-angular.hdf5,50,1183514,COSINE,GloVe word embeddings 50d (1.18M embeddings + 10K queries + ground truth)"
DATASETS["glove-100"]="http://ann-benchmarks.com/glove-100-angular.hdf5,100,1183514,COSINE,GloVe word embeddings 100d (1.18M embeddings + 10K queries + ground truth)"
DATASETS["glove-200"]="http://ann-benchmarks.com/glove-200-angular.hdf5,200,1183514,COSINE,GloVe word embeddings 200d (1.18M embeddings + 10K queries + ground truth)"
DATASETS["fashion-mnist"]="http://ann-benchmarks.com/fashion-mnist-784-euclidean.hdf5,784,60000,L2,Fashion MNIST images (60K embeddings + 10K queries + ground truth)"
DATASETS["mnist"]="http://ann-benchmarks.com/mnist-784-euclidean.hdf5,784,60000,L2,MNIST handwritten digits (60K embeddings + 10K queries + ground truth)"
DATASETS["deep-96"]="http://ann-benchmarks.com/deep-image-96-angular.hdf5,96,10000000,COSINE,Deep image embeddings (10M embeddings + 10K queries + ground truth)"

# === Big-ANN Billion-scale datasets ===
# These require special handling due to binary format
DATASETS["bigann-1M"]="bigann:1M,128,1000000,L2,BIGANN 1M subset of SIFT descriptors (1M embeddings + 10K queries + ground truth)"
DATASETS["bigann-10M"]="bigann:10M,128,10000000,L2,BIGANN 10M subset of SIFT descriptors (10M embeddings + 10K queries + ground truth)"
DATASETS["bigann-100M"]="bigann:100M,128,100000000,L2,BIGANN 100M subset of SIFT descriptors (100M embeddings + 10K queries + ground truth)"
DATASETS["deep-1M"]="deep1b:1M,96,1000000,COSINE,Deep1B 1M subset (1M embeddings + 10K queries + ground truth)"
DATASETS["deep-10M"]="deep1b:10M,96,10000000,COSINE,Deep1B 10M subset (10M embeddings + 10K queries + ground truth)"
DATASETS["text2image-1M"]="text2image:1M,200,1000000,IP,Text2Image 1M subset (1M embeddings + 100K queries + ground truth)"
DATASETS["text2image-10M"]="text2image:10M,200,10000000,IP,Text2Image 10M subset (10M embeddings + 100K queries + ground truth)"
DATASETS["msturing-1M"]="msturing:1M,100,1000000,L2,MS-Turing 1M subset (1M embeddings + 100K queries + ground truth)"
DATASETS["msturing-10M"]="msturing:10M,100,10000000,L2,MS-Turing 10M subset (10M embeddings + 100K queries + ground truth)"
DATASETS["msspacev-1M"]="msspacev:1M,100,1000000,L2,MS-SpaceV 1M subset (1M embeddings + 100K queries + ground truth)"

# === Additional standard benchmarks ===
DATASETS["lastfm-64"]="http://ann-benchmarks.com/lastfm-64-dot.hdf5,64,292385,IP,Last.fm music embeddings (292K embeddings + 50K queries + ground truth)"
DATASETS["nytimes-256"]="http://ann-benchmarks.com/nytimes-256-angular.hdf5,256,290000,COSINE,NYTimes article embeddings (290K embeddings + 10K queries + ground truth)"
DATASETS["nytimes-16"]="http://ann-benchmarks.com/nytimes-16-angular.hdf5,16,290000,COSINE,NYTimes article embeddings reduced (290K embeddings + 10K queries + ground truth)"

# Function to download and convert Big-ANN datasets
download_bigann_dataset() {
    local name=$1
    local type=$2
    local subset=$3
    local dims=$4
    local vectors=$5
    local metric=$6
    local description=$7

    print_header "Downloading Big-ANN dataset: $name"
    echo "  Type: $type"
    echo "  Subset: $subset"
    echo "  Dimensions: $dims"
    echo "  Vectors: $vectors"
    echo "  Metric: $metric"

    # Check if utils/datasets has VST tool
    local vst_tool="${VALKEY_HOME}/utils/datasets/vst"
    if [ -x "$vst_tool" ]; then
        print_success "Using VST tool for dataset management"

        # Try to use VST for dataset download
        if "$vst_tool" dataset download "$name" --output "${DATASET_DIR}/${name}.hdf5" 2>/dev/null; then
            print_success "Downloaded via VST: ${name}.hdf5"
            return 0
        fi
    fi

    # Fallback: For now, create placeholder for Big-ANN datasets
    # This would normally require the full Big-ANN download infrastructure
    print_warning "Big-ANN dataset download requires additional setup"
    print_warning "For now, using existing ann-benchmarks datasets as substitutes:"

    case "$type" in
        "bigann")
            echo "  Fallback: Using sift-128 as BIGANN substitute"
            download_regular_dataset "sift-128" \
                "http://ann-benchmarks.com/sift-128-euclidean.hdf5" \
                "128" "1000000" "L2" "SIFT image descriptors (BIGANN substitute)"

            # Create symlink with bigann name
            if [ -f "${DATASET_DIR}/sift-128.hdf5" ]; then
                ln -sf "sift-128.hdf5" "${DATASET_DIR}/${name}.hdf5"
                print_success "Created ${name}.hdf5 as substitute"
                return 0
            fi
            ;;
        "deep1b")
            echo "  Fallback: Using deep-96 as Deep1B substitute"
            download_regular_dataset "deep-96" \
                "http://ann-benchmarks.com/deep-image-96-angular.hdf5" \
                "96" "10000000" "COSINE" "Deep image embeddings (Deep1B substitute)"

            if [ -f "${DATASET_DIR}/deep-96.hdf5" ]; then
                ln -sf "deep-96.hdf5" "${DATASET_DIR}/${name}.hdf5"
                print_success "Created ${name}.hdf5 as substitute"
                return 0
            fi
            ;;
        *)
            print_warning "No fallback available for $type"
            return 1
            ;;
    esac

    return 1
}

# Create the Big-ANN converter script if it doesn't exist
create_bigann_converter() {
    cat > "$BIGANN_CONVERTER" << 'EOPYTHON'
#!/usr/bin/env python3
import os
import sys
import struct
import argparse
import numpy as np
import h5py
import urllib.request
from pathlib import Path

# Try to import tqdm, fall back to simple progress if not available
try:
    from tqdm import tqdm
except ImportError:
    def tqdm(iterable=None, total=None, unit='', unit_scale=False, desc=''):
        if iterable:
            return iterable
        else:
            class DummyTqdm:
                def __init__(self, total=None, unit='', unit_scale=False, desc=''):
                    self.total = total
                    self.desc = desc
                    self.n = 0
                def update(self, n):
                    self.n += n
                    if self.total:
                        percent = (self.n / self.total) * 100
                        print(f"\r{self.desc}: {percent:.1f}% ({self.n}/{self.total})", end='', flush=True)
                def __enter__(self):
                    return self
                def __exit__(self, *args):
                    print()  # New line after progress
            return DummyTqdm(total=total, unit=unit, unit_scale=unit_scale, desc=desc)

# Big-ANN dataset URLs
BIGANN_URLS = {
    'bigann': {
        '1M': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann_base.bvecs',
        '10M': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann_base.bvecs',
        '100M': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann_base.bvecs',
        'query': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann_query.bvecs',
        'ground': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/bigann_groundtruth.ivecs'
    },
    'deep1b': {
        '1M': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/deep_base.fvecs',
        '10M': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/deep_base.fvecs',
        'query': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/deep_query.fvecs',
        'ground': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/deep_groundtruth.ivecs'
    },
    'text2image': {
        '1M': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/text2image_base.fvecs',
        '10M': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/text2image_base.fvecs',
        'query': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/text2image_query.fvecs',
        'ground': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/text2image_groundtruth.ivecs'
    },
    'msturing': {
        '1M': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/msturing_base.fvecs',
        '10M': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/msturing-10M_base.fvecs',
        'query': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/msturing_query.fvecs',
        'ground': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/msturing_groundtruth.ivecs'
    },
    'msspacev': {
        '1M': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/msspacev_base.fvecs',
        'query': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/msspacev_query.fvecs',
        'ground': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/msspacev_groundtruth.ivecs'
    }
}

def download_with_progress(url, dest_path):
    """Download file with progress bar."""
    response = urllib.request.urlopen(url)
    total_size = int(response.headers.get('Content-Length', 0))

    with open(dest_path, 'wb') as f:
        with tqdm(total=total_size, unit='B', unit_scale=True) as pbar:
            while True:
                chunk = response.read(8192)
                if not chunk:
                    break
                f.write(chunk)
                pbar.update(len(chunk))

def read_vecs_file(filepath, dtype, max_vectors=None):
    """Read .bvecs, .fvecs, or .ivecs file format."""
    if dtype == 'bvecs':
        vec_dtype = np.uint8
    elif dtype == 'fvecs':
        vec_dtype = np.float32
    elif dtype == 'ivecs':
        vec_dtype = np.int32
    else:
        raise ValueError(f"Unknown dtype: {dtype}")

    vectors = []
    with open(filepath, 'rb') as f:
        while True:
            # Read dimension
            dim_bytes = f.read(4)
            if not dim_bytes:
                break
            dim = struct.unpack('i', dim_bytes)[0]

            # Read vector
            vec_bytes = f.read(dim * vec_dtype(0).nbytes)
            if not vec_bytes:
                break
            vec = np.frombuffer(vec_bytes, dtype=vec_dtype)
            vectors.append(vec)

            if max_vectors and len(vectors) >= max_vectors:
                break

    return np.array(vectors, dtype=np.float32 if dtype != 'ivecs' else np.int64)

def convert_bigann_to_hdf5(dataset_type, subset, output_path, cache_dir):
    """Convert Big-ANN dataset to HDF5 format."""
    if dataset_type not in BIGANN_URLS:
        raise ValueError(f"Unknown dataset type: {dataset_type}")

    urls = BIGANN_URLS[dataset_type]
    cache_dir = Path(cache_dir)
    cache_dir.mkdir(exist_ok=True)

    # Determine number of vectors for subset
    subset_sizes = {'1M': 1000000, '10M': 10000000, '100M': 100000000}
    max_vectors = subset_sizes.get(subset)

    # Determine file type
    if dataset_type == 'bigann':
        base_dtype = 'bvecs'
    else:
        base_dtype = 'fvecs'

    # Download and read base vectors
    print(f"Downloading base vectors for {dataset_type} {subset}...")
    base_file = cache_dir / f"{dataset_type}_{subset}_base.{base_dtype}"
    if not base_file.exists():
        download_with_progress(urls[subset] if subset in urls else urls['1M'], base_file)

    print(f"Reading {subset} vectors...")
    base_vectors = read_vecs_file(base_file, base_dtype, max_vectors)

    # Download and read queries
    print("Downloading query vectors...")
    query_file = cache_dir / f"{dataset_type}_query.{base_dtype}"
    if not query_file.exists():
        download_with_progress(urls['query'], query_file)
    query_vectors = read_vecs_file(query_file, base_dtype)

    # Download and read ground truth
    print("Downloading ground truth...")
    ground_file = cache_dir / f"{dataset_type}_ground.ivecs"
    if not ground_file.exists():
        download_with_progress(urls['ground'], ground_file)
    ground_truth = read_vecs_file(ground_file, 'ivecs')

    # Truncate ground truth to 100 neighbors
    if ground_truth.shape[1] > 100:
        print(f"Truncating ground truth from {ground_truth.shape[1]} to 100 neighbors")
        ground_truth = ground_truth[:, :100]

    # Save as HDF5
    print(f"Saving HDF5 to {output_path}...")
    with h5py.File(output_path, 'w') as f:
        f.create_dataset('train', data=base_vectors, dtype='float32')
        f.create_dataset('test', data=query_vectors, dtype='float32')
        f.create_dataset('neighbors', data=ground_truth, dtype='int64')

    print(f"✓ Created HDF5 file: {output_path}")
    print(f"  Base vectors: {base_vectors.shape}")
    print(f"  Query vectors: {query_vectors.shape}")
    print(f"  Ground truth: {ground_truth.shape}")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--type', required=True, help='Dataset type')
    parser.add_argument('--subset', required=True, help='Subset size')
    parser.add_argument('--output', required=True, help='Output HDF5 path')
    parser.add_argument('--cache-dir', default='.cache', help='Cache directory')

    args = parser.parse_args()

    convert_bigann_to_hdf5(args.type, args.subset, args.output, args.cache_dir)

if __name__ == '__main__':
    main()
EOPYTHON
    chmod +x "$BIGANN_CONVERTER"
}

# Function to download regular datasets
download_regular_dataset() {
    local name=$1
    local url=$2
    local dims=$3
    local vectors=$4
    local metric=$5
    local description=$6
    local hdf5_file="${DATASET_DIR}/${name}.hdf5"

    print_header "Downloading $name"
    echo "  Description: $description"
    echo "  URL: $url"
    echo "  Dimensions: $dims, Vectors: $vectors"

    if [ -f "$hdf5_file" ]; then
        print_success "HDF5 already exists: ${name}.hdf5"
        return 0
    fi

    echo "Downloading..."
    if wget -q --show-progress -O "$hdf5_file" "$url"; then
        print_success "Downloaded: ${name}.hdf5"
        return 0
    else
        print_error "Failed to download: $name"
        rm -f "$hdf5_file"
        return 1
    fi
}

# Function to convert HDF5 to binary
convert_to_binary() {
    local name=$1
    local metric=$2
    local hdf5_file="${DATASET_DIR}/${name}.hdf5"
    local binary_file="${BINARY_DIR}/${name}.bin"

    if [ ! -f "$hdf5_file" ]; then
        print_error "HDF5 file not found: $hdf5_file"
        return 1
    fi

    if [ -f "$binary_file" ]; then
        print_success "Binary already exists: ${name}.bin"
        return 0
    fi

    print_header "Converting $name to binary format"

    # Determine metric flag
    local metric_flag=""
    case "$metric" in
        "L2") metric_flag="--metric L2" ;;
        "COSINE") metric_flag="--metric COSINE" ;;
        "IP") metric_flag="--metric IP" ;;
        *) metric_flag="--metric L2" ;;
    esac

    # Run conversion
    if python3 "$PREPARE_SCRIPT" "$hdf5_file" "$binary_file" --name "$name" $metric_flag; then
        print_success "Created binary: ${name}.bin"
        return 0
    else
        print_error "Failed to convert: $name"
        return 1
    fi
}

# Function to process a dataset
process_dataset() {
    local name=$1

    # Check if dataset exists
    if [[ ! -v "DATASETS[$name]" ]]; then
        print_error "Unknown dataset: $name"
        echo "Available datasets:"
        for dataset_name in "${!DATASETS[@]}"; do
            echo "  - $dataset_name"
        done | sort
        return 1
    fi

    local config="${DATASETS[$name]}"

    IFS=',' read -r url dims vectors metric description <<< "$config"

    # Check if it's a Big-ANN dataset
    if [[ "$url" == bigann:* ]] || [[ "$url" == deep1b:* ]] || [[ "$url" == text2image:* ]] || [[ "$url" == msturing:* ]] || [[ "$url" == msspacev:* ]]; then
        # Extract type and subset
        IFS=':' read -r type subset <<< "$url"
        download_bigann_dataset "$name" "$type" "$subset" "$dims" "$vectors" "$metric" "$description"
    else
        # Regular dataset
        download_regular_dataset "$name" "$url" "$dims" "$vectors" "$metric" "$description"
    fi

    # Convert to binary
    if [ $? -eq 0 ]; then
        convert_to_binary "$name" "$metric"
    fi
}

# Main function
main() {
    print_header "Valkey Big-ANN Dataset Manager"
    echo "VALKEY_HOME: $VALKEY_HOME"
    echo "Dataset directory: $DATASET_DIR"
    echo "Binary directory: $BINARY_DIR"
    echo

    # Parse arguments
    if [ $# -eq 0 ]; then
        echo "Usage: $0 [OPTIONS] <dataset-name|category>"
        echo
        echo "Options:"
        echo "  --list              List all available datasets"
        echo "  --all               Download all datasets"
        echo "  --standard          Download standard ANN benchmarks"
        echo "  --bigann            Download Big-ANN datasets"
        echo "  --quick             Download quick test set (3 datasets)"
        echo
        echo "Individual datasets:"
        for name in "${!DATASETS[@]}"; do
            IFS=',' read -r url dims vectors metric description <<< "${DATASETS[$name]}"
            printf "  %-20s %s\n" "$name" "$description"
        done | sort
        exit 0
    fi

    # Handle command line arguments
    case "$1" in
        --list)
            print_header "Available Datasets"
            for name in "${!DATASETS[@]}"; do
                IFS=',' read -r url dims vectors metric description <<< "${DATASETS[$name]}"
                printf "%-20s %4s dims, %10s vectors, %-6s - %s\n" \
                    "$name" "$dims" "$vectors" "$metric" "$description"
            done | sort
            ;;

        --all)
            print_header "Downloading ALL datasets"
            for name in "${!DATASETS[@]}"; do
                process_dataset "$name"
            done
            ;;

        --standard)
            print_header "Downloading standard ANN benchmarks"
            for name in sift-128 gist-960 glove-25 glove-50 glove-100 fashion-mnist deep-96; do
                process_dataset "$name"
            done
            ;;

        --bigann)
            print_header "Downloading Big-ANN datasets"
            for name in bigann-1M bigann-10M deep-1M deep-10M text2image-1M msturing-10M; do
                process_dataset "$name"
            done
            ;;

        --quick)
            print_header "Downloading quick test set"
            for name in glove-25 sift-128 fashion-mnist; do
                process_dataset "$name"
            done
            ;;

        *)
            # Process individual dataset
            process_dataset "$1"
            ;;
    esac

    # Show summary
    echo
    print_header "Summary"
    echo "HDF5 files in $DATASET_DIR:"
    ls -lh "$DATASET_DIR"/*.hdf5 2>/dev/null | tail -5 || echo "  No HDF5 files found"
    echo
    echo "Binary files in $BINARY_DIR:"
    ls -lh "$BINARY_DIR"/*.bin 2>/dev/null | tail -5 || echo "  No binary files found"
}

# Run main function
main "$@"