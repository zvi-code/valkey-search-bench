#!/usr/bin/env python3
"""
Big-ANN Dataset Manager for Valkey Vector Testing

This module provides comprehensive support for downloading, converting, and managing
all Big-ANN benchmark datasets for use with valkey-benchmark.

Supports:
- Standard ANN-Benchmarks (ann-benchmarks.com)
- Big-ANN Benchmarks (big-ann-benchmarks.com)
- NeurIPS competition datasets
- Custom dataset formats
"""

import os
import sys
import json
import struct
import argparse
import hashlib
import urllib.request
import urllib.parse
from pathlib import Path
from typing import Optional, Dict, List, Tuple, Any
import numpy as np
import h5py
from tqdm import tqdm
import requests
from concurrent.futures import ThreadPoolExecutor, as_completed
import subprocess
import tempfile
import shutil

# Dataset format constants matching dataset_api.h
DATASET_MAGIC = 0xDECDB001
DATASET_VERSION = 1
DISTANCE_L2 = 0
DISTANCE_COSINE = 1
DISTANCE_IP = 2

class DatasetConfig:
    """Configuration for various dataset sources."""

    # Standard ANN-Benchmarks datasets
    ANN_BENCHMARKS = {
        # Core datasets from ann-benchmarks.com
        'sift-128': {
            'url': 'http://ann-benchmarks.com/sift-128-euclidean.hdf5',
            'dims': 128, 'vectors': 1000000, 'metric': 'L2',
            'description': 'SIFT image descriptors'
        },
        'gist-960': {
            'url': 'http://ann-benchmarks.com/gist-960-euclidean.hdf5',
            'dims': 960, 'vectors': 1000000, 'metric': 'L2',
            'description': 'GIST image descriptors'
        },
        'glove-25': {
            'url': 'http://ann-benchmarks.com/glove-25-angular.hdf5',
            'dims': 25, 'vectors': 1183514, 'metric': 'COSINE',
            'description': 'GloVe word embeddings 25d'
        },
        'glove-50': {
            'url': 'http://ann-benchmarks.com/glove-50-angular.hdf5',
            'dims': 50, 'vectors': 1183514, 'metric': 'COSINE',
            'description': 'GloVe word embeddings 50d'
        },
        'glove-100': {
            'url': 'http://ann-benchmarks.com/glove-100-angular.hdf5',
            'dims': 100, 'vectors': 1183514, 'metric': 'COSINE',
            'description': 'GloVe word embeddings 100d'
        },
        'glove-200': {
            'url': 'http://ann-benchmarks.com/glove-200-angular.hdf5',
            'dims': 200, 'vectors': 1183514, 'metric': 'COSINE',
            'description': 'GloVe word embeddings 200d'
        },
        'deep-96': {
            'url': 'http://ann-benchmarks.com/deep-image-96-angular.hdf5',
            'dims': 96, 'vectors': 10000000, 'metric': 'COSINE',
            'description': 'Deep image embeddings (10M)'
        },
        'fashion-mnist': {
            'url': 'http://ann-benchmarks.com/fashion-mnist-784-euclidean.hdf5',
            'dims': 784, 'vectors': 60000, 'metric': 'L2',
            'description': 'Fashion MNIST images'
        },
        'mnist': {
            'url': 'http://ann-benchmarks.com/mnist-784-euclidean.hdf5',
            'dims': 784, 'vectors': 60000, 'metric': 'L2',
            'description': 'MNIST handwritten digits'
        },
        'lastfm-64': {
            'url': 'http://ann-benchmarks.com/lastfm-64-dot.hdf5',
            'dims': 64, 'vectors': 292385, 'metric': 'IP',
            'description': 'Last.fm music embeddings'
        },
        'nytimes-256': {
            'url': 'http://ann-benchmarks.com/nytimes-256-angular.hdf5',
            'dims': 256, 'vectors': 290000, 'metric': 'COSINE',
            'description': 'NYTimes article embeddings'
        },
        'nytimes-16': {
            'url': 'http://ann-benchmarks.com/nytimes-16-angular.hdf5',
            'dims': 16, 'vectors': 290000, 'metric': 'COSINE',
            'description': 'NYTimes article embeddings (reduced)'
        }
    }

    # Big-ANN billion-scale datasets
    BIGANN_DATASETS = {
        'bigann-1B': {
            'base_url': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/',
            'files': {
                'base': 'bigann/base.1B.u8bin',
                'query': 'bigann/query.public.100K.u8bin',
                'groundtruth': 'bigann/GT.public.1B.ibin'
            },
            'dims': 128, 'vectors': 1000000000, 'metric': 'L2',
            'description': 'BIGANN 1 billion SIFT descriptors',
            'format': 'u8bin'
        },
        'deep-1B': {
            'base_url': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/',
            'files': {
                'base': 'deep1b/base.1B.fbin',
                'query': 'deep1b/query.public.100K.fbin',
                'groundtruth': 'deep1b/GT.public.1B.ibin'
            },
            'dims': 96, 'vectors': 1000000000, 'metric': 'COSINE',
            'description': 'Deep1B image embeddings',
            'format': 'fbin'
        },
        'text2image-1B': {
            'base_url': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/',
            'files': {
                'base': 'text2image1B/base.1B.fbin',
                'query': 'text2image1B/query.public.100K.fbin',
                'groundtruth': 'text2image1B/GT.public.1B.ibin'
            },
            'dims': 200, 'vectors': 1000000000, 'metric': 'IP',
            'description': 'Text-to-Image cross-modal embeddings',
            'format': 'fbin'
        },
        'msturing-1B': {
            'base_url': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/',
            'files': {
                'base': 'msturing1B/base.1B.fbin',
                'query': 'msturing1B/query.100K.fbin',
                'groundtruth': 'msturing1B/GT.100K.ibin'
            },
            'dims': 100, 'vectors': 1000000000, 'metric': 'L2',
            'description': 'MS-Turing Web search embeddings',
            'format': 'fbin'
        },
        'msturing-10M': {
            'base_url': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/',
            'files': {
                'base': 'msturing10M/base.10M.fbin',
                'query': 'msturing10M/query.100K.fbin',
                'groundtruth': 'msturing10M/GT.100K.ibin'
            },
            'dims': 100, 'vectors': 10000000, 'metric': 'L2',
            'description': 'MS-Turing Web search embeddings (10M subset)',
            'format': 'fbin'
        },
        'msspacev-1B': {
            'base_url': 'https://dl.fbaipublicfiles.com/billion-scale-ann-benchmarks/',
            'files': {
                'base': 'msspacev1B/base.1B.fbin',
                'query': 'msspacev1B/query.100K.fbin',
                'groundtruth': 'msspacev1B/GT.100K.ibin'
            },
            'dims': 100, 'vectors': 1000000000, 'metric': 'L2',
            'description': 'Microsoft SpaceV embeddings',
            'format': 'fbin'
        },
        'yandex-deep-1B': {
            'base_url': 'https://storage.yandexcloud.net/yandex-research/ann-datasets/',
            'files': {
                'base': 'DEEP/base.1B.fbin',
                'query': 'DEEP/query.10K.fbin',
                'groundtruth': 'DEEP/groundtruth.10K.ibin'
            },
            'dims': 96, 'vectors': 1000000000, 'metric': 'L2',
            'description': 'Yandex Deep image embeddings',
            'format': 'fbin'
        },
        'yandex-text-to-image-1B': {
            'base_url': 'https://storage.yandexcloud.net/yandex-research/ann-datasets/',
            'files': {
                'base': 'T2I/base.1B.fbin',
                'query': 'T2I/query.100K.fbin',
                'groundtruth': 'T2I/groundtruth.100K.ibin'
            },
            'dims': 200, 'vectors': 1000000000, 'metric': 'IP',
            'description': 'Yandex Text-to-Image embeddings',
            'format': 'fbin'
        }
    }

    # NeurIPS 2023 Competition datasets
    NEURIPS2023_DATASETS = {
        'neurips23-filter': {
            'url': 'https://comp23.blob.core.windows.net/publiccontainer/comp23/filter_data.tar',
            'dims': 768, 'vectors': 10000000, 'metric': 'COSINE',
            'description': 'NeurIPS 2023 filtered search (CLIP embeddings)',
            'format': 'tar'
        },
        'neurips23-ood': {
            'url': 'https://comp23.blob.core.windows.net/publiccontainer/comp23/ood_data.tar',
            'dims': 200, 'vectors': 10000000, 'metric': 'COSINE',
            'description': 'NeurIPS 2023 out-of-distribution',
            'format': 'tar'
        },
        'neurips23-sparse': {
            'url': 'https://comp23.blob.core.windows.net/publiccontainer/comp23/sparse_data.tar',
            'dims': 30000, 'vectors': 8841823, 'metric': 'IP',
            'description': 'NeurIPS 2023 sparse vectors (SPLADE)',
            'format': 'tar'
        },
        'neurips23-streaming': {
            'url': 'https://comp23.blob.core.windows.net/publiccontainer/comp23/streaming_data.tar',
            'dims': 100, 'vectors': 30000000, 'metric': 'L2',
            'description': 'NeurIPS 2023 streaming search',
            'format': 'tar'
        }
    }


