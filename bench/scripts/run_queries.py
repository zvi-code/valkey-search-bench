#!/usr/bin/env python3
"""
SPDX-License-Identifier: BSD-3-Clause

Copyright (c) 2024-present, Zvi Schneider

Auto-optimized Query Benchmark

Leverages valkey-benchmark's built-in optimizer to find optimal configuration
for maximum query throughput with constraints. Only requires dataset and host.

The script:
1. Detects dataset properties (dimensions, size) 
2. Sets up optimization objective and constraints
3. Uses valkey-benchmark --optimize for closed-loop search
4. Reports results and the exact command used

Example usage:
    # Simple - just dataset name and host
    ./run_queries.py --host localhost --dataset openai-large-5m
    
    # With custom constraints
    ./run_queries.py --host localhost --dataset sift-128 --target-recall 0.98 --max-p99-latency 50
"""

import sys
import argparse
from pathlib import Path
import struct
import os

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))

from wrappers import (
    ValKeyBenchmarkWrapper,
    BenchmarkConfig,
    BenchmarkError,
    BinaryNotFoundError,
)


def find_dataset_path(dataset_name: str) -> str:
    """Find dataset file path from dataset name.
    
    Looks for dataset in standard locations:
    1. datasets/ directory (relative to script)
    2. BENCHMARK_HOME/datasets/ if BENCHMARK_HOME is set
    3. Current directory
    
    Args:
        dataset_name: Dataset name (e.g., "openai-large-5m", "sift-128")
    
    Returns:
        Full path to dataset file
    
    Raises:
        BenchmarkError: If dataset file not found
    """
    # Try with and without .bin extension
    candidates = [dataset_name]
    if not dataset_name.endswith('.bin'):
        candidates.append(f"{dataset_name}.bin")
    
    # Standard search locations
    script_dir = Path(__file__).parent.parent.parent  # Go up to project root
    search_paths = [
        script_dir / "datasets",
    ]
    
    # Add BENCHMARK_HOME if set
    if benchmark_home := os.getenv("BENCHMARK_HOME"):
        search_paths.insert(0, Path(benchmark_home) / "datasets")
    
    # Also try current directory
    search_paths.append(Path.cwd())
    
    # Search for dataset file
    for search_dir in search_paths:
        for candidate in candidates:
            dataset_path = search_dir / candidate
            if dataset_path.exists():
                return str(dataset_path)
    
    # If not found, provide helpful error
    raise BenchmarkError(
        f"Dataset '{dataset_name}' not found. Searched in:\n" +
        "\n".join(f"  - {p}" for p in search_paths) +
        f"\n\nTried filenames: {', '.join(candidates)}"
    )


def detect_dataset_info(dataset_path: str) -> tuple:
    """Detect dataset dimensions and size from binary file.
    
    Binary format (valkey-search-benchmark custom format):
        Header (4KB):
            uint32_t magic (0xDECDB001)
            uint32_t version
            char dataset_name[256]
            uint8_t distance_metric
            uint8_t dtype
            uint8_t has_metadata
            uint8_t padding
            uint32_t dim
            uint64_t num_vectors
            uint64_t num_queries
            ...
    
    Returns:
        (num_vectors, dimensions)
    """
    try:
        with open(dataset_path, 'rb') as f:
            # Read magic number
            magic = struct.unpack('I', f.read(4))[0]
            
            if magic != 0xDECDB001:
                raise ValueError(f"Invalid magic number: 0x{magic:08X} (expected 0xDECDB001)")
            
            # Read version
            version = struct.unpack('I', f.read(4))[0]
            
            # Skip dataset_name[256]
            f.seek(256, 1)
            
            # Skip distance_metric, dtype, has_metadata, padding (4 bytes total)
            f.seek(4, 1)
            
            # Read dim (uint32_t)
            dimensions = struct.unpack('I', f.read(4))[0]
            
            # Read num_vectors (uint64_t)
            num_vectors = struct.unpack('Q', f.read(8))[0]
            
        return num_vectors, dimensions
        
    except Exception as e:
        # Fallback - try to infer from filename
        filename = Path(dataset_path).stem
        
        # Common patterns: sift-128, glove-50, openai-large-5m, etc.
        # Try to extract dimension from filename
        if '-' in filename:
            parts = filename.split('-')
            for part in parts:
                if part.isdigit():
                    dimensions = int(part)
                    return None, dimensions
        
        raise BenchmarkError(f"Could not detect dataset info: {e}")


def estimate_initial_config(num_vectors: int, dimensions: int):
    """Estimate reasonable starting configuration for optimizer.
    
    The optimizer will adjust these values automatically, but providing
    reasonable starting points helps it converge faster.
    
    Returns:
        (ef_search, num_clients, num_threads)
    """
    import os
    
    # Detect CPU cores
    num_cores = os.cpu_count() or 4
    num_threads = min(num_cores, 8)  # Cap at 8 for most workloads
    
    # ef_search starting point based on dimensions
    if dimensions <= 128:
        ef_search = 100
    elif dimensions <= 512:
        ef_search = 150
    else:
        ef_search = 200
    
    # num_clients: Start conservative, optimizer will increase if needed
    if num_vectors and num_vectors < 100000:
        num_clients = 20
    elif num_vectors and num_vectors < 1000000:
        num_clients = 50
    else:
        num_clients = 100
    
    return ef_search, num_clients, num_threads


