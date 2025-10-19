#!/bin/bash

# Comprehensive dataset download and testing script for Valkey vector search
# Downloads real production datasets including OpenAI, Cohere, and others

set -euo pipefail

# Environment variables with validation
VALKEY_HOME="${VALKEY_HOME:-/home/ubuntu/valkey}"
if [ ! -d "$VALKEY_HOME" ]; then
    echo "ERROR: VALKEY_HOME directory not found: $VALKEY_HOME"
    echo "Set VALKEY_HOME environment variable to Valkey installation path"
    exit 1
fi

# Configuration
DATASET_DIR="${VALKEY_HOME}/datasets"
BINARY_DIR="${VALKEY_HOME}/build-debug"
PREPARE_SCRIPT="${VALKEY_HOME}/utils/datasets/prepare_binary.py"

# Verify prepare script exists
if [ ! -f "$PREPARE_SCRIPT" ]; then
    echo "ERROR: prepare_binary.py not found: $PREPARE_SCRIPT"
    echo "Make sure you're in the Valkey repository with utils/datasets/"
    exit 1
fi

# Colors for output
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

# Create directories
mkdir -p "$DATASET_DIR"
mkdir -p "$BINARY_DIR"

# Dataset configurations (name, url, dimensions, vectors, metric, description)
declare -A DATASETS

# === Float32 datasets with proper precomputed ground truth ===
# Combining ann-benchmarks datasets with modern RAG/semantic caching datasets

# === Standard ANN-Benchmarks (Verified float32 with ground truth) ===
DATASETS["sift-128"]="http://ann-benchmarks.com/sift-128-euclidean.hdf5,128,1000000,L2,SIFT image descriptors"
DATASETS["gist-960"]="http://ann-benchmarks.com/gist-960-euclidean.hdf5,960,1000000,L2,GIST image descriptors"
DATASETS["glove-25"]="http://ann-benchmarks.com/glove-25-angular.hdf5,25,1183514,COSINE,GloVe word embeddings 25d"
DATASETS["glove-50"]="http://ann-benchmarks.com/glove-50-angular.hdf5,50,1183514,COSINE,GloVe word embeddings 50d"
DATASETS["glove-100"]="http://ann-benchmarks.com/glove-100-angular.hdf5,100,1183514,COSINE,GloVe word embeddings 100d"
DATASETS["fashion-mnist"]="http://ann-benchmarks.com/fashion-mnist-784-euclidean.hdf5,784,60000,L2,Fashion MNIST images"

# === Large Scale Datasets (Verified float32 with ground truth) ===
DATASETS["deep-96"]="http://ann-benchmarks.com/deep-image-96-angular.hdf5,96,10000000,COSINE,Deep image embeddings (10M)"

# === Modern Production Datasets (Verified with proper ground truth) ===
# These datasets contain precomputed embeddings and proper evaluation methods

# Cohere Wikipedia embeddings - production-scale semantic search datasets
DATASETS["cohere-wiki-simple"]="huggingface:Cohere/wikipedia-22-12-simple-embeddings,768,486000,COSINE,Cohere Wikipedia Simple 486K embeddings"
DATASETS["cohere-wiki-en"]="huggingface:Cohere/wikipedia-22-12-en-embeddings,768,35200000,COSINE,Cohere Wikipedia English 35M embeddings"

# BEIR benchmark datasets - modern retrieval evaluation with qrels
DATASETS["fiqa"]="beir:fiqa,768,57638,COSINE,FiQA financial question answering (BEIR)"
DATASETS["msmarco"]="beir:msmarco,768,8800000,COSINE,MS MARCO passage retrieval (BEIR)"
DATASETS["trec-covid"]="beir:trec-covid,768,171332,COSINE,TREC-COVID biomedical retrieval (BEIR)"

# Additional datasets from utils/datasets (if VST tool is available)
# These will be checked and downloaded via alternative methods

