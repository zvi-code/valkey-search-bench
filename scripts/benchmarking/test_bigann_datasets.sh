#!/bin/bash

# Big-ANN dataset testing script for Valkey
# Tests Big-ANN datasets with valkey-benchmark using the existing binary format

set -euo pipefail

# Environment variables
VALKEY_HOME="${VALKEY_HOME:-/home/ubuntu/valkey}"
# HOST="${HOST:-ec-search-zvi-ec-1shard-no-tls-0001-001.ajfdds.0001.euw1devo.cache.amazonaws.com}"
if [ -z "${HOST:-}" ]; then
    echo "ERROR: HOST environment variable not set"
    echo "Set HOST to the Valkey/Redis cluster endpoint"
    exit 1
fi
BINARY_DIR="${VALKEY_HOME}/build-debug"
BENCHM="${BINARY_DIR}/bin/valkey-benchmark"
CLI="${BINARY_DIR}/bin/valkey-cli"

# Test parameters
NUM_QUERIES="${NUM_QUERIES:-1000}"
CONCURRENCY="${CONCURRENCY:-20}"
THREADS="${THREADS:-20}"
EF_SEARCH_VALUES="${EF_SEARCH_VALUES:-50,100,200,400}"
K_NEIGHBORS=100

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

print_header() {
    echo -e "${BLUE}=== $1 ===${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

# Extended dataset configurations including Big-ANN datasets
declare -A DATASET_CONFIG

# === Standard ANN-Benchmarks (existing) ===
DATASET_CONFIG["glove-25"]="glove-25,zvec_glove25:,25,1183514,100"
DATASET_CONFIG["glove-50"]="glove-50,zvec_glove50:,50,1183514,100"
DATASET_CONFIG["glove-100"]="glove-100,zvec_glove100:,100,1183514,100"
DATASET_CONFIG["glove-200"]="glove-200,zvec_glove200:,200,1183514,100"
DATASET_CONFIG["sift-128"]="sift-128,zvec_sift:,128,1000000,100"
DATASET_CONFIG["gist-960"]="gist-960,zvec_gist:,960,1000000,100"
DATASET_CONFIG["fashion-mnist"]="fashion-mnist,zvec_fashion:,784,60000,100"
DATASET_CONFIG["mnist"]="mnist,zvec_mnist:,784,60000,100"
DATASET_CONFIG["deep-96"]="deep-96,zvec_deep:,96,10000000,100"
DATASET_CONFIG["lastfm-64"]="lastfm-64,zvec_lastfm:,64,292385,100"
DATASET_CONFIG["nytimes-256"]="nytimes-256,zvec_nytimes256:,256,290000,100"
DATASET_CONFIG["nytimes-16"]="nytimes-16,zvec_nytimes16:,16,290000,100"

# === Big-ANN Datasets (new) ===
DATASET_CONFIG["bigann-1M"]="bigann-1M,zvec_bigann1m:,128,1000000,100"
DATASET_CONFIG["bigann-10M"]="bigann-10M,zvec_bigann10m:,128,10000000,100"
DATASET_CONFIG["bigann-100M"]="bigann-100M,zvec_bigann100m:,128,100000000,100"
DATASET_CONFIG["deep-1M"]="deep-1M,zvec_deep1m:,96,1000000,100"
DATASET_CONFIG["deep-10M"]="deep-10M,zvec_deep10m:,96,10000000,100"
DATASET_CONFIG["text2image-1M"]="text2image-1M,zvec_text2img1m:,200,1000000,100"
DATASET_CONFIG["text2image-10M"]="text2image-10M,zvec_text2img10m:,200,10000000,100"
DATASET_CONFIG["msturing-1M"]="msturing-1M,zvec_msturing1m:,100,1000000,100"
DATASET_CONFIG["msturing-10M"]="msturing-10M,zvec_msturing10m:,100,10000000,100"
DATASET_CONFIG["msspacev-1M"]="msspacev-1M,zvec_msspacev1m:,100,1000000,100"

# Function to check if dataset binary exists
check_dataset_binary() {
    local dataset=$1
    local binary_file="${BINARY_DIR}/${dataset}.bin"

    if [ -f "$binary_file" ]; then
        return 0
    else
        return 1
    fi
}

# Function to generate index name
generate_index_name() {
    local dataset_name=$1
    IFS=',' read -r index_name prefix dims expected_vectors k_neighbors <<< "${DATASET_CONFIG[$dataset_name]}"

    # Convert vectors to human-readable format
    local vec_str=""
    if [ "$expected_vectors" -ge 1000000000 ]; then
        vec_str="$((expected_vectors / 1000000000))B"
    elif [ "$expected_vectors" -ge 1000000 ]; then
        vec_str="$((expected_vectors / 1000000))M"
    elif [ "$expected_vectors" -ge 1000 ]; then
        vec_str="$((expected_vectors / 1000))K"
    else
        vec_str="$expected_vectors"
    fi

    echo "${index_name}-${vec_str}-${dims}-${k_neighbors}"
}

# Function to cleanup old indexes
cleanup_old_indexes() {
    print_header "Checking index capacity"

    # Get list of current indexes
    local current_indexes=$($CLI -h "$HOST" --cluster FT._LIST 2>/dev/null | wc -l || echo "0")

    echo "Current indexes: $current_indexes / 10"

    if [ "$current_indexes" -ge "8" ]; then
        print_warning "Near or at index limit. Need to remove old indexes."

        # Get list of indexes
        local indexes=$($CLI -h "$HOST" --cluster FT._LIST 2>/dev/null || true)

        if [ -n "$indexes" ]; then
            echo "Current indexes:"
            echo "$indexes" | head -10

            read -p "Remove ALL existing indexes? [y/N] " -n 1 -r
            echo
            if [[ $REPLY =~ ^[Yy]$ ]]; then
                for index in $indexes; do
                    echo "Dropping index: $index"
                    $CLI -h "$HOST" --cluster FT.DROPINDEX "$index" 2>/dev/null || true
                done
                print_success "Cleared all indexes"
            else
                print_error "Cannot proceed without clearing indexes"
                return 1
            fi
        fi
    fi

    echo
}

# Function to test a dataset
test_dataset() {
    local dataset=$1

    if ! check_dataset_binary "$dataset"; then
        print_warning "Dataset binary not found: ${dataset}.bin"
        echo "Run: ./download_bigann_datasets.sh $dataset"
        return 1
    fi

    # Clean up indexes if needed
    cleanup_old_indexes

    IFS=',' read -r index_name prefix dims expected_vectors k_neighbors <<< "${DATASET_CONFIG[$dataset]}"
    local full_index_name=$(generate_index_name "$dataset")

    print_header "Testing $dataset"
    echo "  Index: $full_index_name"
    echo "  Dimensions: $dims"
    echo "  Vectors: $expected_vectors"
    echo "  Queries: $NUM_QUERIES"

    # Create results file
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local results_file="ef_search_results_${dataset}_${timestamp}.csv"
    echo "dataset,ef_search,avg_recall,min_recall,max_recall,queries,qps,latency_ms" > "$results_file"

    # Test each ef_search value
    IFS=',' read -ra ef_values <<< "$EF_SEARCH_VALUES"
    for ef_search in "${ef_values[@]}"; do
        echo -n "Testing ef_search=$ef_search... "

        # Run benchmark (following test_multi_dataset.sh format)
        local binary_file="${BINARY_DIR}/${dataset}.bin"
        local output=$($BENCHM \
            -h "$HOST" --cluster --rfr no \
            --dataset "$binary_file" \
            -t vec-query --search --vector-dim "$dims" \
            --search-name "$full_index_name" --search-prefix "$prefix" \
            --ef-search "$ef_search" \
            -n "$NUM_QUERIES" -c "$CONCURRENCY" --threads "$THREADS" 2>&1 || true)

        # Parse results (following test_multi_dataset.sh format)
        if echo "$output" | grep -q "Average:"; then
            local avg_recall=$(echo "$output" | grep "Average:" | awk '{print $2}' | sed 's/%//')
            local min_recall=$(echo "$output" | grep "Min:" | awk '{print $2}' | sed 's/%//')
            local max_recall=$(echo "$output" | grep "Max:" | awk '{print $2}' | sed 's/%//')
            local total_queries=$(echo "$output" | grep "Queries evaluated:" | awk '{print $3}')
            local qps=$(echo "$output" | grep "throughput summary:" | awk '{print $3}')
            local latency=$(echo "$output" | grep "avg       min" -A 1 | tail -1 | awk '{print $1}')

            # Use defaults if not found
            avg_recall=${avg_recall:-"N/A"}
            min_recall=${min_recall:-"N/A"}
            max_recall=${max_recall:-"N/A"}
            total_queries=${total_queries:-"0"}
            qps=${qps:-"N/A"}
            latency=${latency:-"N/A"}

            echo "$dataset,$ef_search,$avg_recall,$min_recall,$max_recall,$total_queries,$qps,$latency" >> "$results_file"
            echo -e "${GREEN}✓${NC} Recall: ${avg_recall}%, QPS: $qps"
        else
            echo -e "${RED}✗${NC} Test failed"
            echo "$output" | tail -5
        fi
    done

    print_success "Results saved to: $results_file"
    echo
}

# Function to test multiple datasets
test_multiple_datasets() {
    local datasets=("$@")
    local total=${#datasets[@]}
    local current=0

    print_header "Testing ${total} datasets"

    for dataset in "${datasets[@]}"; do
        current=$((current + 1))
        echo "[$current/$total] Processing: $dataset"
        test_dataset "$dataset"
    done
}

# Function to list available datasets
list_datasets() {
    print_header "Available Big-ANN Datasets"

    echo "Standard ANN-Benchmarks:"
    for dataset in glove-25 glove-50 glove-100 sift-128 gist-960 fashion-mnist deep-96; do
        if check_dataset_binary "$dataset"; then
            echo -e "  ${GREEN}✓${NC} $dataset"
        else
            echo -e "  ${YELLOW}○${NC} $dataset (not downloaded)"
        fi
    done

    echo
    echo "Big-ANN Datasets:"
    for dataset in bigann-1M bigann-10M bigann-100M deep-1M deep-10M text2image-1M text2image-10M msturing-1M msturing-10M msspacev-1M; do
        if check_dataset_binary "$dataset"; then
            echo -e "  ${GREEN}✓${NC} $dataset"
        else
            echo -e "  ${YELLOW}○${NC} $dataset (not downloaded)"
        fi
    done
}

# Function to run quick test
run_quick_test() {
    print_header "Quick Test (3 small datasets)"

    # Download if needed
    if ! check_dataset_binary "glove-25"; then
        echo "Downloading glove-25..."
        ./download_bigann_datasets.sh glove-25
    fi

    if ! check_dataset_binary "fashion-mnist"; then
        echo "Downloading fashion-mnist..."
        ./download_bigann_datasets.sh fashion-mnist
    fi

    if ! check_dataset_binary "bigann-1M"; then
        echo "Downloading bigann-1M..."
        ./download_bigann_datasets.sh bigann-1M
    fi

    # Test
    test_multiple_datasets glove-25 fashion-mnist bigann-1M
}

# Function to run comprehensive test
run_comprehensive_test() {
    print_header "Comprehensive Big-ANN Test"

    local datasets=(
        # Start with smaller datasets
        "glove-25"
        "glove-100"
        "sift-128"
        "fashion-mnist"
        # Medium Big-ANN datasets
        "bigann-1M"
        "deep-1M"
        "text2image-1M"
        "msturing-1M"
        # Larger datasets (if available)
        "bigann-10M"
        "deep-10M"
        "msturing-10M"
    )

    # Filter to only available datasets
    local available=()
    for dataset in "${datasets[@]}"; do
        if check_dataset_binary "$dataset"; then
            available+=("$dataset")
        fi
    done

    if [ ${#available[@]} -eq 0 ]; then
        print_error "No datasets found. Run ./download_bigann_datasets.sh first"
        exit 1
    fi

    echo "Found ${#available[@]} datasets to test"
    test_multiple_datasets "${available[@]}"
}

# Main function
main() {
    case "${1:-}" in
        --list)
            list_datasets
            ;;
        --quick)
            run_quick_test
            ;;
        --comprehensive)
            run_comprehensive_test
            ;;
        --dataset)
            if [ $# -lt 2 ]; then
                print_error "Usage: $0 --dataset <name>"
                exit 1
            fi
            test_dataset "$2"
            ;;
        --help|"")
            echo "Big-ANN Dataset Testing for Valkey"
            echo
            echo "Usage: $0 [OPTION]"
            echo
            echo "Options:"
            echo "  --list              List available datasets"
            echo "  --quick             Run quick test (3 datasets)"
            echo "  --comprehensive     Test all available datasets"
            echo "  --dataset <name>    Test specific dataset"
            echo "  --help              Show this help"
            echo
            echo "Environment variables:"
            echo "  HOST                Valkey host (default: localhost)"
            echo "  NUM_QUERIES         Number of queries (default: 1000)"
            echo "  EF_SEARCH_VALUES    Comma-separated ef_search values (default: 50,100,200,400)"
            echo "  CONCURRENCY         Number of concurrent connections (default: 20)"
            echo
            echo "Examples:"
            echo "  $0 --quick"
            echo "  $0 --dataset bigann-1M"
            echo "  EF_SEARCH_VALUES='100,200,400,800' $0 --comprehensive"
            ;;
        *)
            print_error "Unknown option: $1"
            echo "Run: $0 --help"
            exit 1
            ;;
    esac
}

# Check prerequisites
if [ ! -x "$BENCHM" ]; then
    print_error "valkey-benchmark not found: $BENCHM"
    exit 1
fi

if [ ! -x "$CLI" ]; then
    print_error "valkey-cli not found: $CLI"
    exit 1
fi

# Run main
main "$@"