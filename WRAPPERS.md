# Wrapper Framework - Implementation Guide

## ✅ Status: IMPLEMENTED

The Python wrapper framework has been successfully implemented following the Keep It Simple (KIS) principle.

## Architecture

```
bench/
├── wrappers/
│   ├── __init__.py           # Package exports
│   └── base_wrapper.py       # All core logic (~580 lines)
│
└── scripts/
    ├── max_qps_recall.py     # Example: Find max QPS at target recall
    └── stage-monitor.sh      # Example: External stage monitoring
```

## Quick Start

See `bench/wrappers/README.md` for complete documentation.

### Basic Example

```python
from wrappers import ValKeyBenchmarkWrapper, BenchmarkConfig

wrapper = ValKeyBenchmarkWrapper(verbose=True)
config = BenchmarkConfig(
    host="localhost",
    dataset="datasets/sift-128.bin",
    num_clients=20,
    ef_search=100
)

# Run benchmark
result = wrapper.run(config, operation="vec-query")
print(f"QPS: {result.qps:.1f}, Recall: {result.recall_avg:.2%}")

# Find max QPS at 95% recall
best = wrapper.binary_search_max_qps(
    base_config=config,
    target_recall=0.95
)
```

## Key Features

1. **Single-file core** - All logic in `base_wrapper.py` for simplicity
2. **Search algorithms** - Binary search, grid search, constrained optimization
3. **Stage signaling** - `[STAGE:START/END]` signals for external monitoring
4. **Auto-detection** - Binary location, cluster mode
5. **Result parsing** - Console output and CSV export
6. **No external dependencies** - Pure Python stdlib

## Stage Signaling Protocol

Stages are emitted to stderr for external tool coordination:

```
[STAGE:START] vec-query:ef_100
[STAGE:END] vec-query:ef_100 duration=12.345s
```

External tools (like `stage-monitor.sh`) can parse these signals to:
- Start/stop perf collection during specific stages
- Skip profiling during warmup/prefill
- Tag collected data with stage names

## Example Scripts

### max_qps_recall.py

```bash
./bench/scripts/max_qps_recall.py \
    --host localhost \
    --dataset datasets/sift-128.bin \
    --target-recall 0.95 \
    --ef-search 100 \
    --output results.csv
```

### stage-monitor.sh

```bash
# Terminal 1: Run benchmark
./bench/scripts/max_qps_recall.py ... 2>&1 | tee bench.log

# Terminal 2: Monitor and collect perf
./bench/scripts/stage-monitor.sh \
    --watch vec-query \
    --collect-perf \
    < bench.log
```

## Implemented Wrappers

✅ **Wrapper #1**: Max QPS at target recall - `max_qps_recall.py`  
✅ **Wrapper #2**: Max QPS with latency constraints - `find_max_qps_with_constraints()`  
✅ **Wrapper #3**: Optimal config discovery - `grid_search()`  
✅ **Wrapper #4**: Profiling integration - `stage-monitor.sh`

## Future Wrappers

See TODO.md for planned wrappers:
- Wrapper #5: Memory saturation testing
- Wrapper #6: Payload impact testing

---

# Original Design Discussion

Let me read the wrapper-related sections in the TODO more carefully: 