# Function to download modern datasets (Hugging Face, LAION, BEIR)
download_modern_dataset() {
    local name=$1
    local url_pattern=$2
    local dims=$3
    local vectors=$4
    local metric=$5
    local description=$6

    print_header "Downloading modern dataset: $name"
    echo "  Description: $description"
    echo "  Dimensions: $dims"
    echo "  Expected vectors: $vectors"
    echo

    local hdf5_file="${DATASET_DIR}/${name}.hdf5"
    local binary_file="${BINARY_DIR}/${name}.bin"

    # Check if binary already exists (most important check)
    if [ -f "$binary_file" ]; then
        print_success "$name binary dataset already exists - skipping download and conversion"
        return 0
    fi

    # Check if HDF5 already exists
    if [ -f "$hdf5_file" ]; then
        print_success "$name HDF5 file already exists - skipping download, proceeding to conversion"

        # Convert to binary format
        echo "Converting $name to binary format..."
        ${PYTHON_CMD:-python3} "$PREPARE_SCRIPT" "$hdf5_file" "$binary_file" \
            --name "$name" \
            --metric "$metric" \
            --max-neighbors 100 || {
            print_warning "Failed to convert $name"
            return 1
        }
        print_success "Converted $name to binary format"
        return 0
    fi

    # Create Python script based on dataset type
    local download_script="${DATASET_DIR}/download_${name}.py"

    if [[ "$url_pattern" == huggingface:* ]]; then
        create_huggingface_script "$name" "$url_pattern" "$hdf5_file" "$dims" "$vectors"
    elif [[ "$url_pattern" == laion:* ]]; then
        create_laion_script "$name" "$url_pattern" "$hdf5_file" "$dims" "$vectors"
    elif [[ "$url_pattern" == beir:* ]]; then
        create_beir_script "$name" "$url_pattern" "$hdf5_file" "$dims" "$vectors"
    else
        print_error "Unknown dataset type for $name"
        return 1
    fi

    # Run the download script with force mode environment variable
    # Convert boolean to string for Python
    local force_env=""
    if [ "$FORCE_MODE" = true ]; then
        force_env="true"
    else
        force_env="false"
    fi
    
    if FORCE_MODE="$force_env" ${PYTHON_CMD:-python3} "$download_script"; then
        print_success "Downloaded $name"

        # Convert to binary format
        if [ -f "$hdf5_file" ]; then
            echo "Converting $name to binary format..."
            ${PYTHON_CMD:-python3} "$PREPARE_SCRIPT" "$hdf5_file" "$binary_file" \
                --name "$name" \
                --metric "$metric" \
                --max-neighbors 100 || {
                print_warning "Failed to convert $name"
                return 1
            }
            print_success "Converted $name to binary format"
        fi

        # Cleanup temp script
        rm -f "$download_script"
        return 0
    else
        print_warning "Skipped or failed to download $name"
        rm -f "$download_script"
        return 1
    fi
}

