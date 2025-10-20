#!/usr/bin/env python3
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Copyright (c) 2024-present, Zvi Schneider
#
# Analyze and visualize ef_search performance results across multiple datasets
#
# This file is part of valkey-search-benchmark and is licensed under the
# BSD 3-Clause License. See the LICENSE file in the root directory.
#
"""
Analyze and visualize ef_search performance results across multiple datasets
"""

import csv
import glob
import sys
from pathlib import Path
from typing import Dict, List, Tuple
import argparse

def load_results(pattern: str = "ef_search_results_*.csv") -> Dict[str, List[dict]]:
    """Load all result files matching the pattern"""
    results = {}

    for filepath in glob.glob(pattern):
        with open(filepath, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                dataset = row['dataset']
                if dataset not in results:
                    results[dataset] = []
                results[dataset].append(row)

    return results

def analyze_dataset(dataset_name: str, data: List[dict]) -> dict:
    """Analyze results for a single dataset"""

    # Filter valid data
    valid_data = []
    for row in data:
        try:
            if row['avg_recall'] != 'N/A' and row['qps'] != 'N/A':
                valid_data.append({
                    'ef_search': int(row['ef_search']),
                    'avg_recall': float(row['avg_recall']),
                    'min_recall': float(row['min_recall']) if row['min_recall'] != 'N/A' else 0,
                    'max_recall': float(row['max_recall']) if row['max_recall'] != 'N/A' else 100,
                    'qps': float(row['qps']),
                    'latency_ms': float(row['latency_ms']) if row['latency_ms'] != 'N/A' else 0
                })
        except (ValueError, KeyError):
            continue

    if not valid_data:
        return {}

    # Sort by ef_search
    valid_data.sort(key=lambda x: x['ef_search'])

    # Find best configurations
    best_recall = max(valid_data, key=lambda x: x['avg_recall'])
    best_qps = max(valid_data, key=lambda x: x['qps'])

    # Find sweet spot (best QPS with recall >= 80%)
    sweet_spots = [d for d in valid_data if d['avg_recall'] >= 80]
    best_sweet_spot = max(sweet_spots, key=lambda x: x['qps']) if sweet_spots else None

    # Calculate recall improvement rate
    recall_improvements = []
    for i in range(1, len(valid_data)):
        ef_diff = valid_data[i]['ef_search'] - valid_data[i-1]['ef_search']
        recall_diff = valid_data[i]['avg_recall'] - valid_data[i-1]['avg_recall']
        if ef_diff > 0:
            recall_improvements.append(recall_diff / ef_diff)

    avg_improvement = sum(recall_improvements) / len(recall_improvements) if recall_improvements else 0

    return {
        'dataset': dataset_name,
        'num_tests': len(valid_data),
        'best_recall': best_recall,
        'best_qps': best_qps,
        'sweet_spot': best_sweet_spot,
        'avg_recall_improvement': avg_improvement,
        'data_points': valid_data
    }

def generate_report(results: Dict[str, List[dict]], output_file: str = None):
    """Generate a comprehensive report"""

    analyses = {}
    for dataset, data in results.items():
        analysis = analyze_dataset(dataset, data)
        if analysis:
            analyses[dataset] = analysis

    if not analyses:
        print("No valid data found to analyze")
        return

    # Print report
    output = []
    output.append("=" * 80)
    output.append("EF_SEARCH PERFORMANCE ANALYSIS REPORT")
    output.append("=" * 80)
    output.append("")

    # Summary table
    output.append("DATASET SUMMARY")
    output.append("-" * 80)
    output.append(f"{'Dataset':<20} {'Tests':<8} {'Best Recall':<15} {'Best QPS':<15} {'Sweet Spot':<15}")
    output.append("-" * 80)

    for dataset, analysis in sorted(analyses.items()):
        best_recall = analysis['best_recall']
        best_qps = analysis['best_qps']
        sweet_spot = analysis['sweet_spot']

        recall_str = f"{best_recall['avg_recall']:.1f}% (ef={best_recall['ef_search']})"
        qps_str = f"{best_qps['qps']:.0f} (ef={best_qps['ef_search']})"

        if sweet_spot:
            sweet_str = f"{sweet_spot['qps']:.0f}@{sweet_spot['avg_recall']:.1f}%"
        else:
            sweet_str = "N/A"

        output.append(f"{dataset:<20} {analysis['num_tests']:<8} {recall_str:<15} {qps_str:<15} {sweet_str:<15}")

    output.append("")

    # Detailed analysis per dataset
    for dataset, analysis in sorted(analyses.items()):
        output.append("=" * 80)
        output.append(f"DATASET: {dataset}")
        output.append("=" * 80)

        # Performance table
        output.append("")
        output.append(f"{'ef_search':<10} {'Recall %':<12} {'QPS':<10} {'Latency ms':<12} {'Efficiency':<10}")
        output.append("-" * 60)

        for point in analysis['data_points']:
            efficiency = point['qps'] * point['avg_recall'] / 100  # Simple efficiency metric
            output.append(f"{point['ef_search']:<10} {point['avg_recall']:<12.1f} {point['qps']:<10.0f} {point['latency_ms']:<12.1f} {efficiency:<10.0f}")

        output.append("")

        # Recommendations
        output.append("RECOMMENDATIONS:")
        output.append("-" * 40)

        best_recall = analysis['best_recall']
        best_qps = analysis['best_qps']
        sweet_spot = analysis['sweet_spot']

        output.append(f"• For maximum recall ({best_recall['avg_recall']:.1f}%): use ef_search={best_recall['ef_search']}")
        output.append(f"• For maximum throughput ({best_qps['qps']:.0f} QPS): use ef_search={best_qps['ef_search']}")

        if sweet_spot:
            output.append(f"• Balanced configuration: ef_search={sweet_spot['ef_search']} ({sweet_spot['avg_recall']:.1f}% recall, {sweet_spot['qps']:.0f} QPS)")

        # Diminishing returns analysis
        if len(analysis['data_points']) > 3:
            mid_point = analysis['data_points'][len(analysis['data_points'])//2]
            last_point = analysis['data_points'][-1]

            recall_gain = last_point['avg_recall'] - mid_point['avg_recall']
            qps_loss = mid_point['qps'] - last_point['qps']

            if recall_gain < 2 and qps_loss > 0:
                output.append(f"• Diminishing returns after ef_search={mid_point['ef_search']}")

        output.append("")

    # Cross-dataset comparison
    if len(analyses) > 1:
        output.append("=" * 80)
        output.append("CROSS-DATASET COMPARISON")
        output.append("=" * 80)
        output.append("")

        # Find dataset with best overall performance
        best_overall_recall = max(analyses.items(), key=lambda x: x[1]['best_recall']['avg_recall'])
        best_overall_qps = max(analyses.items(), key=lambda x: x[1]['best_qps']['qps'])

        output.append(f"• Best overall recall: {best_overall_recall[0]} ({best_overall_recall[1]['best_recall']['avg_recall']:.1f}%)")
        output.append(f"• Best overall QPS: {best_overall_qps[0]} ({best_overall_qps[1]['best_qps']['qps']:.0f})")

        # Calculate recall/QPS trade-off scores
        output.append("")
        output.append("Trade-off Analysis (higher is better):")
        scores = []
        for dataset, analysis in analyses.items():
            if analysis['sweet_spot']:
                score = analysis['sweet_spot']['qps'] * analysis['sweet_spot']['avg_recall'] / 100
                scores.append((dataset, score, analysis['sweet_spot']))

        scores.sort(key=lambda x: x[1], reverse=True)
        for dataset, score, config in scores[:5]:
            output.append(f"  {dataset}: {score:.0f} (ef={config['ef_search']}, {config['avg_recall']:.1f}% @ {config['qps']:.0f} QPS)")

        output.append("")

    # Print or save report
    report_text = "\n".join(output)

    if output_file:
        with open(output_file, 'w') as f:
            f.write(report_text)
        print(f"Report saved to: {output_file}")
    else:
        print(report_text)

    # Generate CSV for further analysis
    csv_file = output_file.replace('.txt', '.csv') if output_file else "ef_search_analysis.csv"
    with open(csv_file, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['dataset', 'ef_search', 'avg_recall', 'min_recall', 'max_recall', 'qps', 'latency_ms'])

        for dataset, analysis in analyses.items():
            for point in analysis['data_points']:
                writer.writerow([
                    dataset,
                    point['ef_search'],
                    point['avg_recall'],
                    point['min_recall'],
                    point['max_recall'],
                    point['qps'],
                    point['latency_ms']
                ])

    print(f"CSV data saved to: {csv_file}")

def main():
    parser = argparse.ArgumentParser(description='Analyze ef_search performance results')
    parser.add_argument('--pattern', default='ef_search_results_*.csv',
                       help='Pattern for result files (default: ef_search_results_*.csv)')
    parser.add_argument('--output', help='Output file for report')
    parser.add_argument('--dataset', help='Analyze specific dataset only')

    args = parser.parse_args()

    # Load results
    results = load_results(args.pattern)

    if not results:
        print(f"No result files found matching pattern: {args.pattern}")
        sys.exit(1)

    # Filter by dataset if specified
    if args.dataset:
        if args.dataset in results:
            results = {args.dataset: results[args.dataset]}
        else:
            print(f"Dataset '{args.dataset}' not found in results")
            print(f"Available datasets: {', '.join(results.keys())}")
            sys.exit(1)

    # Generate report
    generate_report(results, args.output)

if __name__ == '__main__':
    main()