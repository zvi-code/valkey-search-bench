# Valkey Vector Search Testing Framework

This framework provides comprehensive testing for Valkey vector search performance using modern production datasets for RAG and semantic caching applications, plus standard ANN benchmarks.

## Overview

The framework consists of:

1. **`download_datasets.sh`** - Downloads production datasets and converts to Valkey binary format
2. **`test_multi_dataset.sh`** - Runs ef_search parameter sweeps across different datasets
3. **`_util_analyze_results.py`** - Analyzes and compares performance across datasets
4. **Automated index management** - Handles Valkey's 10-index limit with proper cleanup

## Prerequisites

1. **Valkey Server Running**
   ```bash
   # Start Valkey with vector search module
   # Set HOST environment variable to your Valkey instance
   export HOST="your-valkey-host:6379"
   ```

2. **Environment Setup**
   ```bash
   # Set Valkey installation path (optional if using default)
   export VALKEY_HOME="/home/ubuntu/valkey"

   # Navigate to testing directory
   cd $VALKEY_HOME/vector-testing
   ```

## Quick Start Examples

### 1. Essential Test (5-10 minutes)
Test with small, fast datasets for verification:

```bash
./download_datasets.sh --quick --test
```
Downloads: GloVe-25 (1.2M), SIFT-128 (1M), Fashion-MNIST (60K)

### 2. Modern RAG Test (20-30 minutes)
Test with production-scale RAG datasets:

```bash
./download_datasets.sh --openai --test
```
Downloads: Cohere Wikipedia (35M), MS MARCO (8.8M), GloVe-100 (1.2M), DEEP-96 (10M)

### 3. Comprehensive Test (1-2 hours)
Test all available datasets:

```bash
./download_datasets.sh --all
./test_multi_dataset.sh --all --queries 10000
```

## Complete Dataset Catalog

### Modern RAG and Semantic Caching Datasets (Production-Scale)

| Dataset | Dims | Vectors | Size | Source | Metric | Ground Truth | Description | Download Time |
|---------|------|---------|------|--------|--------|--------------|-------------|---------------|
| **cohere-wiki-simple** | 768 | 486K | ~1.5GB | Hugging Face | Cosine | ⚠️ Computed | Cohere Wikipedia Simple English | 5-10 min |
| **cohere-wiki-en** | 768 | 35.2M | ~110GB | Hugging Face | Cosine | ⚠️ Computed | Cohere Wikipedia English 35M | 60-120 min |
| **fiqa** | 768 | 57K | ~175MB | BEIR | Cosine | ⚠️ Synthetic | FiQA financial Q&A (BEIR qrels) | 2-3 min |
| **msmarco** | 768 | 8.8M | ~27GB | BEIR | Cosine | ⚠️ Synthetic | MS MARCO passage retrieval | 15-30 min |
| **trec-covid** | 768 | 171K | ~525MB | BEIR | Cosine | ⚠️ Synthetic | TREC-COVID biomedical retrieval | 2-5 min |

### Standard ANN Benchmarks (Verified Float32 + Precomputed Ground Truth)

| Dataset | Dims | Vectors | Size | Source | Metric | Ground Truth | Description | Download Time |
|---------|------|---------|------|--------|--------|--------------|-------------|---------------|
| **glove-25** | 25 | 1.18M | ~120MB | ann-benchmarks | Cosine | ✅ Precomputed | GloVe word embeddings 25d | 30-60 sec |
| **glove-50** | 50 | 1.18M | ~240MB | ann-benchmarks | Cosine | ✅ Precomputed | GloVe word embeddings 50d | 1-2 min |
| **glove-100** | 100 | 1.18M | ~480MB | ann-benchmarks | Cosine | ✅ Precomputed | GloVe word embeddings 100d | 2-3 min |
| **sift-128** | 128 | 1.0M | ~500MB | ann-benchmarks | L2 | ✅ Precomputed | SIFT image descriptors | 2-3 min |
| **gist-960** | 960 | 1.0M | ~3.6GB | ann-benchmarks | L2 | ✅ Precomputed | GIST image descriptors | 10-15 min |
| **fashion-mnist** | 784 | 60K | ~200MB | ann-benchmarks | L2 | ✅ Precomputed | Fashion MNIST images | 30-60 sec |
| **deep-96** | 96 | 10.0M | ~3.6GB | ann-benchmarks | Cosine | ✅ Precomputed | Deep image embeddings | 10-20 min |

