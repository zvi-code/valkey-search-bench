#!/usr/bin/env python3
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) 2024-present, Zvi Schneider
#
# Create a small synthetic dataset with metadata for testing
#
# This file is part of valkey-search-benchmark and is licensed under the
# BSD 3-Clause License. See the LICENSE file in the root directory.
#
"""
Create a small synthetic dataset with metadata for testing.

Creates a tiny dataset (100 vectors, 10 queries) with simple metadata:
- 5 tags in vocabulary: "red", "blue", "green", "large", "small"
- Each vector gets 1-3 random tags
- Each query gets 1-2 random predicates

Usage:
  python create_metadata_test_dataset.py --output test_metadata.bin
"""

import numpy as np
import struct
import argparse
from pathlib import Path
import sys

# Add utils to path
sys.path.insert(0, str(Path(__file__).parent.parent.parent / 'utils' / 'datasets'))

DATASET_MAGIC = 0xDECDB001
DATASET_VERSION_METADATA = 2
DISTANCE_L2 = 0

def create_synthetic_csr(nrow, ncol, density=0.1):
    """Create random sparse matrix in CSR format"""
    nnz_per_row = max(1, int(ncol * density))
    
    indptr = [0]
    indices = []
    data = []
    
    for i in range(nrow):
        # Random tags for this row
        row_tags = np.random.choice(ncol, size=min(nnz_per_row, ncol), replace=False)
        row_tags = np.sort(row_tags)
        
        for tag in row_tags:
            indices.append(tag)
            data.append(1.0)  # Binary (tag present or not)
        
        indptr.append(len(indices))
    
    return {
        'nrow': nrow,
        'ncol': ncol,
        'nnz': len(indices),
        'indptr': np.array(indptr, dtype=np.uint32),
        'indices': np.array(indices, dtype=np.uint32),
        'data': np.array(data, dtype=np.float32)
    }

def write_csr_matrix(f, csr_dict, offset):
    """Write CSR matrix to binary file"""
    f.seek(offset)
    f.write(struct.pack('<III', csr_dict['nrow'], csr_dict['ncol'], csr_dict['nnz']))
    csr_dict['indptr'].tofile(f)
    csr_dict['indices'].tofile(f)
    csr_dict['data'].tofile(f)
    
    header_size = 12
    indptr_size = len(csr_dict['indptr']) * 4
    indices_size = len(csr_dict['indices']) * 4
    data_size = len(csr_dict['data']) * 4
    return header_size + indptr_size + indices_size + data_size

def write_vocabulary(f, words, offset):
    """Write vocabulary as null-terminated strings"""
    f.seek(offset)
    f.write(struct.pack('<I', len(words)))
    
    bytes_written = 4
    for word in words:
        word_bytes = word.encode('utf-8') + b'\x00'
        f.write(word_bytes)
        bytes_written += len(word_bytes)
    
    return bytes_written

