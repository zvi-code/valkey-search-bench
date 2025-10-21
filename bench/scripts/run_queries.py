#!/usr/bin/env python3
"""
SPDX-License-Identifier: BSD-3-Clause

Copyright (c) 2024-present, Zvi Schneider

Auto-optimized Query Benchmark

Automatically finds optimal configuration for maximum query throughput
while maintaining reasonable latency. Only requires dataset and host.

The script:
1. Detects dataset properties (dimensions, size)
2. Determines optimal ef_search, num_clients, and num_threads
3. Runs benchmark with optimal configuration
4. Reports results and the exact command used

Example usage:
    # Simple - just dataset name and host
    ./run_queries.py --host localhost --dataset openai-large-5m
    
    # With custom target recall
    ./run_queries.py --host localhost --dataset sift-128 --target-recall 0.98
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


def estimate_optimal_config(num_vectors: int, dimensions: int, target_recall: float):
    """Estimate optimal configuration based on dataset properties.
    
    Heuristics:
    - ef_search: Higher for high recall, scales with dimensions
    - num_clients: More clients for throughput, but watch latency
    - num_threads: Based on available CPU cores
    
    Returns:
        (ef_search_min, ef_search_max, num_clients_max, num_threads)
    """
    import os
    
    # Detect CPU cores
    num_cores = os.cpu_count() or 4
    num_threads = min(num_cores, 8)  # Cap at 8 for most workloads
    
    # ef_search estimation based on dimensions and target recall
    if dimensions <= 128:
        # Small dimensions (SIFT, MNIST)
        ef_base = 50
    elif dimensions <= 512:
        # Medium dimensions (GloVe)
        ef_base = 100
    else:
        # Large dimensions (OpenAI, Cohere)
        ef_base = 150
    
    # Scale by target recall
    if target_recall >= 0.99:
        ef_multiplier = 3.0
    elif target_recall >= 0.95:
        ef_multiplier = 2.0
    else:
        ef_multiplier = 1.5
    
    ef_search_min = int(ef_base * ef_multiplier)
    ef_search_max = int(ef_base * ef_multiplier * 2)
    
    # num_clients: More clients = more throughput, but watch latency
    # Start conservative, let binary search find optimal
    if num_vectors and num_vectors < 100000:
        num_clients_max = 50
    elif num_vectors and num_vectors < 1000000:
        num_clients_max = 100
    else:
        num_clients_max = 200
    
    return ef_search_min, ef_search_max, num_clients_max, num_threads


def main():
    parser = argparse.ArgumentParser(
        description="Auto-optimized query benchmark - only requires dataset and host",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Simplest usage - automatic optimization
  %(prog)s --host localhost --dataset openai-large-5m
  
  # With custom target recall
  %(prog)s --host localhost --dataset sift-128 --target-recall 0.98
  
  # With custom number of requests
  %(prog)s --host localhost --dataset cohere-large-10m --num-requests 50000

The script automatically:
  - Detects dataset dimensions and size
  - Determines optimal ef_search range
  - Finds optimal number of clients via binary search
  - Runs benchmark with optimal configuration
  - Reports exact command used for reproducibility
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
        help="Target recall threshold (default: 0.95)"
    )
    parser.add_argument(
        "--max-p99-latency",
        type=float,
        help="Maximum acceptable P99 latency in ms (default: auto - 20ms for small dims, 50ms for large)"
    )
    parser.add_argument(
        "--num-requests",
        type=int,
        default=10000,
        help="Number of requests to run (default: 10000)"
    )
    
    # Output
    parser.add_argument(
        "--output",
        help="Save results to CSV file"
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="Enable verbose output"
    )
    parser.add_argument(
        "--skip-optimization",
        action="store_true",
        help="Skip optimization, just run with estimated config once"
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
        wrapper = ValKeyBenchmarkWrapper(verbose=args.verbose)
    except BinaryNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    
    print(f"\n{'='*70}")
    print(f"Auto-Optimized Query Benchmark")
    print(f"{'='*70}")
    print(f"Host:        {args.host}")
    print(f"Dataset:     {dataset_name}")
    print(f"Index Name:  {dataset_name}")
    print(f"File Path:   {dataset_path}")
    print(f"{'='*70}\n")
    
    # Step 1: Detect dataset properties
    print("📊 Detecting dataset properties...")
    try:
        num_vectors, dimensions = detect_dataset_info(dataset_path)
        if num_vectors:
            print(f"   Vectors:    {num_vectors:,}")
        print(f"   Dimensions: {dimensions}")
    except BenchmarkError as e:
        print(f"   Warning: {e}")
        print(f"   Continuing with default configuration...")
        dimensions = 128
        num_vectors = None
    
    # Step 2: Estimate optimal configuration
    print(f"\n🔧 Estimating optimal configuration...")
    ef_min, ef_max, max_clients, num_threads = estimate_optimal_config(
        num_vectors, dimensions, args.target_recall
    )
    
    print(f"   ef_search range:   [{ef_min}, {ef_max}]")
    print(f"   num_threads:       {num_threads}")
    print(f"   max_clients:       {max_clients}")
    print(f"   target_recall:     {args.target_recall:.2%}")
    
    # Auto-determine latency threshold if not specified
    if args.max_p99_latency is None:
        if dimensions <= 128:
            max_p99_latency = 20.0  # Fast for small dims
        elif dimensions <= 512:
            max_p99_latency = 30.0  # Medium
        else:
            max_p99_latency = 50.0  # More lenient for large dims
    else:
        max_p99_latency = args.max_p99_latency
    
    print(f"   max_p99_latency:   {max_p99_latency:.1f}ms")
    
    # Step 3: Search for optimal ef_search
    print(f"\n🔍 Searching for optimal ef_search...")
    
    best_result = None
    best_ef = None
    
    # Try a few ef_search values in the range
    ef_values = [ef_min, (ef_min + ef_max) // 2, ef_max]
    
    for ef_search in ef_values:
        print(f"\n   Testing ef_search={ef_search}...")
        
        base_config = BenchmarkConfig(
            host=args.host,
            dataset=dataset_path,
            num_clients=10,  # Start conservative
            num_threads=num_threads,
            num_requests=args.num_requests,
            ef_search=ef_search,
            operation="vec-query",
            extra_args=["--search-name", dataset_name]  # Use dataset name as index name
        )
        
        if args.skip_optimization:
            # Just run once with estimated config
            with wrapper.stage("vec-query", tag=f"ef_{ef_search}"):
                try:
                    result = wrapper.run(base_config)
                    print(f"   ✓ QPS={result.qps:.1f}, "
                          f"Recall={result.recall_avg:.2%}, "
                          f"P99={result.latency_p99:.3f}ms")
                    
                    if result.recall_avg >= args.target_recall:
                        best_result = result
                        best_ef = ef_search
                        break
                except BenchmarkError as e:
                    print(f"   ✗ Failed: {e}")
                    continue
        else:
            # Find optimal num_clients for this ef_search
            try:
                result = wrapper.find_max_qps_with_constraints(
                    base_config=base_config,
                    min_recall=args.target_recall,
                    max_latency_p99=max_p99_latency,
                    param_name="num_clients",
                    min_val=5,
                    max_val=max_clients,
                )
                
                if result:
                    print(f"   ✓ Optimal: clients={result.config.num_clients}, "
                          f"QPS={result.qps:.1f}, "
                          f"Recall={result.recall_avg:.2%}, "
                          f"P99={result.latency_p99:.3f}ms")
                    
                    if best_result is None or result.qps > best_result.qps:
                        best_result = result
                        best_ef = ef_search
                else:
                    print(f"   ✗ Could not meet constraints")
            
            except BenchmarkError as e:
                print(f"   ✗ Failed: {e}")
                continue
        
        # If we found a good config and are in fast mode, stop
        if args.skip_optimization and best_result:
            break
    
    # Step 4: Report results
    print(f"\n{'='*70}")
    if best_result:
        print("✅ OPTIMAL CONFIGURATION FOUND")
        print(f"{'='*70}\n")
        
        print("Configuration:")
        print(f"  ef_search:   {best_result.config.ef_search}")
        print(f"  num_clients: {best_result.config.num_clients}")
        print(f"  num_threads: {best_result.config.num_threads}")
        print(f"  num_requests: {best_result.config.num_requests}")
        
        print(f"\nPerformance:")
        print(f"  QPS:         {best_result.qps:.1f} requests/sec")
        print(f"  Latency Avg: {best_result.latency_avg:.3f} ms")
        print(f"  Latency P50: {best_result.latency_p50:.3f} ms")
        print(f"  Latency P95: {best_result.latency_p95:.3f} ms")
        print(f"  Latency P99: {best_result.latency_p99:.3f} ms")
        
        if best_result.recall_avg is not None:
            print(f"\nRecall:")
            print(f"  Average:     {best_result.recall_avg:.2%}")
            print(f"  Min:         {best_result.recall_min:.2%}")
            print(f"  Max:         {best_result.recall_max:.2%}")
        
        if best_result.baseline_latency_avg is not None:
            overhead = best_result.latency_avg - best_result.baseline_latency_avg
            print(f"\nBaseline:")
            print(f"  Network RTT: {best_result.baseline_latency_avg:.3f} ms")
            print(f"  Overhead:    {overhead:.3f} ms")
        
        # Show exact command for reproducibility
        print(f"\n{'='*70}")
        print("📋 Exact Command for Reproducibility:")
        print(f"{'='*70}")
        
        cluster_flag = "--cluster" if wrapper._detect_cluster(args.host) else ""
        cmd = (
            f"{wrapper.binary} -t vec-query -h {args.host} "
            f"--dataset {dataset_path} "
            f"--search-name {dataset_name} "
            f"-c {best_result.config.num_clients} "
            f"--threads {best_result.config.num_threads} "
            f"-n {best_result.config.num_requests} "
            f"--ef-search {best_result.config.ef_search}"
        )
        if cluster_flag:
            cmd += f" {cluster_flag}"
        
        print(f"\n{cmd}\n")
        print(f"{'='*70}")
        
        # Save to CSV if requested
        if args.output:
            wrapper.save_results_csv([best_result], args.output)
            print(f"\n✅ Results saved to: {args.output}")
        
        print()
        return 0
    else:
        print("❌ COULD NOT FIND OPTIMAL CONFIGURATION")
        print(f"{'='*70}\n")
        print(f"Could not achieve target recall of {args.target_recall:.2%}")
        print(f"with P99 latency <= {max_p99_latency:.1f}ms\n")
        print("Suggestions:")
        print(f"  - Lower --target-recall (try 0.90 or 0.85)")
        print(f"  - Increase --max-p99-latency")
        print(f"  - Check dataset and index configuration")
        print(f"{'='*70}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