### Ground Truth Information

All datasets support proper recall validation, but with different approaches:

- **✅ Precomputed**: ann-benchmarks datasets include precomputed k-NN neighbors (instant download)
- **⚠️ Computed**: Cohere datasets compute k-NN from precomputed embeddings (requires user confirmation)
- **⚠️ Synthetic**: BEIR datasets create synthetic embeddings from text (requires user confirmation)

**Ground Truth Computation Options**:
- **Automatic**: Use `--force` flag to skip confirmation prompts
- **Interactive**: Default mode asks for user confirmation before computation
- **Skip**: Choose 'N' to skip datasets requiring computation

**Note**: All datasets are float32 format. For fastest setup, use ann-benchmarks datasets with precomputed ground truth.

## Detailed Usage Instructions

### Step-by-Step Guide

#### 1. Environment Setup
```bash
# Navigate to Valkey installation
cd /home/ubuntu/valkey

# Set environment variables
export VALKEY_HOME="/home/ubuntu/valkey"
export HOST="your-valkey-host:6379"

# Navigate to testing framework
cd vector-testing
```

#### 2. List Available Datasets
```bash
# Show all available datasets with details
./download_datasets.sh --list

# Output example:
#   cohere-wiki-en      1024 dims, 35200000 vectors - Cohere Wikipedia English embeddings
#   msmarco             768 dims,  8800000 vectors - MS MARCO passage retrieval
#   glove-25            25 dims,   1183514 vectors - GloVe word embeddings 25d
```

#### 3. Download Datasets by Category

**Quick Testing (Recommended for first-time users)**
```bash
# Downloads: glove-25, sift-128, fashion-mnist
./download_datasets.sh --quick

# Downloads + runs basic tests
./download_datasets.sh --quick --test
```

**Modern RAG & Semantic Caching**
```bash
# Downloads: cohere-wiki-simple (486K), cohere-wiki-en (35M), fiqa (57K), msmarco (8.8M), glove-100, deep-96
./download_datasets.sh --openai

# Downloads + runs tests automatically
./download_datasets.sh --openai --test

# Skip confirmation prompts for ground truth computation
./download_datasets.sh --openai --force
```

**All Available Datasets**
```bash
# Downloads all 12 datasets (may take 3-6 hours with ground truth computation)
./download_datasets.sh --all
```

**Individual Datasets**
```bash
# Download specific dataset
./download_datasets.sh --dataset cohere-wiki-simple
./download_datasets.sh --dataset sift-128
./download_datasets.sh --dataset msmarco
```

#### 4. Test Vector Search Performance

**Single Dataset Testing**
```bash
# Test with default parameters (1000 queries, ef_search: 50,100,150,200,250,300,400,500)
./test_multi_dataset.sh --dataset glove-25

# Custom testing parameters
./test_multi_dataset.sh \
    --dataset sift-128 \
    --queries 5000 \
    --ef-search "50,100,200,400,800" \
    --concurrency 50
```

**Multi-Dataset Testing**
```bash
# Test all downloaded datasets
./test_multi_dataset.sh --all

# Test specific datasets with custom parameters
./test_multi_dataset.sh \
    --dataset glove-25,sift-128,fashion-mnist \
    --queries 2000 \
    --ef-search "100,200,400"
```

**Advanced Testing Options**
```bash
# Skip data insertion (if already loaded)
./test_multi_dataset.sh --dataset glove-25 --skip-insert

# Insert data only (no performance testing)
./test_multi_dataset.sh --dataset glove-25 --insert-only

# Test with memory monitoring
./test_multi_dataset.sh --dataset deep-96 --monitor-memory
```

#### 5. Analyze Results

**Basic Analysis**
```bash
# Analyze latest results
python3 _util_analyze_results.py

# Generate detailed report
python3 _util_analyze_results.py --output detailed_analysis.txt

# Analyze specific dataset results
python3 _util_analyze_results.py --dataset cohere-wiki-en
```

## Command Reference