def main():
    parser = argparse.ArgumentParser(
        description="Auto-optimized query benchmark - only requires dataset and host",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Simplest usage - find max QPS with 95%% recall
  %(prog)s --host localhost --dataset openai-large-5m
  
  # Custom recall requirement
  %(prog)s --host localhost --dataset sift-128 --target-recall 0.98

The script uses valkey-benchmark's native optimizer to:
  - Automatically detect dataset properties
  - Find maximum QPS while maintaining target recall
  - Optimize ef_search, num_clients, and num_threads
  - Report performance metrics and exact command for reproducibility
        """
    )
    
    # Required arguments
    parser.add_argument(
        "--host",
        required=True,
        help="Redis/Valkey server hostname or IP"
    )
    parser.add_argument(
        "--dataset",
        required=True,
        help="Dataset name (e.g., 'openai-large-5m', 'sift-128'). "
             "Will be used as index name and searched in datasets/ directory"
    )
    
    # Optional tuning
    parser.add_argument(
        "--target-recall",
        type=float,
        default=0.95,
        help="Minimum target recall threshold (default: 0.95)"
    )
    parser.add_argument(
        "--num-requests",
        type=int,
        default=10000,
        help="Number of requests per optimization iteration (default: 10000)"
    )
    parser.add_argument(
        "--max-iterations",
        type=int,
        default=20,
        help="Maximum optimizer iterations (default: 20)"
    )
    
    # Output
    parser.add_argument(
        "--output",
        help="Save optimization results to CSV file"
    )
    parser.add_argument(
        "--show-output",
        action="store_true",
        default=True,
        help="Display benchmark output in real-time (default: True)"
    )
    parser.add_argument(
        "--no-show-output",
        dest="show_output",
        action="store_false",
        help="Suppress benchmark output (only show final results)"
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="Enable verbose output"
    )
    
    args = parser.parse_args()
    
    # Validate
    if args.target_recall < 0.0 or args.target_recall > 1.0:
        parser.error("--target-recall must be between 0.0 and 1.0")
    
    # Find dataset file
    dataset_name = args.dataset
    try:
        dataset_path = find_dataset_path(dataset_name)
    except BenchmarkError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    
    # Initialize wrapper
    try:
        wrapper = ValKeyBenchmarkWrapper(
            verbose=args.verbose,
            display_output=args.show_output
        )
    except BinaryNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    
    print(f"\n{'='*70}")
    print(f"Auto-Optimized Query Benchmark")
    print(f"{'='*70}")
    print(f"Host:        {args.host}")
    print(f"Dataset:     {dataset_name}")
    print(f"File Path:   {dataset_path}")
    print(f"{'='*70}\n")
    
    # Step 1: Detect dataset properties and generate index name + prefix
    print("📊 Detecting dataset properties...")
    try:
        num_vectors, dimensions = detect_dataset_info(dataset_path)
        if num_vectors:
            print(f"   Vectors:    {num_vectors:,}")
        print(f"   Dimensions: {dimensions}")
        
        # Generate index name in the format: dataset-{numVectors}-{dimensions}-{k}
        # Example: openai-large-5m-5M-1536-100
        if num_vectors:
            if num_vectors >= 1_000_000:
                vec_str = f"{num_vectors // 1_000_000}M"
            elif num_vectors >= 1_000:
                vec_str = f"{num_vectors // 1_000}K"
            else:
                vec_str = str(num_vectors)
            index_name = f"{dataset_name}-{vec_str}-{dimensions}-100"
        else:
            index_name = f"{dataset_name}-{dimensions}-100"
        
        # Generate search prefix based on dataset name
        # Format: zvec_{shortname}:
        # Examples: zvec_openai5m:, zvec_cohere100k:, zvec_sift:
        short_name = dataset_name.replace('-', '').replace('_', '')
        search_prefix = f"zvec_{short_name}:"
        
        print(f"   Index name:    {index_name}")
        print(f"   Search prefix: {search_prefix}")
    except BenchmarkError as e:
        print(f"   Warning: {e}")
        print(f"   Continuing with default configuration...")
        dimensions = 1536
        num_vectors = None
        index_name = f"{dataset_name}-1536-100"  # Fallback
        short_name = dataset_name.replace('-', '').replace('_', '')
        search_prefix = f"zvec_{short_name}:"
    
    # Step 2: Estimate initial configuration for optimizer
    print(f"\n🔧 Setting up optimizer configuration...")
    ef_search, num_clients, num_threads = estimate_initial_config(num_vectors, dimensions)
    
    print(f"   Initial ef_search: {ef_search}")
    print(f"   Initial clients:   {num_clients}")
    print(f"   Threads:           {num_threads}")
    
    print(f"\n🎯 Optimization goal:")
    print(f"   Maximize QPS with recall >= {args.target_recall:.0%}")
    print(f"   Max iterations: {args.max_iterations}")
    
    # Step 3: Run optimizer using native valkey-benchmark --optimize
    print(f"\n🔍 Running optimizer (closed-loop search)...\n")
    
    # Build optimization command using native optimizer
    # Put optimizer flags in prefix_args (before -t), other params use normal config
    config = BenchmarkConfig(
        host=args.host,
        dataset=dataset_path,
        num_clients=num_clients,
        num_threads=num_threads,
        num_requests=args.num_requests,
        ef_search=ef_search,
        operation="vec-query",
        prefix_args=[
            "--search",  # Boolean flag to enable search workload
            "--search-name", index_name,  # Index name with format: dataset-{M/K}-{dim}-{k}
            "--search-prefix", search_prefix,  # Prefix for keys: zvec_{name}:
            "--vector-dim", str(dimensions),  # Vector dimensions
            "--optimize",
            "--optimize-objective", "maximize:qps",
            "--optimize-constraint", f"recall_avg:gt:{args.target_recall}",
            "--optimize-max-iterations", str(args.max_iterations),
            "--optimize-min-requests", str(args.num_requests),
        ]
    )
    
    # Add CSV output if requested
    if args.output:
        config.extra_args.extend(["--optimize-csv", args.output])
    
    # Print the full command that will be executed
    full_cmd = [wrapper.binary] + config.prefix_args + ["-t", config.operation] + config.to_args()
    print(f"Command: {' '.join(full_cmd)}\n")
    
    # Run with stage monitoring
    with wrapper.stage("optimize"):
        try:
            result = wrapper.run(config)
            best_result = result
        except BenchmarkError as e:
            print(f"\n❌ Optimization failed: {e}\n", file=sys.stderr)
            best_result = None
    
    # Step 4: Report results
    print(f"\n{'='*70}")
    if best_result:
        print("✅ OPTIMIZATION COMPLETE")
        print(f"{'='*70}\n")
        
        print("Optimal Configuration:")
        print(f"  ef_search:   {best_result.config.ef_search}")
        print(f"  num_clients: {best_result.config.num_clients}")
        print(f"  num_threads: {best_result.config.num_threads}")
        
        print(f"\nPerformance:")
        print(f"  QPS:         {best_result.qps:.1f} requests/sec")
        print(f"  Latency Avg: {best_result.latency_avg:.3f} ms")
        print(f"  Latency P50: {best_result.latency_p50:.3f} ms")
        print(f"  Latency P95: {best_result.latency_p95:.3f} ms")
        print(f"  Latency P99: {best_result.latency_p99:.3f} ms")
        
        if best_result.recall_avg is not None:
            print(f"\nRecall:")
            print(f"  Average:     {best_result.recall_avg:.2%}")
            if best_result.recall_min is not None:
                print(f"  Min:         {best_result.recall_min:.2%}")
            if best_result.recall_max is not None:
                print(f"  Max:         {best_result.recall_max:.2%}")
        
        if best_result.baseline_latency_avg is not None:
            overhead = best_result.latency_avg - best_result.baseline_latency_avg
            print(f"\nBaseline:")
            print(f"  Network RTT: {best_result.baseline_latency_avg:.3f} ms")
            print(f"  Search Overhead: {overhead:.3f} ms")
        
        # Show exact command for reproducibility
        print(f"\n{'='*70}")
        print("📋 Command to Reproduce This Configuration:")
        print(f"{'='*70}")
        
        cluster_flag = "--cluster" if wrapper._detect_cluster(args.host) else ""
        cmd = (
            f"{wrapper.binary} -t vec-query -h {args.host} "
            f"--dataset {dataset_path} "
            f"--search --search-name {index_name} "
            f"-c {best_result.config.num_clients} "
            f"--threads {best_result.config.num_threads} "
            f"-n {best_result.config.num_requests} "
            f"--ef-search {best_result.config.ef_search}"
        )
        if cluster_flag:
            cmd += f" {cluster_flag}"
        
        print(f"\n{cmd}\n")
        print(f"{'='*70}")
        
        if args.output:
            print(f"\n✅ Optimization results saved to: {args.output}")
        
        print()
        return 0
    else:
        print("❌ OPTIMIZATION FAILED")
        print(f"{'='*70}\n")
        print(f"Could not find configuration with recall >= {args.target_recall:.0%}\n")
        print("Suggestions:")
        print(f"  - Lower --target-recall (try 0.90 or 0.85)")
        print(f"  - Increase --max-iterations (current: {args.max_iterations})")
        print(f"  - Check that index '{index_name}' exists and is configured correctly")
        print(f"{'='*70}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
