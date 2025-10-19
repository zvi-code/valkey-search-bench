#!/bin/bash

# Entry point for Valkey vector search testing
# Redirects to the organized vector-testing directory

VALKEY_HOME="${VALKEY_HOME:-/home/ubuntu/valkey}"

echo "Valkey Vector Search Testing"
echo "============================="
echo
echo "Testing scripts have been organized in: ${VALKEY_HOME}/vector-testing/"
echo
echo "Quick start:"
echo "  # Download OpenAI datasets and test"
echo "  cd \${VALKEY_HOME}/vector-testing"
echo "  ./download_datasets.sh --openai --test"
echo
echo "  # Or download all datasets"
echo "  ./download_datasets.sh --all"
echo
echo "  # Run tests"
echo "  ./test_multi_dataset.sh --dataset openai-1m"
echo
echo "For full documentation, see: ${VALKEY_HOME}/vector-testing/README.md"
echo

# If arguments provided, forward to download script
if [ $# -gt 0 ]; then
    echo "Forwarding to download script..."
    exec "${VALKEY_HOME}/vector-testing/download_datasets.sh" "$@"
fi