### download_datasets.sh Options

| Option | Description | Example |
|--------|-------------|---------|
| `--quick` | Download 3 essential test datasets | `./download_datasets.sh --quick` |
| `--openai` | Download 6 modern RAG datasets | `./download_datasets.sh --openai` |
| `--all` | Download all 12 available datasets | `./download_datasets.sh --all` |
| `--dataset NAME` | Download specific dataset | `./download_datasets.sh --dataset glove-25` |
| `--test` | Run tests after downloading | `./download_datasets.sh --quick --test` |
| `--force` | Skip confirmation prompts | `./download_datasets.sh --openai --force` |
| `--list` | Show available datasets | `./download_datasets.sh --list` |
| `--help` | Show help message | `./download_datasets.sh --help` |

### test_multi_dataset.sh Options

| Option | Description | Default | Example |
|--------|-------------|---------|---------|
| `--dataset NAME` | Test specific dataset | - | `--dataset glove-25` |
| `--all` | Test all downloaded datasets | - | `--all` |
| `--queries N` | Number of test queries | 1000 | `--queries 5000` |
| `--ef-search "X,Y,Z"` | ef_search values to test | "50,100,150,200,250,300,400,500" | `--ef-search "100,200,400"` |
| `--concurrency N` | Concurrent connections | 20 | `--concurrency 50` |
| `--skip-insert` | Skip data insertion | false | `--skip-insert` |
| `--insert-only` | Insert data only | false | `--insert-only` |
| `--help` | Show help message | - | `--help` |

### Dataset Selection Guide

**For Performance Testing:**
- Start with: `glove-25` (fast, 1.2M vectors)
- Production test: `cohere-wiki-simple` (medium, 486K vectors)
- Large scale: `cohere-wiki-en` (large, 35M vectors)

**For Recall Validation:**
- Traditional: `sift-128` (verified ground truth)
- Modern RAG: `msmarco` (BEIR qrels)
- Semantic search: `cohere-wiki-en` (computed similarities)

**For Resource Testing:**
- Memory efficient: `fashion-mnist` (60K vectors)
- High dimensional: `gist-960` (960 dimensions)
- Massive scale: `laion-100m` (100M vectors)

## Output Files

### Test Results

- `ef_search_results_<dataset>_<timestamp>.csv` - Raw test data per dataset
- Individual columns: dataset, ef_search, avg_recall, min_recall, max_recall, queries, qps, latency_ms

### Analysis Reports

- `dataset_analysis_report.txt` - Comprehensive text report
- `ef_search_analysis.csv` - Combined CSV for further analysis
- `comparison_<timestamp>.csv` - Cross-dataset comparison

## Integration with utils/datasets

The framework integrates with the existing `utils/datasets` tools:

1. **prepare_binary.py** - Converts HDF5 to binary format
2. **setup_datasets.sh** - Alternative dataset download script
3. **VST tool** - Can be used for additional dataset management

Example using VST (if installed):

```bash
# Using VST for OpenAI datasets
vst dataset download openai-5m
vst dataset validate ./datasets/openai-5m.h5
```

## Performance Expectations

### Typical Results by Dataset Category

**Small Datasets (60K-500K vectors)**
| Dataset | ef_search | Recall | QPS | Latency | Memory |
|---------|-----------|--------|-----|---------|--------|
| fashion-mnist | 100 | ~85% | 3000+ | <1ms | ~200MB |
| cohere-wiki-simple | 100 | ~75% | 2500+ | 1ms | ~2GB |

**Medium Datasets (1M-2M vectors)**
| Dataset | ef_search | Recall | QPS | Latency | Memory |
|---------|-----------|--------|-----|---------|--------|
| glove-25 | 100 | ~72% | 2000+ | 1-2ms | ~500MB |
| sift-128 | 100 | ~78% | 1800+ | 2ms | ~2GB |

**Large Datasets (8M-35M vectors)**
| Dataset | ef_search | Recall | QPS | Latency | Memory |
|---------|-----------|--------|-----|---------|--------|
| msmarco | 200 | ~68% | 1200+ | 3-4ms | ~25GB |
| cohere-wiki-en | 200 | ~71% | 1000+ | 4-5ms | ~140GB |