def align_to(size, alignment=64):
    return ((size + alignment - 1) // alignment) * alignment

def main():
    parser = argparse.ArgumentParser(description='Create synthetic metadata test dataset')
    parser.add_argument('--output', default='test_metadata.bin', help='Output file')
    parser.add_argument('--num-vectors', type=int, default=100, help='Number of vectors')
    parser.add_argument('--num-queries', type=int, default=10, help='Number of queries')
    parser.add_argument('--dim', type=int, default=128, help='Vector dimension')
    parser.add_argument('--k', type=int, default=10, help='Ground truth neighbors')
    
    args = parser.parse_args()
    
    num_vectors = args.num_vectors
    num_queries = args.num_queries
    dim = args.dim
    k = args.k
    
    print(f"Creating synthetic dataset...")
    print(f"  Vectors: {num_vectors}, Queries: {num_queries}, Dim: {dim}, K: {k}")
    
    # Create random vectors and queries
    np.random.seed(42)
    vectors = np.random.randn(num_vectors, dim).astype(np.float32)
    queries = np.random.randn(num_queries, dim).astype(np.float32)
    
    # Compute ground truth
    print(f"Computing ground truth...")
    ground_truth = np.zeros((num_queries, k), dtype=np.int64)
    for i in range(num_queries):
        distances = np.sum((vectors - queries[i]) ** 2, axis=1)
        neighbors = np.argsort(distances)[:k]
        ground_truth[i] = neighbors
    
    # Create metadata
    vocabulary = ["red", "blue", "green", "large", "small"]
    vocab_size = len(vocabulary)
    
    print(f"Creating metadata...")
    print(f"  Vocabulary: {vocabulary}")
    
    # Vector metadata: each vector gets 1-3 tags
    vector_metadata = create_synthetic_csr(num_vectors, vocab_size, density=0.4)
    print(f"  Vector metadata: {vector_metadata['nnz']} tag assignments")
    
    # Query metadata: each query gets 1-2 predicates
    query_metadata = create_synthetic_csr(num_queries, vocab_size, density=0.3)
    print(f"  Query metadata: {query_metadata['nnz']} predicate assignments")
    
    # Calculate offsets
    header_size = 4096
    vectors_offset = header_size
    vectors_size = align_to(num_vectors * dim * 4)
    
    queries_offset = vectors_offset + vectors_size
    queries_size = align_to(num_queries * dim * 4)
    
    ground_truth_offset = queries_offset + queries_size
    ground_truth_size = num_queries * k * 8
    
    vector_metadata_offset = ground_truth_offset + ground_truth_size
    vm_size = 12 + (vector_metadata['nrow'] + 1) * 4 + vector_metadata['nnz'] * 8
    
    query_metadata_offset = vector_metadata_offset + vm_size
    qm_size = 12 + (query_metadata['nrow'] + 1) * 4 + query_metadata['nnz'] * 8
    
    vocab_offset = query_metadata_offset + qm_size
    vocab_size_bytes = 4 + sum(len(w.encode('utf-8')) + 1 for w in vocabulary)
    
    total_size = vocab_offset + vocab_size_bytes
    
    print(f"Writing binary file ({total_size / 1024:.1f} KB)...")
    
    # Write file
    with open(args.output, 'wb') as f:
        # Header
        name_bytes = b'Synthetic-Metadata-Test'.ljust(256, b'\x00')
        base_header_size = struct.calcsize('<II256sBBxxIQQIBxxxIQQQQQ')
        padding_size = 4096 - base_header_size
        
        header = struct.pack(
            f'<II256sBBxxIQQIBxxxIQQQQQ{padding_size}x',
            DATASET_MAGIC,
            DATASET_VERSION_METADATA,
            name_bytes,
            DISTANCE_L2,
            0,  # dtype FLOAT32
            dim,
            num_vectors,
            num_queries,
            k,
            1,  # has_metadata
            vocab_size,
            vectors_offset,
            queries_offset,
            ground_truth_offset,
            vector_metadata_offset,
            query_metadata_offset,
            vocab_offset
        )
        f.write(header)
        
        # Vectors
        f.seek(vectors_offset)
        vectors.tofile(f)
        padding = vectors_size - (num_vectors * dim * 4)
        if padding > 0:
            f.write(b'\x00' * padding)
        
        # Queries
        f.seek(queries_offset)
        queries.tofile(f)
        padding = queries_size - (num_queries * dim * 4)
        if padding > 0:
            f.write(b'\x00' * padding)
        
        # Ground truth
        f.seek(ground_truth_offset)
        ground_truth.tofile(f)
        
        # Metadata
        write_csr_matrix(f, vector_metadata, vector_metadata_offset)
        write_csr_matrix(f, query_metadata, query_metadata_offset)
        write_vocabulary(f, vocabulary, vocab_offset)
    
    print(f"✓ Created {args.output}")
    print(f"  Total size: {total_size / 1024:.1f} KB")
    print(f"\nTest with:")
    print(f"  valkey-benchmark --dataset {args.output} --filtered")

if __name__ == '__main__':
    main()