Read [](file:///home/ubuntu/valkey-search-benchmark/TODO.md#170-170), lines 170 to 230

Now let me check TODO item #5 (Test Stage and Tag Reporting) which is related to wrapper integration: 

Read [](file:///home/ubuntu/valkey-search-benchmark/TODO.md#30-30), lines 30 to 50

Perfect! Now I have a complete picture. Based on my analysis of:
1. The existing test_multi_dataset.sh bash wrapper
2. The TODO items for wrapper enhancements
3. The requirements (value-centric interface, stage awareness, profiling integration)
4. The config persistence feature

Here's my comprehensive recommendation:

## My Recommendation: **Hybrid Approach with Python as Primary**

### **TL;DR:**
- **Python for main wrappers** (80% of use cases)
- **Bash for simple utilities** (20% of use cases)
- **Generic Python base class/template** for consistency

---

### **Reasoning:**

#### **✅ Why Python for Wrappers:**

1. **Complexity Management**
   - Your wrapper needs are getting sophisticated (binary search for max QPS, grid search, multi-objective optimization)
   - test_multi_dataset.sh is already 577 lines and harder to maintain
   - Parsing CSV outputs, JSON configs, and complex data structures is easier in Python

2. **Stage Awareness & Profiling Integration** (TODO #5, Wrapper #4)
   ```python
   # Python makes this elegant:
   with benchmark.stage("index_creation", tag="initial_load"):
       perf_collector.start(nodes=cluster.nodes)
       result = benchmark.run(operation="vec-ground-truth")
       perf_collector.stop()
   ```

3. **Config Persistence Awareness**
   - Python can easily read/parse the `.valkey-benchmark.conf` files
   - Can intelligently override or preserve settings
   - Can validate configs before running

4. **Data Analysis Built-in**
   - CSV parsing, pandas, matplotlib for result analysis
   - Can generate plots and reports automatically
   - Better error handling and validation

5. **Algorithm Implementation**
   - Binary search for max QPS (Wrapper #1, #2)
   - Grid search for optimal configs (Wrapper #3)
   - Multi-objective optimization
   - These are cleaner in Python

6. **Cluster/Remote Execution**
   - SSH, parallel execution, remote perf collection
   - Python libraries like `paramiko`, `fabric`, `multiprocessing`

#### **✅ When to Use Bash:**

1. **Simple utilities** (like the existing `dataset.sh`)
2. **Quick command chaining**
3. **Shell environment manipulation**
4. **Lightweight tasks that don't need parsing/analysis**

---

### **Proposed Architecture:**

```
bench/
├── wrappers/              # New Python wrapper framework
│   ├── __init__.py
│   ├── base_wrapper.py    # Generic base class
│   ├── config_manager.py  # Parse .valkey-benchmark.conf
│   ├── stage_manager.py   # Stage & tag reporting (TODO #5)
│   ├── perf_collector.py  # Profiling integration (Wrapper #4)
│   └── cluster_manager.py # Remote execution
│
├── scripts/               # Specific wrapper scripts
│   ├── max_qps_recall.py     # Wrapper #1
│   ├── max_qps_sla.py        # Wrapper #2
│   ├── optimize_config.py    # Wrapper #3
│   ├── memory_saturation.py  # Wrapper #5
│   ├── payload_impact.py     # Wrapper #6
│   └── multi_dataset.py      # Rewrite of test_multi_dataset.sh
│
├── perf/                  # Keep existing perf utilities
│   ├── collect-stats.sh   # Simple bash - keep as is
│   └── fix-perf.sh
│
└── utils/                 # Simple bash utilities
    └── dataset.sh         # Keep as is
```

---

### **Generic Base Class Template:**

```python
# bench/wrappers/base_wrapper.py
"""
Base wrapper class for valkey-search-benchmark.

Provides:
- Config persistence awareness
- Stage and tag reporting
- Profiling integration
- Result collection and analysis
- Error handling
"""

import subprocess
import json
import csv
from pathlib import Path
from typing import Dict, List, Optional, Any
from contextlib import contextmanager
from dataclasses import dataclass
import time

@dataclass
class BenchmarkConfig:
    """Wrapper for benchmark configuration."""
    host: str
    dataset: str
    num_clients: int = 10
    num_threads: int = 4
    ef_search: Optional[int] = None
    # ... other common params
    custom_args: Dict[str, Any] = None
    
    def to_cli_args(self) -> List[str]:
        """Convert to CLI arguments."""
        args = ["-h", self.host, "--dataset", self.dataset]
        if self.ef_search:
            args.extend(["--ef-search", str(self.ef_search)])
        # ... build full arg list
        return args

@dataclass
class BenchmarkResult:
    """Standardized result format."""
    qps: float
    latency_avg: float
    latency_p50: float
    latency_p99: float
    recall_avg: float
    recall_min: float
    recall_max: float
    baseline_latency: Optional[float] = None
    processing_overhead: Optional[float] = None
    stage: Optional[str] = None
    tag: Optional[str] = None
    timestamp: float = None
    
    def __post_init__(self):
        if self.timestamp is None:
            self.timestamp = time.time()

class ValKeyBenchmarkWrapper:
    """Base class for all benchmark wrappers."""
    
    def __init__(self, 
                 benchmark_binary: str = None,
                 config_file: str = None,
                 verbose: bool = False):
        self.binary = benchmark_binary or self._find_binary()
        self.config_manager = ConfigManager(config_file)
        self.stage_manager = StageManager()
        self.perf_collector = PerfCollector()
        self.verbose = verbose
        
    def _find_binary(self) -> str:
        """Locate valkey-benchmark binary."""
        # Check build dirs, PATH, etc.
        pass
    
    @contextmanager
    def stage(self, stage_name: str, tag: str = None):
        """Context manager for test stages with profiling."""
        self.stage_manager.enter_stage(stage_name, tag)
        
        # Optionally start profiling
        if self.perf_collector.enabled:
            self.perf_collector.start(stage=stage_name, tag=tag)
        
        try:
            yield
        finally:
            if self.perf_collector.enabled:
                self.perf_collector.stop()
            self.stage_manager.exit_stage()
    
    def run_benchmark(self, 
                      config: BenchmarkConfig,
                      operation: str = "vec-query") -> BenchmarkResult:
        """Run benchmark and parse results."""
        args = [self.binary, "-t", operation] + config.to_cli_args()
        
        if self.verbose:
            print(f"Running: {' '.join(args)}")
        
        result = subprocess.run(args, capture_output=True, text=True)
        
        if result.returncode != 0:
            raise RuntimeError(f"Benchmark failed: {result.stderr}")
        
        return self._parse_output(result.stdout)
    
    def _parse_output(self, output: str) -> BenchmarkResult:
        """Parse benchmark output into structured result."""
        # Extract metrics from output
        # Handle CSV, console output formats
        pass
    
    def binary_search_max_qps(self,
                               base_config: BenchmarkConfig,
                               target_recall: float,
                               param_name: str = "num_clients",
                               min_val: int = 1,
                               max_val: int = 100) -> BenchmarkResult:
        """Binary search to find max QPS at target recall."""
        # Generic binary search implementation
        pass
    
    def grid_search(self,
                    base_config: BenchmarkConfig,
                    param_grid: Dict[str, List]) -> List[BenchmarkResult]:
        """Grid search over parameter combinations."""
        pass
    
    def save_results(self, 
                     results: List[BenchmarkResult],
                     output_file: str):
        """Save results to CSV."""
        pass
```

---

### **Example Specific Wrapper:**

```python
# bench/scripts/max_qps_recall.py
"""
Find maximum QPS at target recall threshold.
Implements TODO Wrapper #1.
"""

from wrappers.base_wrapper import ValKeyBenchmarkWrapper, BenchmarkConfig
import argparse

def main():
    parser = argparse.ArgumentParser(
        description="Find max QPS at target recall"
    )
    parser.add_argument("--host", required=True)
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--target-recall", type=float, default=0.95)
    parser.add_argument("--ef-search", type=int, required=True)
    parser.add_argument("--output", default="max_qps_results.csv")
    args = parser.parse_args()
    
    wrapper = ValKeyBenchmarkWrapper(verbose=True)
    
    config = BenchmarkConfig(
        host=args.host,
        dataset=args.dataset,
        ef_search=args.ef_search
    )
    
    with wrapper.stage("max_qps_search", tag=f"recall_{args.target_recall}"):
        result = wrapper.binary_search_max_qps(
            base_config=config,
            target_recall=args.target_recall,
            param_name="num_clients",
            min_val=1,
            max_val=200
        )
    
    print(f"Max QPS: {result.qps} at {result.num_clients} clients")
    print(f"Recall: {result.recall_avg:.2%}")
    
    wrapper.save_results([result], args.output)

if __name__ == "__main__":
    main()
```

---

### **Migration Path:**

1. **Phase 1:** Create Python framework (`wrappers/` directory)
2. **Phase 2:** Implement 1-2 wrappers (e.g., max_qps_recall.py)
3. **Phase 3:** Rewrite test_multi_dataset.sh as multi_dataset.py
4. **Phase 4:** Keep bash scripts only for simple utils

---

### **Summary:**

| Aspect | Bash | Python |
|--------|------|--------|
| **Simple utilities** | ✅ Perfect | ❌ Overkill |
| **Complex wrappers** | ❌ Hard to maintain | ✅ Ideal |
| **Data parsing** | ❌ `awk`/`sed` fragile | ✅ Native |
| **Profiling integration** | ⚠️ Possible but messy | ✅ Clean |
| **Config awareness** | ⚠️ Manual parsing | ✅ Easy |
| **Algorithms** | ❌ Very hard | ✅ Natural |
| **Error handling** | ⚠️ Limited | ✅ Robust |
| **Maintainability** | ❌ Gets messy at scale | ✅ Scales well |

**Recommendation: Use Python with a generic base class template. Keep bash for simple utilities only.**

Would you like me to start implementing this architecture? I can create the base wrapper framework and one or two example wrappers to demonstrate the pattern.