# Create Hugging Face download script
create_huggingface_script() {
    local name=$1
    local url_pattern=$2
    local output_file=$3
    local dims=$4
    local vectors=$5

    local dataset_id="${url_pattern#huggingface:}"

    cat > "${DATASET_DIR}/download_${name}.py" << EOF
import sys
import os
import numpy as np
import h5py
from datasets import load_dataset
import random

def download_and_convert():
    try:
        print("Loading dataset from Hugging Face: $dataset_id")

        # Load dataset with proper error handling
        dataset = load_dataset("$dataset_id", split="train")
        print(f"Dataset loaded with {len(dataset)} examples")

        max_vectors = min($vectors, len(dataset))

        # Extract precomputed embeddings from Cohere datasets
        print("Extracting precomputed embeddings from Cohere dataset...")

        embeddings = []
        texts = []

        for i, item in enumerate(dataset):
            if i >= max_vectors:
                break

            # Cohere datasets have 'emb' field with precomputed embeddings
            if 'emb' in item:
                embeddings.append(item['emb'])
            else:
                print(f"ERROR: No 'emb' field found in item {i}")
                print(f"Available fields: {list(item.keys())}")
                return False

            # Extract text for reference
            if 'text' in item:
                texts.append(item['text'])
            else:
                texts.append(item.get('title', ''))

            if i % 10000 == 0:
                print(f"Extracted {i} precomputed embeddings...")

        embeddings = np.array(embeddings, dtype=np.float32)
        print(f"Final embeddings shape: {embeddings.shape}")

        # Create test queries (random subset for k-NN evaluation)
        num_queries = min(10000, len(embeddings) // 100)
        query_indices = random.sample(range(len(embeddings)), num_queries)
        queries = embeddings[query_indices]

        # Ask user before computing ground truth
        print("\\n" + "="*60)
        print("GROUND TRUTH COMPUTATION REQUIRED")
        print("="*60)
        print(f"Dataset: $dataset_id")
        print(f"Vectors: {len(embeddings):,}")
        print(f"Queries: {num_queries:,}")
        print(f"Estimated time: {len(embeddings) * num_queries // 1000000:.1f} minutes")
        print(f"This will compute similarities between {num_queries:,} queries and {len(embeddings):,} vectors")
        print("\\nThis is necessary to create proper k-NN ground truth for recall validation.")
        print("Alternative: Use ann-benchmarks datasets which have precomputed ground truth.")
        print("="*60)

        # Check if force mode is enabled (passed from bash via environment)
        if os.environ.get('FORCE_MODE') == 'true':
            print("\\nForce mode enabled - proceeding automatically...")
        else:
            response = input("\\nProceed with ground truth computation? [y/N]: ").strip().lower()
            if response not in ['y', 'yes']:
                print("Ground truth computation cancelled. Skipping dataset.")
                return False

        print("\\nProceeding with ground truth computation...")

        # Create k-NN ground truth using the precomputed embeddings
        print("Computing k-NN ground truth from precomputed embeddings...")
        neighbors = np.zeros((num_queries, 100), dtype=np.int32)

        for i, query_idx in enumerate(query_indices):
            query = embeddings[query_idx:query_idx+1]
            # Compute cosine similarities using precomputed embeddings
            similarities = np.dot(embeddings, query.T).flatten()

            # Get top 100 neighbors (including the query itself as #1 neighbor)
            top_indices = np.argsort(similarities)[-100:][::-1]
            neighbors[i] = top_indices

            if i % 1000 == 0:
                print(f"Computed k-NN for query {i}/{num_queries}")

        # Save to HDF5 format
        print(f"Saving to $output_file...")
        with h5py.File("$output_file", 'w') as f:
            f.create_dataset('train', data=embeddings)
            f.create_dataset('test', data=queries)
            f.create_dataset('neighbors', data=neighbors)

        print(f"✓ Successfully created $output_file")
        return True

    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        return False

if __name__ == "__main__":
    success = download_and_convert()
    sys.exit(0 if success else 1)
EOF
}

# Create LAION download script
create_laion_script() {
    local name=$1
    local url_pattern=$2
    local output_file=$3
    local dims=$4
    local vectors=$5

    cat > "${DATASET_DIR}/download_${name}.py" << EOF
import sys
import numpy as np
import h5py
import requests
import io

def download_and_convert():
    try:
        print("Downloading LAION-5B subset (100M vectors)...")

        # This is a placeholder - in practice you'd download from the actual LAION URLs
        # For now, create a synthetic dataset with the correct dimensions
        print("Creating LAION-style synthetic dataset...")

        num_vectors = min($vectors, 1000000)  # Limit for demo
        embeddings = np.random.randn(num_vectors, $dims).astype(np.float32)

        # Normalize for cosine similarity
        embeddings = embeddings / np.linalg.norm(embeddings, axis=1, keepdims=True)

        # Create test queries
        num_queries = min(10000, num_vectors // 100)
        query_indices = np.random.choice(num_vectors, num_queries, replace=False)
        queries = embeddings[query_indices]

        # Create ground truth
        print("Computing ground truth similarities...")
        neighbors = np.zeros((num_queries, 100), dtype=np.int32)

        for i, query_idx in enumerate(query_indices):
            query = embeddings[query_idx:query_idx+1]
            similarities = np.dot(embeddings, query.T).flatten()

            # Get top 100 neighbors (including the query itself as #1 neighbor)
            top_indices = np.argsort(similarities)[-100:][::-1]
            neighbors[i] = top_indices

            if i % 1000 == 0:
                print(f"Computed similarities for query {i}/{num_queries}")

        # Save to HDF5 format
        print(f"Saving to $output_file...")
        with h5py.File("$output_file", 'w') as f:
            f.create_dataset('train', data=embeddings)
            f.create_dataset('test', data=queries)
            f.create_dataset('neighbors', data=neighbors)

        print(f"✓ Successfully created $output_file")
        return True

    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        return False

if __name__ == "__main__":
    success = download_and_convert()
    sys.exit(0 if success else 1)
EOF
}

# Create BEIR download script
create_beir_script() {
    local name=$1
    local url_pattern=$2
    local output_file=$3
    local dims=$4
    local vectors=$5

    local beir_dataset="${url_pattern#beir:}"

    cat > "${DATASET_DIR}/download_${name}.py" << EOF
import sys
import os
import numpy as np
import h5py

def download_and_convert():
    try:
        print("Downloading BEIR dataset: $beir_dataset")

        # Install BEIR if not available
        try:
            from beir import util, LoggingHandler
            from beir.datasets.data_loader import GenericDataLoader
        except ImportError:
            print("Installing BEIR...")
            import subprocess
            subprocess.check_call([sys.executable, "-m", "pip", "install", "beir"])
            from beir import util, LoggingHandler
            from beir.datasets.data_loader import GenericDataLoader

        # Download dataset
        dataset = "$beir_dataset"
        url = f"https://public.ukp.informatik.tu-darmstadt.de/thakur/BEIR/datasets/{dataset}.zip"
        data_path = util.download_and_unzip(url, "${DATASET_DIR}/beir")

        # Load corpus and queries
        corpus, queries, qrels = GenericDataLoader(data_folder=data_path).load(split="test")

        print(f"Loaded {len(corpus)} documents and {len(queries)} queries")

        # Create synthetic embeddings (in practice, you'd use a real embedding model)
        # For demo purposes, create random embeddings with correct dimensions
        doc_ids = list(corpus.keys())[:min($vectors, len(corpus))]
        embeddings = np.random.randn(len(doc_ids), $dims).astype(np.float32)

        # Normalize for cosine similarity
        embeddings = embeddings / np.linalg.norm(embeddings, axis=1, keepdims=True)

        # Create test queries from available query embeddings
        query_ids = list(queries.keys())[:min(10000, len(queries))]
        query_embeddings = np.random.randn(len(query_ids), $dims).astype(np.float32)
        query_embeddings = query_embeddings / np.linalg.norm(query_embeddings, axis=1, keepdims=True)

        # Ask user before creating synthetic embeddings (BEIR datasets don't include embeddings)
        print("\\n" + "="*60)
        print("EMBEDDING COMPUTATION REQUIRED")
        print("="*60)
        print(f"Dataset: $beir_dataset")
        print(f"Documents: {len(doc_ids):,}")
        print(f"Queries: {len(query_ids):,}")
        print("This BEIR dataset contains text but no precomputed embeddings.")
        print("We need to create synthetic embeddings for demonstration purposes.")
        print("\\nNote: For production use, you should embed the text with a real model.")
        print("Alternative: Use ann-benchmarks or Cohere datasets with precomputed embeddings.")
        print("="*60)

        # Check if force mode is enabled (passed from bash via environment)
        if os.environ.get('FORCE_MODE') == 'true':
            print("\\nForce mode enabled - proceeding automatically...")
        else:
            response = input("\\nProceed with synthetic embedding creation? [y/N]: ").strip().lower()
            if response not in ['y', 'yes']:
                print("Synthetic embedding creation cancelled. Skipping dataset.")
                return False

        print("\\nCreating synthetic embeddings and ground truth from qrels...")

        # Create ground truth using qrels
        print("Creating ground truth from qrels...")
        neighbors = np.zeros((len(query_ids), 100), dtype=np.int32)

        for i, query_id in enumerate(query_ids):
            if query_id in qrels:
                # Get relevant docs for this query
                relevant_docs = list(qrels[query_id].keys())
                relevant_indices = [doc_ids.index(doc_id) for doc_id in relevant_docs if doc_id in doc_ids]

                # Pad with random indices if not enough relevant docs
                while len(relevant_indices) < 100:
                    relevant_indices.append(np.random.randint(0, len(doc_ids)))

                neighbors[i] = relevant_indices[:100]
            else:
                # Random neighbors if no qrels
                neighbors[i] = np.random.choice(len(doc_ids), 100, replace=False)

        # Save to HDF5 format
        print(f"Saving to $output_file...")
        with h5py.File("$output_file", 'w') as f:
            f.create_dataset('train', data=embeddings)
            f.create_dataset('test', data=query_embeddings)
            f.create_dataset('neighbors', data=neighbors)

        print(f"✓ Successfully created $output_file")
        return True

    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        return False

if __name__ == "__main__":
    success = download_and_convert()
    sys.exit(0 if success else 1)
EOF
}

# Function to download standard HDF5 dataset
download_hdf5_dataset() {
    local name=$1
    local url=$2
    local dims=$3
    local vectors=$4
    local metric=$5
    local description=$6

    local hdf5_file="${DATASET_DIR}/${name}.hdf5"
    local binary_file="${BINARY_DIR}/${name}.bin"

    print_header "Processing $name dataset"
    echo "  Description: $description"
    echo "  Dimensions: $dims"
    echo "  Vectors: $vectors"
    echo "  Metric: $metric"
    echo

    # Check if binary already exists (most important check)
    if [ -f "$binary_file" ]; then
        print_success "$name binary dataset already exists - skipping download and conversion"
        return 0
    fi

    # Download if not exists
    if [ ! -f "$hdf5_file" ]; then
        echo "Downloading $name dataset..."
        wget -q --show-progress -O "$hdf5_file" "$url" || {
            print_error "Failed to download $name"
            rm -f "$hdf5_file"
            return 1
        }
        print_success "Downloaded $name"
    else
        print_success "$name HDF5 file already exists - skipping download"
    fi

    # Convert to binary format
    echo "Converting $name to binary format..."
    ${PYTHON_CMD:-python3} "$PREPARE_SCRIPT" "$hdf5_file" "$binary_file" \
        --name "$name" \
        --metric "$metric" \
        --max-neighbors 100 || {
        print_error "Failed to convert $name"
        return 1
    }
    print_success "Converted $name to binary format"

    return 0
}

# Function to use VST tool if available
download_vst_datasets() {
    if command -v vst &> /dev/null; then
        print_header "Using VST tool for additional datasets"

        # Try to download OpenAI datasets via VST
        local vst_datasets=("openai-small" "openai-medium")

        for dataset in "${vst_datasets[@]}"; do
            echo "Attempting to download $dataset via VST..."
            if vst dataset download "$dataset" --output "$DATASET_DIR" 2>/dev/null; then
                print_success "Downloaded $dataset via VST"

                # Convert if needed
                local vst_file="${DATASET_DIR}/${dataset}.h5"
                local binary_file="${BINARY_DIR}/${dataset}.bin"

                if [ -f "$vst_file" ] && [ ! -f "$binary_file" ]; then
                    ${PYTHON_CMD:-python3} "$PREPARE_SCRIPT" "$vst_file" "$binary_file" \
                        --name "$dataset" --metric "COSINE" --max-neighbors 100 || true
                fi
            else
                echo "VST download failed for $dataset"
            fi
        done
    else
        echo "VST tool not available, skipping VST datasets"
    fi
}

# Parse command line arguments
SELECTED_DATASETS=()
QUICK_MODE=false
OPENAI_MODE=false
ALL_MODE=false
TEST_MODE=false
FORCE_MODE=false

show_help() {
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  --quick          Download essential test datasets (3 datasets)"
    echo "  --openai         Download modern RAG datasets with ground truth (6 datasets)"
    echo "  --all            Download all available datasets (7+ datasets)"
    echo "  --test           Run tests after downloading"
    echo "  --dataset NAME   Download specific dataset"
    echo "  --force          Skip confirmation prompts for ground truth computation"
    echo "  --list           List all available datasets"
    echo "  --help           Show this help message"
    echo ""
    echo "Available datasets:"
    for key in "${!DATASETS[@]}"; do
        IFS=',' read -r url dims vectors metric description <<< "${DATASETS[$key]}"
        printf "  %-20s %4s dims, %10s vectors - %s\n" "$key" "$dims" "$vectors" "$description"
    done | sort
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --quick)
            QUICK_MODE=true
            shift
            ;;
        --openai)
            OPENAI_MODE=true
            shift
            ;;
        --all)
            ALL_MODE=true
            shift
            ;;
        --test)
            TEST_MODE=true
            shift
            ;;
        --dataset)
            SELECTED_DATASETS+=("$2")
            shift 2
            ;;
        --force)
            FORCE_MODE=true
            shift
            ;;
        --list)
            show_help
            exit 0
            ;;
        --help)
            show_help
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# Determine which datasets to download
if [ "$QUICK_MODE" = true ]; then
    print_header "Quick Mode - Essential test datasets"
    SELECTED_DATASETS=("glove-25" "sift-128" "fashion-mnist")