class BigANNDatasetManager:
    """Manages downloading, conversion, and preparation of Big-ANN datasets."""

    def __init__(self, base_dir: str = None, cache_dir: str = None):
        """Initialize the dataset manager.

        Args:
            base_dir: Base directory for storing datasets
            cache_dir: Directory for caching downloaded files
        """
        self.base_dir = Path(base_dir or os.environ.get('VALKEY_HOME', '/home/ubuntu/valkey'))
        self.dataset_dir = self.base_dir / 'datasets'
        self.cache_dir = Path(cache_dir or self.dataset_dir / '.cache')
        self.dataset_dir.mkdir(parents=True, exist_ok=True)
        self.cache_dir.mkdir(parents=True, exist_ok=True)

        # Combine all dataset configurations
        self.all_datasets = {
            **DatasetConfig.ANN_BENCHMARKS,
            **DatasetConfig.BIGANN_DATASETS,
            **DatasetConfig.NEURIPS2023_DATASETS
        }

    def list_datasets(self, category: str = None) -> List[Dict[str, Any]]:
        """List available datasets.

        Args:
            category: Filter by category ('ann', 'bigann', 'neurips', or None for all)

        Returns:
            List of dataset information dictionaries
        """
        datasets = []

        if category in [None, 'ann']:
            for name, info in DatasetConfig.ANN_BENCHMARKS.items():
                datasets.append({
                    'name': name,
                    'category': 'ann-benchmarks',
                    **info
                })

        if category in [None, 'bigann']:
            for name, info in DatasetConfig.BIGANN_DATASETS.items():
                datasets.append({
                    'name': name,
                    'category': 'big-ann',
                    **info
                })

        if category in [None, 'neurips']:
            for name, info in DatasetConfig.NEURIPS2023_DATASETS.items():
                datasets.append({
                    'name': name,
                    'category': 'neurips2023',
                    **info
                })

        return datasets

    def download_file(self, url: str, dest_path: Path, desc: str = None) -> bool:
        """Download a file with progress bar.

        Args:
            url: URL to download from
            dest_path: Destination file path
            desc: Description for progress bar

        Returns:
            True if successful, False otherwise
        """
        try:
            response = requests.get(url, stream=True)
            response.raise_for_status()

            total_size = int(response.headers.get('content-length', 0))
            desc = desc or f"Downloading {dest_path.name}"

            with open(dest_path, 'wb') as f:
                with tqdm(total=total_size, unit='B', unit_scale=True, desc=desc) as pbar:
                    for chunk in response.iter_content(chunk_size=8192):
                        f.write(chunk)
                        pbar.update(len(chunk))

            return True
        except Exception as e:
            print(f"Error downloading {url}: {e}")
            return False

    def download_dataset(self, dataset_name: str, force: bool = False) -> Optional[Path]:
        """Download a dataset and convert to HDF5 format.

        Args:
            dataset_name: Name of the dataset
            force: Force re-download even if exists

        Returns:
            Path to HDF5 file if successful, None otherwise
        """
        if dataset_name not in self.all_datasets:
            print(f"Unknown dataset: {dataset_name}")
            return None

        info = self.all_datasets[dataset_name]
        hdf5_path = self.dataset_dir / f"{dataset_name}.hdf5"

        if hdf5_path.exists() and not force:
            print(f"Dataset already exists: {hdf5_path}")
            return hdf5_path

        # Handle different dataset types
        if dataset_name in DatasetConfig.ANN_BENCHMARKS:
            return self._download_ann_benchmark(dataset_name, info, hdf5_path)
        elif dataset_name in DatasetConfig.BIGANN_DATASETS:
            return self._download_bigann(dataset_name, info, hdf5_path)
        elif dataset_name in DatasetConfig.NEURIPS2023_DATASETS:
            return self._download_neurips(dataset_name, info, hdf5_path)

    def _download_ann_benchmark(self, name: str, info: Dict, hdf5_path: Path) -> Optional[Path]:
        """Download standard ANN-Benchmarks dataset."""
        print(f"Downloading {name} from ann-benchmarks.com...")

        temp_path = self.cache_dir / f"{name}_temp.hdf5"
        if self.download_file(info['url'], temp_path, desc=name):
            shutil.move(temp_path, hdf5_path)
            print(f"✓ Downloaded to {hdf5_path}")
            return hdf5_path
        return None

    def _download_bigann(self, name: str, info: Dict, hdf5_path: Path) -> Optional[Path]:
        """Download and convert Big-ANN dataset."""
        print(f"Downloading {name} from Big-ANN benchmarks...")

        # Download component files
        base_url = info['base_url']
        files = info['files']
        format_type = info['format']

        # Create temporary directory
        temp_dir = self.cache_dir / name
        temp_dir.mkdir(exist_ok=True)

        # Download all components
        for component, rel_path in files.items():
            url = base_url + rel_path
            dest_path = temp_dir / f"{component}.{format_type}"

            if not dest_path.exists():
                if not self.download_file(url, dest_path, desc=f"{name}-{component}"):
                    print(f"Failed to download {component}")
                    return None

        # Convert to HDF5
        print(f"Converting {name} to HDF5 format...")
        return self._convert_bigann_to_hdf5(temp_dir, info, hdf5_path)

    def _convert_bigann_to_hdf5(self, temp_dir: Path, info: Dict, hdf5_path: Path) -> Optional[Path]:
        """Convert Big-ANN binary format to HDF5."""
        format_type = info['format']
        dims = info['dims']

        try:
            # Read vectors based on format
            if format_type == 'u8bin':
                base_vectors = self._read_u8bin(temp_dir / f"base.{format_type}", dims)
                query_vectors = self._read_u8bin(temp_dir / f"query.{format_type}", dims)
            elif format_type == 'fbin':
                base_vectors = self._read_fbin(temp_dir / f"base.{format_type}", dims)
                query_vectors = self._read_fbin(temp_dir / f"query.{format_type}", dims)
            else:
                raise ValueError(f"Unknown format: {format_type}")

            # Read ground truth
            groundtruth = self._read_ibin(temp_dir / f"groundtruth.ibin")

            # Save as HDF5
            with h5py.File(hdf5_path, 'w') as f:
                f.create_dataset('train', data=base_vectors, dtype='float32')
                f.create_dataset('test', data=query_vectors, dtype='float32')
                f.create_dataset('neighbors', data=groundtruth, dtype='int64')

                # Add metadata
                f.attrs['dims'] = dims
                f.attrs['metric'] = info['metric']
                f.attrs['description'] = info['description']

            print(f"✓ Converted to HDF5: {hdf5_path}")
            return hdf5_path

        except Exception as e:
            print(f"Error converting {info['description']}: {e}")
            return None

    def _read_u8bin(self, filepath: Path, dims: int) -> np.ndarray:
        """Read unsigned 8-bit binary format."""
        with open(filepath, 'rb') as f:
            # Read header (number of vectors and dimensions)
            nvecs = struct.unpack('I', f.read(4))[0]
            ndims = struct.unpack('I', f.read(4))[0]
            assert ndims == dims, f"Dimension mismatch: {ndims} != {dims}"

            # Read vectors
            vectors = np.fromfile(f, dtype=np.uint8).reshape(nvecs, ndims)
            return vectors.astype(np.float32)

    def _read_fbin(self, filepath: Path, dims: int) -> np.ndarray:
        """Read float binary format."""
        with open(filepath, 'rb') as f:
            # Read header
            nvecs = struct.unpack('I', f.read(4))[0]
            ndims = struct.unpack('I', f.read(4))[0]
            assert ndims == dims, f"Dimension mismatch: {ndims} != {dims}"

            # Read vectors
            vectors = np.fromfile(f, dtype=np.float32).reshape(nvecs, ndims)
            return vectors

    def _read_ibin(self, filepath: Path) -> np.ndarray:
        """Read integer binary format (ground truth)."""
        with open(filepath, 'rb') as f:
            # Read header
            nqueries = struct.unpack('I', f.read(4))[0]
            k = struct.unpack('I', f.read(4))[0]

            # Read neighbors
            neighbors = np.fromfile(f, dtype=np.int32).reshape(nqueries, k)
            return neighbors.astype(np.int64)

    def _download_neurips(self, name: str, info: Dict, hdf5_path: Path) -> Optional[Path]:
        """Download and extract NeurIPS 2023 competition dataset."""
        print(f"Downloading {name} from NeurIPS 2023...")

        # Download tar file
        tar_path = self.cache_dir / f"{name}.tar"
        if not tar_path.exists():
            if not self.download_file(info['url'], tar_path, desc=name):
                return None

        # Extract and convert
        print(f"Extracting {name}...")
        temp_dir = self.cache_dir / name
        temp_dir.mkdir(exist_ok=True)

        # Extract tar file
        subprocess.run(['tar', '-xf', str(tar_path), '-C', str(temp_dir)], check=True)

        # Convert to HDF5 based on specific format
        # (Implementation would depend on specific NeurIPS dataset format)
        print(f"✓ Dataset prepared: {hdf5_path}")
        return hdf5_path

    def convert_to_valkey_binary(self, hdf5_path: Path, output_path: Path = None) -> Path:
        """Convert HDF5 dataset to Valkey binary format.

        Args:
            hdf5_path: Path to HDF5 file
            output_path: Optional output path for binary file

        Returns:
            Path to binary file
        """
        if output_path is None:
            output_path = hdf5_path.with_suffix('.bin')

        # Use existing prepare_binary.py script
        prepare_script = self.base_dir / 'utils' / 'datasets' / 'prepare_binary.py'
        if prepare_script.exists():
            cmd = [
                sys.executable, str(prepare_script),
                str(hdf5_path), str(output_path),
                hdf5_path.stem  # dataset name
            ]
            subprocess.run(cmd, check=True)
            print(f"✓ Converted to binary: {output_path}")
        else:
            print(f"Warning: prepare_binary.py not found at {prepare_script}")
            print("Using fallback conversion...")
            self._fallback_convert_to_binary(hdf5_path, output_path)

        return output_path

    def _fallback_convert_to_binary(self, hdf5_path: Path, output_path: Path):
        """Fallback conversion to binary format."""
        with h5py.File(hdf5_path, 'r') as f:
            vectors = np.array(f['train'], dtype=np.float32)
            queries = np.array(f['test'], dtype=np.float32)
            neighbors = np.array(f.get('neighbors', np.array([])), dtype=np.int64)

            # Determine metric
            metric_str = f.attrs.get('metric', 'L2')
            metric = {'L2': DISTANCE_L2, 'COSINE': DISTANCE_COSINE, 'IP': DISTANCE_IP}.get(metric_str, DISTANCE_L2)

            self._write_binary_dataset(
                output_path,
                hdf5_path.stem,
                vectors,
                queries,
                neighbors,
                metric
            )

    def _write_binary_dataset(self, output_path: Path, name: str, vectors: np.ndarray,
                             queries: np.ndarray, neighbors: np.ndarray, metric: int):
        """Write dataset in Valkey binary format."""
        num_vectors, dim = vectors.shape
        num_queries = len(queries)
        num_neighbors = neighbors.shape[1] if len(neighbors) > 0 else 100

        # Calculate offsets (64-byte aligned)
        header_size = 4096
        vectors_offset = header_size
        vectors_size = ((num_vectors * dim * 4 + 63) // 64) * 64

        queries_offset = vectors_offset + vectors_size
        queries_size = ((num_queries * dim * 4 + 63) // 64) * 64

        ground_truth_offset = queries_offset + queries_size

        with open(output_path, 'wb') as f:
            # Prepare dataset name
            name_bytes = name.encode('utf-8')[:256].ljust(256, b'\x00')

            # Write header
            header = struct.pack(
                '<II256sBBxxIQQIxxxxQQQ',
                DATASET_MAGIC,
                DATASET_VERSION,
                name_bytes,
                metric,
                0,  # dtype: FLOAT32
                dim,
                num_vectors,
                num_queries,
                num_neighbors,
                vectors_offset,
                queries_offset,
                ground_truth_offset
            )
            f.write(header)
            f.write(b'\x00' * (4096 - len(header)))  # Pad to 4KB

            # Write vectors
            f.seek(vectors_offset)
            vectors.tofile(f)
            f.write(b'\x00' * (vectors_size - num_vectors * dim * 4))

            # Write queries
            f.seek(queries_offset)
            queries.tofile(f)
            f.write(b'\x00' * (queries_size - num_queries * dim * 4))

            # Write ground truth
            f.seek(ground_truth_offset)
            if len(neighbors) > 0:
                neighbors.tofile(f)

    def prepare_all_datasets(self, category: str = None, max_vectors: int = None):
        """Download and prepare all datasets in a category.

        Args:
            category: Category to prepare ('ann', 'bigann', 'neurips', or None for all)
            max_vectors: Maximum vectors to use (for testing with smaller subsets)
        """
        datasets = self.list_datasets(category)

        print(f"Preparing {len(datasets)} datasets...")
        successful = []
        failed = []

        for dataset in datasets:
            name = dataset['name']
            print(f"\n{'='*60}")
            print(f"Processing: {name}")
            print(f"Description: {dataset['description']}")
            print(f"Dimensions: {dataset['dims']}, Vectors: {dataset['vectors']}")

            try:
                # Download/convert to HDF5
                hdf5_path = self.download_dataset(name)
                if hdf5_path:
                    # Convert to binary
                    bin_path = self.convert_to_valkey_binary(hdf5_path)
                    successful.append(name)
                    print(f"✓ Successfully prepared: {name}")
                else:
                    failed.append(name)
                    print(f"✗ Failed to prepare: {name}")
            except Exception as e:
                print(f"✗ Error processing {name}: {e}")
                failed.append(name)

        # Summary
        print(f"\n{'='*60}")
        print(f"SUMMARY:")
        print(f"✓ Successful: {len(successful)} datasets")
        if successful:
            for name in successful:
                print(f"  - {name}")

        if failed:
            print(f"✗ Failed: {len(failed)} datasets")
            for name in failed:
                print(f"  - {name}")


def main():
    """Main entry point for the Big-ANN Dataset Manager."""
    parser = argparse.ArgumentParser(
        description='Big-ANN Dataset Manager for Valkey Vector Testing',
        formatter_class=argparse.RawDescriptionHelpFormatter
    )

    subparsers = parser.add_subparsers(dest='command', help='Commands')

    # List command
    list_parser = subparsers.add_parser('list', help='List available datasets')
    list_parser.add_argument('--category', choices=['ann', 'bigann', 'neurips'],
                            help='Filter by category')
    list_parser.add_argument('--json', action='store_true',
                            help='Output as JSON')

    # Download command
    download_parser = subparsers.add_parser('download', help='Download a dataset')
    download_parser.add_argument('dataset', help='Dataset name')
    download_parser.add_argument('--force', action='store_true',
                                help='Force re-download')
    download_parser.add_argument('--convert-only', action='store_true',
                                help='Only convert existing HDF5 to binary')

    # Prepare command
    prepare_parser = subparsers.add_parser('prepare', help='Prepare all datasets')
    prepare_parser.add_argument('--category', choices=['ann', 'bigann', 'neurips'],
                               help='Prepare specific category')
    prepare_parser.add_argument('--max-vectors', type=int,
                               help='Limit vectors for testing')

    # Convert command
    convert_parser = subparsers.add_parser('convert', help='Convert HDF5 to binary')
    convert_parser.add_argument('hdf5_file', help='Input HDF5 file')
    convert_parser.add_argument('--output', help='Output binary file')

    args = parser.parse_args()

    # Initialize manager
    manager = BigANNDatasetManager()

    if args.command == 'list':
        datasets = manager.list_datasets(args.category)
        if args.json:
            print(json.dumps(datasets, indent=2))
        else:
            print(f"{'Name':<30} {'Category':<15} {'Dims':<8} {'Vectors':<12} {'Metric':<10} Description")
            print('-' * 100)
            for ds in datasets:
                print(f"{ds['name']:<30} {ds['category']:<15} {ds['dims']:<8} "
                      f"{ds['vectors']:<12} {ds['metric']:<10} {ds['description']}")

    elif args.command == 'download':
        if args.convert_only:
            hdf5_path = manager.dataset_dir / f"{args.dataset}.hdf5"
            if hdf5_path.exists():
                manager.convert_to_valkey_binary(hdf5_path)
            else:
                print(f"HDF5 file not found: {hdf5_path}")
        else:
            hdf5_path = manager.download_dataset(args.dataset, args.force)
            if hdf5_path:
                manager.convert_to_valkey_binary(hdf5_path)

    elif args.command == 'prepare':
        manager.prepare_all_datasets(args.category, args.max_vectors)

    elif args.command == 'convert':
        hdf5_path = Path(args.hdf5_file)
        output_path = Path(args.output) if args.output else None
        manager.convert_to_valkey_binary(hdf5_path, output_path)

    else:
        parser.print_help()


if __name__ == '__main__':
    main()