**Massive Datasets (100M+ vectors)**
| Dataset | ef_search | Recall | QPS | Latency | Memory |
|---------|-----------|--------|-----|---------|--------|
| laion-100m | 400 | ~65% | 600+ | 8-10ms | ~300GB |

### Performance Tuning Guidelines

**For High Recall (>75%)**
- Use ef_search ≥ 300
- Expected QPS: 500-1500
- Best for: Precision-critical applications

**For Balanced Performance (~70% recall)**
- Use ef_search = 100-200
- Expected QPS: 1000-2500
- Best for: Most production applications

**For High Throughput (>2000 QPS)**
- Use ef_search = 50-100
- Expected recall: 65-72%
- Best for: Real-time applications

**Memory Requirements**
- HNSW index: ~4-6x vector data size
- FLAT index: ~1x vector data size
- Query processing: ~2-4GB additional

## Advanced Usage

### Custom Dataset Addition

To add a new dataset:

1. Add dataset configuration to `test_multi_dataset.sh`:
```bash
DATASET_CONFIG["my-dataset"]="my_index,zvec_my:,dimensions,num_vectors"
```

2. Add download URL to `download_and_prepare_datasets.sh`:
```bash
DATASETS["my-dataset"]="http://url.to/dataset.hdf5,dimensions,vectors,metric"
```

3. Run preparation:
```bash
./download_and_prepare_datasets.sh --dataset my-dataset
./test_multi_dataset.sh --dataset my-dataset
```

### Parallel Testing

For testing multiple datasets in parallel:

```bash
# Run in separate terminals or use GNU parallel
parallel -j 3 ./test_multi_dataset.sh --dataset {} --skip-insert ::: glove-25 sift-128 fashion-mnist
```

### Continuous Monitoring

Monitor Valkey memory during tests:

```bash
# In separate terminal
watch -n 1 './build-debug/bin/valkey-cli -h $HOST info memory | grep used_memory_rss_human'
```

## Troubleshooting & FAQ

### Common Issues

#### 1. Connection Problems
```bash
# Error: "Connection refused" or "Could not connect to Redis"
# Solution: Check Valkey server and HOST variable
echo $HOST  # Should show your Valkey instance
./build-debug/bin/valkey-cli -h $HOST ping  # Should return PONG

# Fix: Set correct HOST
export HOST="your-valkey-host:6379"
```

#### 2. Index Limit Issues
```bash
# Error: "ERR index limit reached"
# Check current indexes
./build-debug/bin/valkey-cli -h $HOST FT._LIST

# Solution: Drop old indexes
./build-debug/bin/valkey-cli -h $HOST FT.DROPINDEX old_index_name

# Or use automatic cleanup in test script
./test_multi_dataset.sh --dataset glove-25  # Will prompt for cleanup
```

#### 3. Memory Issues
```bash
# Error: "OOM command not allowed" or system memory full
# Check current memory usage
./build-debug/bin/valkey-cli -h $HOST info memory

# Solutions:
# 1. Test smaller datasets first
./download_datasets.sh --dataset fashion-mnist
./test_multi_dataset.sh --dataset fashion-mnist

# 2. Reduce query count
./test_multi_dataset.sh --dataset glove-25 --queries 100

# 3. Use fewer ef_search values
./test_multi_dataset.sh --dataset glove-25 --ef-search "100,200"
```

#### 4. Download Issues
```bash
# Error: "Failed to download dataset"
# Check internet connection and disk space
df -h  # Check available disk space
ping huggingface.co  # Check connectivity

# For Hugging Face datasets, ensure authentication (if needed)
# Large datasets may take time - check logs for progress
tail -f /tmp/download.log
```

#### 5. Low Recall Results
```bash
# Issue: Recall below 60%
# Verify dataset integrity
python3 -c "
import h5py
f = h5py.File('/home/ubuntu/valkey/datasets/glove-25.hdf5', 'r')
print(f'Train shape: {f['train'].shape}')
print(f'Test shape: {f['test'].shape}')
print(f'Neighbors shape: {f['neighbors'].shape}')
"

# Increase ef_search for better recall
./test_multi_dataset.sh --dataset glove-25 --ef-search "400,600,800"
```