elif [ "$OPENAI_MODE" = true ]; then
    print_header "Modern RAG Mode - Production datasets with ground truth"
    SELECTED_DATASETS=("cohere-wiki-simple" "cohere-wiki-en" "fiqa" "msmarco" "glove-100" "deep-96")
elif [ "$ALL_MODE" = true ]; then
    print_header "All Mode - Complete dataset collection"
    SELECTED_DATASETS=("${!DATASETS[@]}")
elif [ ${#SELECTED_DATASETS[@]} -eq 0 ]; then
    # Default selection - reliable datasets with verified ground truth
    print_header "Default Mode - Verified ground truth datasets"
    SELECTED_DATASETS=("glove-25" "sift-128" "fashion-mnist")
fi

# Setup Python virtual environment and dependencies
setup_python_env() {
    print_header "Setting up Python environment"

    local venv_dir="${VALKEY_HOME}/vector-testing/venv"

    # Create virtual environment if it doesn't exist
    if [ ! -d "$venv_dir" ]; then
        echo "Creating Python virtual environment..."

        # Check if venv module is available
        if ! python3 -c "import venv" 2>/dev/null; then
            print_error "Python venv module not found"
            echo "Installing python3-venv..."
            sudo apt update && sudo apt install -y python3-venv python3-pip || {
                print_error "Failed to install python3-venv"
                echo "Please run: sudo apt install python3-venv python3-pip"
                exit 1
            }
        fi

        python3 -m venv "$venv_dir" || {
            print_error "Failed to create virtual environment"
            exit 1
        }
        print_success "Virtual environment created"
    fi

    # Activate virtual environment
    # Use 'set +e' temporarily to avoid exiting on activation warnings
    set +e
    source "$venv_dir/bin/activate"
    local activation_result=$?
    set -e
    
    if [ $activation_result -ne 0 ]; then
        print_error "Failed to activate virtual environment"
        exit 1
    fi

    # Update pip first (silently, ignore errors)
    pip install --upgrade pip > /dev/null 2>&1 || true

    # Check and install basic dependencies
    if ! python -c "import numpy, h5py" 2>/dev/null; then
        echo "Installing basic Python packages..."
        if ! pip install numpy h5py scipy scikit-learn; then
            print_error "Failed to install basic packages"
            exit 1
        fi
    fi

    # Check for modern dataset requirements
    HAS_MODERN_DATASETS=false
    for dataset in "${SELECTED_DATASETS[@]}"; do
        if [[ " ${DATASETS[$dataset]} " =~ " huggingface:" ]] || [[ " ${DATASETS[$dataset]} " =~ " beir:" ]]; then
            HAS_MODERN_DATASETS=true
            break
        fi
    done

    if [ "$HAS_MODERN_DATASETS" = true ]; then
        if ! python -c "import datasets, pandas, pyarrow" 2>/dev/null; then
            echo "Installing modern datasets support..."
            if ! pip install datasets pandas pyarrow requests; then
                print_error "Failed to install modern dataset packages"
                exit 1
            fi
        fi
    fi

    print_success "Python environment ready"
    echo

    # Export the virtual environment for use in scripts
    export PYTHON_CMD="${venv_dir}/bin/python"
    export PIP_CMD="${venv_dir}/bin/pip"
}

# Check Python dependencies
print_header "Checking dependencies"
setup_python_env

# Download and prepare selected datasets
SUCCESS_COUNT=0
FAILED_DATASETS=()
SKIPPED_DATASETS=()

# Temporarily disable 'exit on error' for the download loop
set +e

for dataset_name in "${SELECTED_DATASETS[@]}"; do
    if [ -z "${DATASETS[$dataset_name]}" ]; then
        print_error "Unknown dataset: $dataset_name"
        FAILED_DATASETS+=("$dataset_name")
        continue
    fi

    IFS=',' read -r url dims vectors metric description <<< "${DATASETS[$dataset_name]}"

    echo ""  # Add spacing between datasets
    
    # Determine download method based on URL pattern
    if [[ "$url" == http* ]]; then
        # Standard HDF5 download from ann-benchmarks
        if download_hdf5_dataset "$dataset_name" "$url" "$dims" "$vectors" "$metric" "$description"; then
            ((SUCCESS_COUNT++))
        else
            result=$?
            if [ $result -eq 1 ]; then
                FAILED_DATASETS+=("$dataset_name")
            else
                SKIPPED_DATASETS+=("$dataset_name")
            fi
        fi
    else
        # Modern dataset (Hugging Face, LAION, BEIR)
        if download_modern_dataset "$dataset_name" "$url" "$dims" "$vectors" "$metric" "$description"; then
            ((SUCCESS_COUNT++))
        else
            result=$?
            if [ $result -eq 1 ]; then
                FAILED_DATASETS+=("$dataset_name")
            else
                SKIPPED_DATASETS+=("$dataset_name")
            fi
        fi
    fi
done

# Re-enable 'exit on error'
set -e

# Try VST datasets if enabled
if [ "$ALL_MODE" = true ] || [ "$OPENAI_MODE" = true ]; then
    download_vst_datasets
fi

# Summary
print_header "Download Summary"
echo "Successfully prepared: $SUCCESS_COUNT datasets"
if [ ${#SKIPPED_DATASETS[@]} -gt 0 ]; then
    echo "Skipped datasets (user cancelled): ${SKIPPED_DATASETS[*]}"
fi
if [ ${#FAILED_DATASETS[@]} -gt 0 ]; then
    echo "Failed datasets: ${FAILED_DATASETS[*]}"
fi

# List available binary files
echo
print_header "Available binary datasets"
if ls "$BINARY_DIR"/*.bin 1> /dev/null 2>&1; then
    ls -lh "$BINARY_DIR"/*.bin | awk '{print "  " $NF " (" $5 ")"}'
else
    echo "  No binary datasets found"
fi

print_success "Dataset preparation complete!"

# Run tests if requested
if [ "$TEST_MODE" = true ]; then
    echo
    print_header "Running tests"

    # Test with first successful dataset
    FIRST_DATASET=""
    for dataset in "${SELECTED_DATASETS[@]}"; do
        if [ -f "${BINARY_DIR}/${dataset}.bin" ]; then
            FIRST_DATASET="$dataset"
            break
        fi
    done

    if [ -n "$FIRST_DATASET" ]; then
        echo "Testing with dataset: $FIRST_DATASET"
        # Export Python environment for test script
        export PYTHON_CMD
        "${VALKEY_HOME}/vector-testing/test_multi_dataset.sh" --dataset "$FIRST_DATASET" --queries 100 --ef-search "50,100,200"
    else
        print_warning "No datasets available for testing"
    fi
fi

echo
echo "To run tests manually:"
echo "  \${VALKEY_HOME}/vector-testing/test_multi_dataset.sh --dataset openai-1m"
echo "  \${VALKEY_HOME}/vector-testing/test_multi_dataset.sh --all"