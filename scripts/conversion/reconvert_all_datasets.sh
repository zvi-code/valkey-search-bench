#!/bin/bash
#
# Reconvert ALL vectordb-bench datasets with ID sorting fix
# Run in tmux: tmux new -s reconvert './reconvert_all_datasets.sh 2>&1 | tee /tmp/reconvert_all.log'
#

set -e

SCRIPT_DIR="/home/ubuntu/valkey"
LOG_FILE="/tmp/reconvert_all.log"

echo "======================================================================"
echo "BATCH RECONVERSION - ALL VECTORDB-BENCH DATASETS"
echo "======================================================================"
echo "This will reconvert all datasets with the ID sorting fix"
echo "Started: $(date)"
echo ""

# OpenAI datasets
echo "### OpenAI Datasets ###"
"$SCRIPT_DIR/reconvert_dataset.sh" openai-medium-500k openai/openai_medium_500k COSINE
echo ""
"$SCRIPT_DIR/reconvert_dataset.sh" openai-large-5m openai/openai_large_5m COSINE
echo ""

# COHERE datasets
echo "### COHERE Datasets ###"
"$SCRIPT_DIR/reconvert_dataset.sh" cohere-small-100k cohere/cohere_small_100k COSINE
echo ""
"$SCRIPT_DIR/reconvert_dataset.sh" cohere-medium-1m cohere/cohere_medium_1m COSINE
echo ""
"$SCRIPT_DIR/reconvert_dataset.sh" cohere-large-10m cohere/cohere_large_10m COSINE
echo ""

echo "======================================================================"
echo "✅ ALL DATASETS RECONVERTED SUCCESSFULLY!"
echo "======================================================================"
echo "Completed: $(date)"
echo ""
echo "Summary of converted files:"
ls -lh /mnt/data/build-datasets/*.bin
echo ""
echo "All datasets now have correctly sorted vectors by original ID!"
echo "Ground truth neighbors will now match correctly."
echo ""
echo "Next steps:"
echo "  1. Flush the database"
echo "  2. Re-run all benchmarks"
echo "  3. Verify recall results match ground truth"
echo ""
