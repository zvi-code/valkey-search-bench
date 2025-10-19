#!/usr/bin/bash
#
# Complete workflow to convert vectordb-bench datasets to Valkey binary format
#
# Usage:
#   ./convert_vectordb_dataset.sh cohere_small_100k cohere-small-100k COSINE
#   ./convert_vectordb_dataset.sh cohere_medium_1m cohere-medium-1m COSINE  
#   ./convert_vectordb_dataset.sh cohere_large_10m cohere-large-10m COSINE
#   ./convert_vectordb_dataset.sh openai_large_5m openai-large-5m COSINE
#

set -e

if [ $# -lt 3 ]; then
    echo "Usage: $0 <parquet_dir> <output_name> <metric>"
    echo ""
    echo "Arguments:"
    echo "  parquet_dir  - Directory containing parquet files (e.g., cohere_small_100k)"
    echo "  output_name  - Name for output files (e.g., cohere-small-100k)"
    echo "  metric       - Distance metric: COSINE, L2, or IP"
    echo ""
    echo "Examples:"
    echo "  $0 cohere/cohere_small_100k cohere-small-100k COSINE"
    echo "  $0 openai/openai_large_5m openai-large-5m COSINE"
    exit 1
fi

PARQUET_DIR="$1"
OUTPUT_NAME="$2"
METRIC="$3"

# Paths
DATASETS_ROOT="/mnt/data/datasets"
PARQUET_PATH="${DATASETS_ROOT}/${PARQUET_DIR}"
HDF5_FILE="${DATASETS_ROOT}/${OUTPUT_NAME}.hdf5"
BIN_FILE="/mnt/data/build-datasets/${OUTPUT_NAME}.bin"
VALKEY_HOME="/home/ubuntu/valkey"
CONVERT_SCRIPT="${VALKEY_HOME}/convert_parquet_to_hdf5.py"
PREPARE_SCRIPT="${VALKEY_HOME}/utils/datasets/prepare_binary.py"
PYTHON="/mnt/data/vectordb-bench-env/bin/python"

echo "======================================================================"
echo "Converting vectordb-bench dataset to Valkey binary format"
echo "======================================================================"
echo "Input:  ${PARQUET_PATH}"
echo "HDF5:   ${HDF5_FILE}"
echo "Binary: ${BIN_FILE}"
echo "Metric: ${METRIC}"
echo ""

# Step 1: Convert parquet to HDF5
if [ -f "$HDF5_FILE" ]; then
    echo "✓ HDF5 file already exists, skipping conversion"
else
    echo "Step 1: Converting parquet to HDF5..."
    $PYTHON "$CONVERT_SCRIPT" "$PARQUET_PATH" "$HDF5_FILE" --name "$OUTPUT_NAME"
    echo ""
fi

# Step 2: Convert HDF5 to binary
if [ -f "$BIN_FILE" ]; then
    echo "✓ Binary file already exists, skipping conversion"
else
    echo "Step 2: Converting HDF5 to binary format..."
    python3 "$PREPARE_SCRIPT" "$HDF5_FILE" "$BIN_FILE" \
        --name "$OUTPUT_NAME" \
        --metric "$METRIC" \
        --max-neighbors 100
    echo ""
fi

# Step 3: Create symlink in build-debug
BUILD_DEBUG="${VALKEY_HOME}/build-debug"
SYMLINK="${BUILD_DEBUG}/${OUTPUT_NAME}.bin"

if [ -L "$SYMLINK" ]; then
    echo "✓ Symlink already exists in build-debug"
else
    echo "Step 3: Creating symlink in build-debug..."
    ln -sf "$BIN_FILE" "$SYMLINK"
    echo "✓ Created: ${SYMLINK} -> ${BIN_FILE}"
fi

echo ""
echo "======================================================================"
echo "✓ Conversion complete!"
echo "======================================================================"
echo "Binary file: ${BIN_FILE}"
echo "Symlink:     ${SYMLINK}"
echo ""
echo "You can now use this dataset with valkey-benchmark:"
echo "  ./build-debug/valkey-benchmark --dataset ${OUTPUT_NAME}"
echo ""
ls -lh "$BIN_FILE"