### Performance Debugging

#### Monitor Memory Usage
```bash
# In separate terminal, monitor Valkey memory
watch -n 1 './build-debug/bin/valkey-cli -h $HOST info memory | grep used_memory_rss_human'

# Monitor system memory
watch -n 1 'free -h'
```

#### Check Index Status
```bash
# List all indexes
./build-debug/bin/valkey-cli -h $HOST FT._LIST

# Get index info
./build-debug/bin/valkey-cli -h $HOST FT.INFO index_name

# Check index size
./build-debug/bin/valkey-cli -h $HOST FT.INFO glove-25-1M-25-100
```

#### Debug Query Performance
```bash
# Test single query with timing
time ./build-debug/bin/valkey-cli -h $HOST \
  FT.SEARCH "glove-25-1M-25-100" \
  "*=>[KNN 10 @vector \$query_vec EF_RUNTIME 100]" \
  PARAMS 2 query_vec "blob_data" \
  RETURN 1 __vector_score

# Enable query logging (if available)
./build-debug/bin/valkey-cli -h $HOST CONFIG SET slowlog-log-slower-than 1000
```

### Dataset-Specific Notes

#### Large Datasets (>10M vectors)
- **cohere-wiki-en**, **laion-100m**: Require 100GB+ RAM
- Test with smaller ef_search values first (50-100)
- Consider distributed setup for production

#### High-Dimensional Datasets
- **gist-960**: High memory usage due to 960 dimensions
- **sift-128**: Good balance of size and dimensions
- Expect slower indexing and querying

#### BEIR Datasets
- **msmarco**, **trec-covid**, **fiqa**: Use relevance judgments
- Recall calculation may differ from similarity-based datasets
- Focus on NDCG metrics for evaluation

### Frequently Asked Questions

**Q: Which dataset should I start with?**
A: Use `fashion-mnist` (60K vectors) for quick testing, then `glove-25` (1.2M vectors) for realistic performance evaluation.

**Q: How much memory do I need?**
A: Rule of thumb: 6x the dataset size for HNSW index. See memory requirements in performance tables above.

**Q: Why is recall different between datasets?**
A: Different ground truth methods:
- ann-benchmarks: Precomputed exact k-NN
- BEIR: Human relevance judgments
- Computed: Similarity thresholds

**Q: Can I test with my own vectors?**
A: Yes, see "Custom Dataset Addition" section. Convert to HDF5 format with train/test/neighbors datasets.

**Q: How do I optimize for production?**
A: Start with balanced settings (ef_search=100-200), monitor memory usage, and tune based on recall requirements.

## Best Practices

1. **Start Small**: Test with fashion-mnist (60K vectors) before large datasets
2. **Monitor Resources**: Watch memory and CPU during tests
3. **Incremental Testing**: Start with few queries and ef_search values
4. **Save Results**: Results are timestamped for comparison
5. **Clean Between Tests**: Drop indexes when switching datasets

## Future Enhancements

- [ ] Support for Hugging Face datasets with Parquet format
- [ ] Integration with Big-ANN billion-scale datasets
- [ ] Automated cluster resource monitoring
- [ ] Real-time visualization dashboard
- [ ] CI/CD integration for regression testing

## Directory Structure

```
valkey/
├── test_vector_search.sh           # Entry point script
└── vector-testing/                 # Vector search testing framework
    ├── README.md                   # This documentation
    ├── download_datasets.sh        # Main dataset download script
    ├── test_multi_dataset.sh       # Multi-dataset testing script
    ├── _util_analyze_results.py    # Results analysis utility
    └── IMPROVEMENTS_SUMMARY.md     # Implementation details
```

## Usage from Valkey Root

```bash
# Quick entry point
./test_vector_search.sh --openai --test

# Or use directly
cd vector-testing/
./download_datasets.sh --openai --test
```

## References

- [ann-benchmarks](http://ann-benchmarks.com/) - Standard benchmark datasets
- [Big-ANN](https://big-ann-benchmarks.com/) - Billion-scale datasets
- [VectorDBBench](https://github.com/zilliztech/VectorDBBench) - Production datasets
- [Hugging Face Datasets](https://huggingface.co/datasets) - ML community datasets