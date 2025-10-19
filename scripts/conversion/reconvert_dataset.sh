#!/bin/bash
#
# Generic script to reconvert any vectordb-bench dataset with ID sorting fix
# Usage: ./reconvert_dataset.sh <dataset-name> <parquet-dir> <metric>
#
# Examples:
#   ./reconvert_dataset.sh openai-large-5m openai/openai_large_5m COSINE
#   ./reconvert_dataset.sh cohere-large-10m cohere/cohere_large_10m COSINE
#   ./reconvert_dataset.sh sift-128 sift/sift_large_5m L2
#

set -e

if [ $# -lt 2 ]; then
    echo "Usage: $0 <dataset-name> <parquet-dir> [metric]"
    echo ""
    echo "Examples:"
    echo "  $0 openai-large-5m openai/openai_large_5m COSINE"
    echo "  $0 cohere-large-10m cohere/cohere_large_10m COSINE"
    echo "  $0 sift-5m sift/sift_large_5m L2"
    echo "  $0 gist-1m gist/gist_medium_1m L2"
    echo ""
    exit 1
fi

DATASET_NAME="$1"
PARQUET_DIR="$2"
METRIC="${3:-COSINE}"  # Default to COSINE if not specified

VALKEY_HOME="/home/ubuntu/valkey"
DATASETS_ROOT="/mnt/data/datasets"
BUILD_DATASETS="/mnt/data/build-datasets"
PYTHON="/mnt/data/vectordb-bench-env/bin/python"

PARQUET_PATH="$DATASETS_ROOT/$PARQUET_DIR"
HDF5_PATH="$DATASETS_ROOT/$DATASET_NAME.hdf5"
BINARY_PATH="$BUILD_DATASETS/$DATASET_NAME.bin"
SYMLINK_PATH="$VALKEY_HOME/build-debug/$DATASET_NAME.bin"

echo "======================================================================"
echo "Dataset Reconversion (with ID sorting fix)"
echo "======================================================================"
echo ""
echo "Dataset:     $DATASET_NAME"
echo "Parquet:     $PARQUET_PATH"
echo "HDF5:        $HDF5_PATH"
echo "Binary:      $BINARY_PATH"
echo "Metric:      $METRIC"
echo ""

# Verify parquet directory exists
if [ ! -d "$PARQUET_PATH" ]; then
    echo "❌ Error: Parquet directory not found: $PARQUET_PATH"
    exit 1
fi

# Delete old files
echo "🗑️  Removing old files (if they exist)..."
rm -f "$HDF5_PATH"
rm -f "$BINARY_PATH"
echo "✓ Cleanup complete"
echo ""

# Convert parquet → HDF5 (with ID sorting)
echo "======================================================================"
echo "Step 1: Converting parquet → HDF5 (with ID sorting fix)"
echo "======================================================================"
echo ""
$PYTHON -u "$VALKEY_HOME/convert_parquet_to_hdf5_fast.py" \
    "$PARQUET_PATH" \
    "$HDF5_PATH" \
    --name "$DATASET_NAME"

echo ""
echo "======================================================================"
echo "Step 2: Converting HDF5 → binary"
echo "======================================================================"
echo ""
python3 "$VALKEY_HOME/utils/datasets/prepare_binary.py" \
    "$HDF5_PATH" \
    "$BINARY_PATH" \
    --name "$DATASET_NAME" \
    --metric "$METRIC" \
    --max-neighbors 100

echo ""
echo "======================================================================"
echo "Step 3: Creating symlink"
echo "======================================================================"
ln -sf "$BINARY_PATH" "$SYMLINK_PATH"
echo "✓ Symlink created: $SYMLINK_PATH -> $BINARY_PATH"

echo ""
echo "======================================================================"
echo "✓ Reconversion complete!"
echo "======================================================================"
echo ""
echo "Files created:"
ls -lh "$HDF5_PATH"
ls -lh "$BINARY_PATH"
ls -lh "$SYMLINK_PATH"
echo ""
echo "Next: Run benchmarks with:"
echo "  cd /home/ubuntu/valkey/vector-testing"
echo "  export HOST=your-endpoint.amazonaws.com"
echo "  ./test_multi_dataset.sh --dataset $DATASET_NAME"
echo ""
