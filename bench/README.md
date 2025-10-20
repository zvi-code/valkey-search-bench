# Benchmarking Utilities

Utilities for analyzing benchmark results and multi-dataset testing.

## Tools

### test_multi_dataset.sh

Run benchmarks across multiple datasets with ef_search parameter sweeps.

```bash
./test_multi_dataset.sh --dataset sift-128 --queries 1000 --ef-search "50,100,200"
./test_multi_dataset.sh --all  # Test all available datasets
```

**Features:**
- ef_search parameter sweeping
- Automatic recall validation
- CSV output for analysis
- Index management (respects Valkey's 10-index limit)

### _util_analyze_results.py

Analyze and compare benchmark results across datasets.

```bash
python _util_analyze_results.py results.csv
```

### check_dataset_structure.py

**Moved to**: `prep_datasets/check_dataset_structure.py`

Verify dataset binary format and integrity.

```bash
python ../prep_datasets/check_dataset_structure.py dataset.bin
```

### create_dummy_dataset.py

**Moved to**: `prep_datasets/create_dummy_dataset.py`

Generate small test datasets for development and testing.

```bash
python ../prep_datasets/create_dummy_dataset.py --vectors 1000 --dims 128 --output test.bin
```

## For End Users

**Recommended**: Use the main documentation for benchmarking workflows:

- [BENCHMARKING.md](../../docs/BENCHMARKING.md) - Complete benchmarking guide
- [DATASETS.md](../../docs/DATASETS.md) - Dataset management

## Dataset Management

For downloading and preparing datasets:

```bash
# From project root
./scripts/dataset.sh list              # Show available datasets
./scripts/dataset.sh get <dataset>     # Download and prepare
```

See [DATASETS.md](../../docs/DATASETS.md) for complete documentation.
