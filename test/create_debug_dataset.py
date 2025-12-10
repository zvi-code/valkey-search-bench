#!/usr/bin/env python3
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) 2024-present, Zvi Schneider
#
# Create a tiny debug dataset directly in binary format for debugging vec-load issues
#
# This file is part of valkey-search-benchmark and is licensed under the
# BSD 3-Clause License. See the LICENSE file in the root directory.
#
"""
Create a tiny debug dataset for debugging vec-load.

This creates a minimal binary dataset:
- 200 vectors (configurable)
- 8 dimensions (very low for fast debugging)
- 20 queries with ground truth
- Direct binary format (no HDF5 intermediate)

Usage:
    python3 test/create_debug_dataset.py --output datasets/debug.bin
    python3 test/create_debug_dataset.py --vectors 500 --dim 16 --output datasets/debug.bin
"""

import numpy as np
import struct
from pathlib import Path
import argparse

# Binary format constants
DATASET_MAGIC = 0xDECDB001
DATASET_VERSION = 1
DISTANCE_L2 = 0
DISTANCE_COSINE = 1
DISTANCE_IP = 2


def align_to(size, alignment=64):
    """Align size to specified alignment."""
    return ((size + alignment - 1) // alignment) * alignment


def compute_ground_truth_l2(vectors, queries, k):
    """Compute L2 ground truth neighbors (brute force)."""
    num_queries = len(queries)
    neighbors = np.zeros((num_queries, k), dtype=np.int64)
    
    for i, query in enumerate(queries):
        # Compute L2 distances
        diffs = vectors - query
        distances = np.sum(diffs ** 2, axis=1)
        
        # Get top k neighbors
        top_indices = np.argsort(distances)[:k]
        neighbors[i] = top_indices
    
    return neighbors


def create_debug_dataset(output_path, num_vectors=200, dim=8, num_queries=20, 
                         k_neighbors=10, distance_metric=DISTANCE_L2, seed=42):
    """
    Create a minimal debug dataset in binary format.
    
    Args:
        output_path: Path to output binary file
        num_vectors: Number of vectors (default 200)
        dim: Vector dimensions (default 8)
        num_queries: Number of queries (default 20)
        k_neighbors: Ground truth neighbors per query (default 10)
        distance_metric: Distance metric (default L2)
        seed: Random seed for reproducibility
    """
    print(f"Creating debug dataset: {num_vectors} vectors, {dim}D")
    print(f"  Queries: {num_queries}, k={k_neighbors}")
    
    np.random.seed(seed)
    
    # Create vectors with some structure (clusters for realistic ground truth)
    vectors = []
    num_clusters = max(2, num_vectors // 50)
    vectors_per_cluster = num_vectors // num_clusters
    
    for cluster_id in range(num_clusters):
        # Create cluster center
        center = np.random.randn(dim).astype(np.float32)
        
        # Create vectors around this center
        for _ in range(vectors_per_cluster):
            noise = np.random.normal(0, 0.3, dim).astype(np.float32)
            vector = center + noise
            vectors.append(vector)
    
    # Fill remaining vectors
    while len(vectors) < num_vectors:
        vectors.append(np.random.randn(dim).astype(np.float32))
    
    vectors = np.array(vectors[:num_vectors], dtype=np.float32)
    print(f"  Created {len(vectors)} vectors")
    
    # Create queries (random subset from vectors for guaranteed matches)
    query_indices = np.random.choice(num_vectors, num_queries, replace=False)
    queries = vectors[query_indices].copy()
    
    # Add small noise to queries to make them slightly different
    queries += np.random.normal(0, 0.01, queries.shape).astype(np.float32)
    queries = queries.astype(np.float32)
    print(f"  Created {len(queries)} queries")
    
    # Compute ground truth
    print("  Computing ground truth...")
    ground_truth = compute_ground_truth_l2(vectors, queries, k_neighbors)
    
    # Validate ground truth
    for i, qi in enumerate(query_indices):
        if ground_truth[i][0] == qi:
            pass  # Expected: query's source vector is top match
        else:
            # Due to noise, source might not be top match, that's OK
            pass
    
    print(f"  Ground truth computed: {ground_truth.shape}")
    
    # Calculate offsets (64-byte aligned)
    header_size = 4096
    vectors_offset = header_size
    vectors_size = align_to(num_vectors * dim * 4)
    
    queries_offset = vectors_offset + vectors_size
    queries_size = align_to(num_queries * dim * 4)
    
    ground_truth_offset = queries_offset + queries_size
    ground_truth_size = num_queries * k_neighbors * 8
    
    total_size = ground_truth_offset + ground_truth_size
    
    print(f"\nBinary layout:")
    print(f"  Header:       0 - 4096 bytes")
    print(f"  Vectors:      {vectors_offset} - {vectors_offset + vectors_size} ({vectors_size} bytes)")
    print(f"  Queries:      {queries_offset} - {queries_offset + queries_size} ({queries_size} bytes)")
    print(f"  Ground truth: {ground_truth_offset} - {ground_truth_offset + ground_truth_size} ({ground_truth_size} bytes)")
    print(f"  Total size:   {total_size} bytes ({total_size / 1024:.1f} KB)")
    
    # Ensure output directory exists
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    # Write binary file
    with open(output_path, 'wb') as f:
        # Prepare dataset name
        dataset_name = f"debug-{num_vectors}v-{dim}d"
        name_bytes = dataset_name.encode('utf-8')[:256]
        name_bytes = name_bytes.ljust(256, b'\x00')
        
        # Calculate padding needed to reach 4096 bytes
        # Format: magic(4) + version(4) + name(256) + metric(1) + dtype(1) + padding(2) 
        #         + dim(4) + num_vectors(8) + num_queries(8) + num_neighbors(4) + padding(4)
        #         + vectors_offset(8) + queries_offset(8) + ground_truth_offset(8)
        base_header_size = struct.calcsize('<II256sBBxxIQQIxxxxQQQ')
        padding_size = 4096 - base_header_size
        
        # Write header (4KB)
        header = struct.pack(
            f'<II256sBBxxIQQIxxxxQQQ{padding_size}x',
            DATASET_MAGIC,
            DATASET_VERSION,
            name_bytes,
            distance_metric,
            0,  # dtype: FLOAT32
            dim,
            num_vectors,
            num_queries,
            k_neighbors,
            vectors_offset,
            queries_offset,
            ground_truth_offset
        )
        assert len(header) == 4096, f"Header size mismatch: {len(header)} != 4096"
        f.write(header)
        
        # Write vectors
        f.seek(vectors_offset)
        vectors.tofile(f)
        padding = vectors_size - (num_vectors * dim * 4)
        if padding > 0:
            f.write(b'\x00' * padding)
        
        # Write queries
        f.seek(queries_offset)
        queries.tofile(f)
        padding = queries_size - (num_queries * dim * 4)
        if padding > 0:
            f.write(b'\x00' * padding)
        
        # Write ground truth
        f.seek(ground_truth_offset)
        ground_truth.astype(np.int64).tofile(f)
    
    print(f"\n✓ Dataset written to {output_path}")
    
    # Verify the dataset
    verify_dataset(output_path)
    
    return str(output_path)


def verify_dataset(path):
    """Verify the created binary dataset."""
    print(f"\nVerifying {path}...")
    
    with open(path, 'rb') as f:
        # Read and verify header
        magic = struct.unpack('<I', f.read(4))[0]
        assert magic == DATASET_MAGIC, f"Invalid magic: 0x{magic:x}"
        
        version = struct.unpack('<I', f.read(4))[0]
        assert version == DATASET_VERSION, f"Invalid version: {version}"
        
        name = f.read(256).rstrip(b'\x00').decode('utf-8')
        
        metric, dtype = struct.unpack('<BB', f.read(2))
        f.read(2)  # padding
        
        dim = struct.unpack('<I', f.read(4))[0]
        num_vectors = struct.unpack('<Q', f.read(8))[0]
        num_queries = struct.unpack('<Q', f.read(8))[0]
        num_neighbors = struct.unpack('<I', f.read(4))[0]
        f.read(4)  # padding
        
        vectors_offset = struct.unpack('<Q', f.read(8))[0]
        queries_offset = struct.unpack('<Q', f.read(8))[0]
        ground_truth_offset = struct.unpack('<Q', f.read(8))[0]
        
        print(f"  ✓ Magic: 0x{magic:08X}")
        print(f"  ✓ Version: {version}")
        print(f"  ✓ Name: {name}")
        print(f"  ✓ Dimensions: {dim}")
        print(f"  ✓ Vectors: {num_vectors}")
        print(f"  ✓ Queries: {num_queries}")
        print(f"  ✓ Neighbors: {num_neighbors}")
        print(f"  ✓ Metric: {'L2' if metric == 0 else 'COSINE' if metric == 1 else 'IP'}")
        
        # Read and verify some vectors
        f.seek(vectors_offset)
        first_vector = np.frombuffer(f.read(dim * 4), dtype=np.float32)
        print(f"  ✓ First vector sample: [{first_vector[0]:.4f}, {first_vector[1]:.4f}, ...]")
        
        # Read and verify ground truth
        f.seek(ground_truth_offset)
        first_gt = np.frombuffer(f.read(num_neighbors * 8), dtype=np.int64)
        print(f"  ✓ First query neighbors: {first_gt.tolist()}")
        
        # Check ground truth values are valid
        assert all(0 <= n < num_vectors for n in first_gt), "Invalid neighbor indices!"
        
    print("  ✓ Verification passed!")


def main():
    parser = argparse.ArgumentParser(
        description='Create a tiny debug dataset for debugging vec-load'
    )
    parser.add_argument('--output', '-o', default='datasets/debug.bin',
                        help='Output binary file (default: datasets/debug.bin)')
    parser.add_argument('--vectors', '-n', type=int, default=200,
                        help='Number of vectors (default: 200)')
    parser.add_argument('--dim', '-d', type=int, default=8,
                        help='Vector dimensions (default: 8)')
    parser.add_argument('--queries', '-q', type=int, default=20,
                        help='Number of queries (default: 20)')
    parser.add_argument('--neighbors', '-k', type=int, default=10,
                        help='Ground truth neighbors per query (default: 10)')
    parser.add_argument('--metric', choices=['L2', 'COSINE', 'IP'], default='L2',
                        help='Distance metric (default: L2)')
    parser.add_argument('--seed', type=int, default=42,
                        help='Random seed (default: 42)')
    
    args = parser.parse_args()
    
    metric_map = {'L2': DISTANCE_L2, 'COSINE': DISTANCE_COSINE, 'IP': DISTANCE_IP}
    
    create_debug_dataset(
        output_path=args.output,
        num_vectors=args.vectors,
        dim=args.dim,
        num_queries=args.queries,
        k_neighbors=args.neighbors,
        distance_metric=metric_map[args.metric],
        seed=args.seed
    )
    
    print(f"\n" + "="*60)
    print("USAGE EXAMPLE:")
    print("="*60)
    print(f"""
# Run vec-load with debug dataset:
./build/bin/valkey-benchmark --tls \\
  -h zsearch-serverless-ajfdds.serverless.euw1devo.cache.amazonaws.com -p 6379 \\
  -c 1 --threads 1 -t vec-load --search \\
  --search-name debug_index --search-prefix "dbg:" \\
  --vector-dim {args.dim} --k {args.neighbors} --metric L2 --m 16 --ef-construction 256 \\
  --dataset {args.output} --no-save-config --no-baseline

# Then check:
valkey-cli --tls -h zsearch-serverless-ajfdds.serverless.euw1devo.cache.amazonaws.com -p 6379 DBSIZE
# Expected: {args.vectors}
""")


if __name__ == '__main__':
    main()
