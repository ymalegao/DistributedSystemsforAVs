#!/usr/bin/env python3
"""
Plot consensus timing data
"""

import csv
import matplotlib.pyplot as plt

# Read data from CSV
vehicles = []
times = []

with open('consensus_timing_data.csv', 'r') as f:
    reader = csv.DictReader(f)
    for row in reader:
        vehicles.append(int(row['vehicles']))
        times.append(float(row['avg_consensus_time_ms']))

# Create plot
plt.figure(figsize=(10, 6))
plt.plot(vehicles, times, marker='o', linewidth=2, markersize=8, color='#2E86AB')
plt.grid(True, alpha=0.3)
plt.xlabel('Number of Vehicles', fontsize=12, fontweight='bold')
plt.ylabel('Average Consensus Time (ms)', fontsize=12, fontweight='bold')
plt.title('Consensus Time vs Number of Vehicles', fontsize=14, fontweight='bold')

# Add value labels on points
for i, (v, t) in enumerate(zip(vehicles, times)):
    plt.annotate(f'{t:.1f}ms', (v, t), textcoords="offset points", 
                 xytext=(0,10), ha='center', fontsize=9)

plt.tight_layout()
plt.savefig('consensus_timing_plot.png', dpi=300, bbox_inches='tight')
print("Plot saved to consensus_timing_plot.png")
plt.show()

