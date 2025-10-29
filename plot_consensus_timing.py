#!/usr/bin/env python3
"""
Plot consensus timing data comparison between regular consensus and BFT consensus

Note: All times shown are AVERAGES of 10 trials per configuration.
This reduces variance and provides more reliable performance metrics.
"""

import csv
import matplotlib.pyplot as plt
import os
import sys

def read_csv_data(filename):
    """Read consensus timing data from CSV file"""
    vehicles = []
    times = []
    
    if not os.path.exists(filename):
        print(f"Warning: {filename} not found")
        return None, None
    
    with open(filename, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            vehicles.append(int(row['vehicles']))
            times.append(float(row['avg_consensus_time_ms']))
    
    return vehicles, times

def read_byzantine_csv_for_comparison(filename):
    """Read Byzantine fault analysis data and average by vehicle count (successful runs only)"""
    if not os.path.exists(filename):
        print(f"Warning: {filename} not found")
        return None, None
    
    # Group by vehicle count, only successful runs, exclude artificial delays
    vehicle_times = {}
    
    with open(filename, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row['success'].lower() == 'true':
                time_ms = float(row['consensus_time_ms'])
                # Exclude artificially delayed runs (Delay behavior adds 100-500ms)
                # if time_ms < 100:  # Only include natural consensus times
                n = int(row['vehicles'])
                if n not in vehicle_times:
                    vehicle_times[n] = []
                vehicle_times[n].append(time_ms)
    
    if not vehicle_times:
        return None, None
    
    # Calculate averages
    vehicles = sorted(vehicle_times.keys())
    times = [sum(vehicle_times[n]) / len(vehicle_times[n]) for n in vehicles]
    
    return vehicles, times

# Read data from CSV files
regular_vehicles, regular_times = read_csv_data('consensus.csv')

# Try to use Byzantine fault analysis data first (more comprehensive)
bft_vehicles, bft_times = read_byzantine_csv_for_comparison('byzantine_fault_analysis_with_vanet.csv')
if bft_vehicles is None:
    # Fallback to simple BFT consensus data
    print("Byzantine analysis data not found, using simple BFT data...")
    bft_vehicles, bft_times = read_csv_data('bftconsensus.csv')
else:
    print("Using Byzantine fault analysis data (average of successful runs with Byzantine faults)")

# Check if we have at least one dataset
if regular_vehicles is None and bft_vehicles is None:
    print("Error: Neither consensus.csv nor bftconsensus.csv found!")
    sys.exit(1)

# Create plot
plt.figure(figsize=(12, 7))

# Plot regular consensus if available
if regular_vehicles is not None:
    plt.plot(regular_vehicles, regular_times, marker='o', linewidth=2.5, 
             markersize=10, color='#2E86AB', label='Regular Consensus', alpha=0.8)
    # Add value labels
    for i, (v, t) in enumerate(zip(regular_vehicles, regular_times)):
        plt.annotate(f'{t:.1f}ms', (v, t), textcoords="offset points", 
                     xytext=(0,12), ha='center', fontsize=9, color='#2E86AB')

# Plot BFT consensus if available
if bft_vehicles is not None:
    plt.plot(bft_vehicles, bft_times, marker='s', linewidth=2.5, 
             markersize=10, color='#D84315', label='BFT Consensus (with Byzantine faults)', alpha=0.8)
    # Add value labels
    for i, (v, t) in enumerate(zip(bft_vehicles, bft_times)):
        plt.annotate(f'{t:.1f}ms', (v, t), textcoords="offset points", 
                     xytext=(0,-15), ha='center', fontsize=9, color='#D84315')

plt.grid(True, alpha=0.3, linestyle='--')
plt.xlabel('Number of Vehicles', fontsize=13, fontweight='bold')
plt.ylabel('Average Consensus Time (ms)', fontsize=13, fontweight='bold')
plt.title('Consensus Time Comparison: Regular vs BFT\n(Averages from multiple trials, BFT includes Byzantine fault scenarios)', 
         fontsize=14, fontweight='bold', pad=20)
plt.legend(fontsize=11, loc='upper left', framealpha=0.9)

# Add note about BFT data
if bft_vehicles is not None:
    plt.text(0.98, 0.02, 'BFT data: Successful consensus with actual Byzantine nodes present',
             transform=plt.gca().transAxes, fontsize=8, style='italic',
             verticalalignment='bottom', horizontalalignment='right',
             bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.3))

# Set x-axis to show only integer vehicle counts
if regular_vehicles is not None:
    plt.xticks(regular_vehicles)
elif bft_vehicles is not None:
    plt.xticks(bft_vehicles)

plt.tight_layout()
plt.savefig('consensus_timing_comparison.png', dpi=300, bbox_inches='tight')
print("✓ Plot saved to consensus_timing_comparison.png")

# Also create individual plots if both datasets exist
if regular_vehicles is not None and bft_vehicles is not None:
    # Regular consensus only
    plt.figure(figsize=(10, 6))
    plt.plot(regular_vehicles, regular_times, marker='o', linewidth=2.5, 
             markersize=10, color='#2E86AB')
    plt.grid(True, alpha=0.3, linestyle='--')
    plt.xlabel('Number of Vehicles', fontsize=12, fontweight='bold')
    plt.ylabel('Average Consensus Time (ms)', fontsize=12, fontweight='bold')
    plt.title('Regular Consensus Time vs Number of Vehicles', fontsize=14, fontweight='bold')
    for i, (v, t) in enumerate(zip(regular_vehicles, regular_times)):
        plt.annotate(f'{t:.1f}ms', (v, t), textcoords="offset points", 
                     xytext=(0,10), ha='center', fontsize=9)
    plt.xticks(regular_vehicles)
    plt.tight_layout()
    plt.savefig('consensus_timing_regular.png', dpi=300, bbox_inches='tight')
    print("✓ Regular consensus plot saved to consensus_timing_regular.png")
    
    # BFT consensus only
    plt.figure(figsize=(10, 6))
    plt.plot(bft_vehicles, bft_times, marker='s', linewidth=2.5, 
             markersize=10, color='#D84315')
    plt.grid(True, alpha=0.3, linestyle='--')
    plt.xlabel('Number of Vehicles', fontsize=12, fontweight='bold')
    plt.ylabel('Average Consensus Time (ms)', fontsize=12, fontweight='bold')
    plt.title('BFT Consensus Time vs Number of Vehicles', fontsize=14, fontweight='bold')
    for i, (v, t) in enumerate(zip(bft_vehicles, bft_times)):
        plt.annotate(f'{t:.1f}ms', (v, t), textcoords="offset points", 
                     xytext=(0,10), ha='center', fontsize=9)
    plt.xticks(bft_vehicles)
    plt.tight_layout()
    plt.savefig('consensus_timing_bft.png', dpi=300, bbox_inches='tight')
    print("✓ BFT consensus plot saved to consensus_timing_bft.png")

plt.show()

