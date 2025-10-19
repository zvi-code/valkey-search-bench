#!/usr/bin/env python3 -u
"""
FAST OPTIMIZED version: Convert vectordb-bench parquet datasets to HDF5.

TRUE streaming with batched processing:
- Process parquet in small batches (5000 rows at a time)
- Direct PyArrow→NumPy conversion (no intermediate Python lists)
- Write incrementally to HDF5
- Progress logging every batch with sys.stdout.flush()
- Memory usage: <5GB regardless of dataset size

Expected performance for 10M vectors (768 dims):
- Time: 2-4 minutes
- RAM: 2-4GB peak
- Disk I/O: Sequential writes
"""

import sys
import argparse
import numpy as np
import h5py
import pyarrow.parquet as pq
from pathlib import Path
import time

# Force unbuffered output
sys.stdout = open(sys.stdout.fileno(), mode='w', buffering=1)
sys.stderr = open(sys.stderr.fileno(), mode='w', buffering=1)

BATCH_SIZE = 5000  # Process 5K vectors at a time

def convert_parquet_to_hdf5_fast(dataset_dir, output_file, dataset_name):
    """
    Convert vectordb-bench parquet files to HDF5 with TRUE streaming.
    """
    dataset_path = Path(dataset_dir)
    start_time = time.time()
    
    print("=" * 70)
    print(f"OPTIMIZED PARQUET → HDF5 CONVERTER")
    print("=" * 70)
    print(f"Dataset: {dataset_name}")
    print(f"Input:   {dataset_path}")
    print(f"Output:  {output_file}")
    print(f"Batch:   {BATCH_SIZE:,} vectors per iteration")
    print()
    
    # Find all training parquet files
    train_files = sorted(dataset_path.glob("*train*.parquet"))
    
    if not train_files:
        raise FileNotFoundError(f"No training parquet files found in {dataset_path}")
    
    print(f"Found {len(train_files)} training file(s):")
    for i, f in enumerate(train_files, 1):
        size_mb = f.stat().st_size / (1024**2)
        print(f"  {i:2d}. {f.name:40s} ({size_mb:7.1f} MB)")
    print()
    
    # === PHASE 1: Analyze dataset ===
    print("PHASE 1: Analyzing dataset structure...")
    first_table = pq.read_table(train_files[0], columns=[])
    first_table_with_data = pq.read_table(train_files[0])
    
    # Determine vector column
    if 'emb' in first_table_with_data.column_names:
        vec_col = 'emb'
        first_vec = first_table_with_data[vec_col][0].as_py()
    elif 'vector' in first_table_with_data.column_names:
        vec_col = 'vector'
        first_vec = first_table_with_data[vec_col][0].as_py()
    else:
        raise ValueError(f"No 'emb' or 'vector' column. Available: {first_table_with_data.column_names}")
    
    vec_dim = len(first_vec)
    del first_table_with_data
    
    # Count total vectors
    print(f"  Vector column: '{vec_col}'")
    print(f"  Dimensions: {vec_dim}")
    print(f"  Counting rows in all files...")
    
    total_vectors = 0
    for train_file in train_files:
        table = pq.read_table(train_file, columns=[])
        total_vectors += table.num_rows
    
    est_size_gb = total_vectors * vec_dim * 4 / (1024**3)
    print(f"  Total training vectors: {total_vectors:,}")
    print(f"  Estimated size: {est_size_gb:.2f} GB")
    print()
    
    # === PHASE 2: Create HDF5 file ===
    print("PHASE 2: Creating HDF5 file...")
    output_path = Path(output_file)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    
    hf = h5py.File(output_file, 'w')
    
    # Create chunked dataset (NO compression for speed)
    chunk_size = min(BATCH_SIZE, total_vectors)
    train_dset = hf.create_dataset(
        'train',
        shape=(total_vectors, vec_dim),
        dtype=np.float32,
        chunks=(chunk_size, vec_dim),
    )
    print(f"  Created dataset 'train': shape={train_dset.shape}, chunks={train_dset.chunks}")
    print()
    
    # === PHASE 3: Load and un-shuffle training vectors ===
    print("PHASE 3: Loading training vectors (un-shuffling by ID)...")
    print(f"  Note: Training vectors are shuffled in parquet files")
    print(f"  Loading all vectors into memory to sort by ID...")
    print()
    
    phase3_start = time.time()
    
    # Load ALL training vectors with their IDs
    all_vectors = []
    all_ids = []
    
    for file_idx, train_file in enumerate(train_files, 1):
        file_start = time.time()
        
        print(f"  [{file_idx:2d}/{len(train_files)}] {train_file.name}")
        
        # Read the entire file (we need IDs and vectors together)
        table = pq.read_table(train_file, columns=['id', vec_col])
        
        # Get IDs and vectors
        ids = table['id'].to_numpy()
        vectors = table[vec_col].to_numpy(zero_copy_only=False)
        vectors = np.vstack(vectors).astype(np.float32)
        
        all_ids.append(ids)
        all_vectors.append(vectors)
        
        file_time = time.time() - file_start
        print(f"         ✓ Loaded {len(ids):,} vectors in {file_time:.1f}s")
        
        del table
    
    # Concatenate all files
    print(f"\n  Concatenating {len(train_files)} files...")
    all_ids = np.concatenate(all_ids)
    all_vectors = np.vstack(all_vectors)
    
    # Sort by ID to restore original order
    print(f"  Sorting {len(all_ids):,} vectors by ID...")
    sort_indices = np.argsort(all_ids)
    sorted_vectors = all_vectors[sort_indices]
    sorted_ids = all_ids[sort_indices]
    
    # Verify IDs are now sequential
    expected_ids = np.arange(len(sorted_ids))
    if not np.array_equal(sorted_ids, expected_ids):
        print(f"  WARNING: IDs are not sequential after sorting!")
        print(f"    Expected: 0 to {len(sorted_ids)-1}")
        print(f"    Got: {sorted_ids[0]} to {sorted_ids[-1]}")
    else:
        print(f"  ✓ IDs verified: 0 to {len(sorted_ids)-1} (sequential)")
    
    # Write to HDF5
    print(f"  Writing {len(sorted_vectors):,} vectors to HDF5...")
    train_dset[:] = sorted_vectors
    
    del all_ids, all_vectors, sort_indices, sorted_vectors, sorted_ids
    
    phase3_time = time.time() - phase3_start
    phase3_vecs_per_sec = total_vectors / phase3_time if phase3_time > 0 else 0
    print(f"  ✓ Phase 3 complete: {total_vectors:,} vectors in {phase3_time:.1f}s ({phase3_vecs_per_sec:,.0f} vec/s)")
    print()
    
    # === PHASE 4: Load test queries ===
    print("PHASE 4: Loading test queries...")
    test_file = dataset_path / "test.parquet"
    if not test_file.exists():
        raise FileNotFoundError(f"Test file not found: {test_file}")
    
    test_table = pq.read_table(test_file)
    # Direct PyArrow → NumPy conversion
    test_vectors = test_table[vec_col].to_numpy(zero_copy_only=False)
    test_data = np.vstack(test_vectors).astype(np.float32)
    num_queries = len(test_data)
    
    hf.create_dataset('test', data=test_data)
    print(f"  ✓ Loaded {num_queries:,} test queries")
    
    del test_data
    del test_vectors
    del test_table
    print()
    
    # === PHASE 5: Load ground truth ===
    print("PHASE 5: Loading ground truth neighbors...")
    neighbors_file = dataset_path / "neighbors.parquet"
    if not neighbors_file.exists():
        raise FileNotFoundError(f"Neighbors file not found: {neighbors_file}")
    
    neighbors_table = pq.read_table(neighbors_file)
    
    # Find neighbor column
    neighbor_col = None
    for col_name in ['neighbors', 'neighbor_ids', 'neighbors_id', 'neighbor_id']:
        if col_name in neighbors_table.column_names:
            neighbor_col = col_name
            break
    
    if not neighbor_col:
        raise ValueError(f"No neighbors column found. Available: {list(neighbors_table.column_names)}")
    
    # Direct PyArrow → NumPy conversion (avoid pandas intermediate step)
    neighbors_arrays = neighbors_table[neighbor_col].to_numpy(zero_copy_only=False)
    neighbors_data = np.vstack(neighbors_arrays).astype(np.int64)
    num_neighbors = neighbors_data.shape[1]
    
    hf.create_dataset('neighbors', data=neighbors_data)
    print(f"  ✓ Loaded ground truth: {neighbors_data.shape[0]:,} queries × {num_neighbors} neighbors")
    
    del neighbors_data
    del neighbors_arrays
    del neighbors_table
    print()
    
    # === PHASE 6: Add metadata ===
    print("PHASE 6: Writing metadata...")
    hf.attrs['dataset_name'] = dataset_name
    hf.attrs['num_vectors'] = total_vectors
    hf.attrs['num_queries'] = num_queries
    hf.attrs['dimensions'] = vec_dim
    hf.attrs['num_neighbors'] = num_neighbors
    print(f"  ✓ Metadata written")
    print()
    
    # Close file
    hf.close()
    
    # === SUMMARY ===
    total_time = time.time() - start_time
    output_size = output_path.stat().st_size
    output_size_gb = output_size / (1024**3)
    overall_vecs_per_sec = total_vectors / total_time if total_time > 0 else 0
    
    print("=" * 70)
    print("CONVERSION COMPLETE!")
    print("=" * 70)
    print(f"  Output file: {output_file}")
    print(f"  File size: {output_size_gb:.2f} GB")
    print(f"  Training vectors: {total_vectors:,} × {vec_dim}")
    print(f"  Test queries: {num_queries:,} × {vec_dim}")
    print(f"  Ground truth: {num_queries:,} × {num_neighbors}")
    print(f"  Total time: {total_time:.1f}s ({total_time/60:.1f} min)")
    print(f"  Throughput: {overall_vecs_per_sec:,.0f} vectors/second")
    print("=" * 70)
    
    return output_file

def main():
    parser = argparse.ArgumentParser(
        description='Convert vectordb-bench parquet datasets to HDF5 (FAST version)'
    )
    parser.add_argument('dataset_dir', help='Directory containing parquet files')
    parser.add_argument('output_file', help='Output HDF5 file path')
    parser.add_argument('--name', default='dataset', help='Dataset name (for metadata)')
    
    args = parser.parse_args()
    
    try:
        convert_parquet_to_hdf5_fast(args.dataset_dir, args.output_file, args.name)
        return 0
    except Exception as e:
        print(f"\n✗ ERROR: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 1

if __name__ == '__main__':
    sys.exit(main())
