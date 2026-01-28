#!/usr/bin/env python3
"""
Benchmark Analysis Script v2
Processes all benchmark directories and creates comparison plots for Byzantine vs Honest configurations
Includes collision statistics analysis
"""

import os
import re
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path

# Constants
CAR_SPEED = 14.0          # m/s
INTERSECTION_WIDTH = 15.0 # meters
CLEARANCE_TIME = INTERSECTION_WIDTH / CAR_SPEED

# Regex patterns for metrics
patterns = {
    "msg_recv":    re.compile(r"\[METRICS (\d+)\] Messages received: (\d+)"),
    "msg_sent":    re.compile(r"\[METRICS (\d+)\] Messages sent: (\d+)"),
    "latency":     re.compile(r"\[METRICS (\d+)\] Consensus Latency:\s+([\d\.]+)"),
    "start_time":  re.compile(r"\[METRICS (\d+)\] Consensus Start Time: ([\d\.]+)"),
    "end_time":    re.compile(r"\[METRICS (\d+)\] Consensus End Time:\s+([\d\.]+)"),
    "wait_time":   re.compile(r"\[METRICS (\d+)\] Projected_Total_Wait: ([\d\.]+)"),
    "resume_time": re.compile(r"\[METRICS (\d+)\] Scheduled_Resume: ([\d\.]+)"),
    "stop_time":   re.compile(r"\[METRICS (\d+)\] Stop_Time: ([\d\.]+)"),
    "success":     re.compile(r"\[METRICS (\d+)\] Consensus Completed!"),
    "failure":     re.compile(r"\[METRICS (\d+)\] \(consensus succeeded : FALSE\)"),
}

def parse_log_file(filename):
    """Parse a single log file and extract metrics per vehicle"""
    vehicles = {}
    run_successes = 0
    run_failures = 0

    with open(filename, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            line = line.strip()

            def get_veh(rid):
                if rid not in vehicles:
                    vehicles[rid] = {}
                return vehicles[rid]

            # Messages Received
            m = patterns["msg_recv"].search(line)
            if m:
                get_veh(m.group(1))["recv"] = int(m.group(2))

            # Messages Sent
            m = patterns["msg_sent"].search(line)
            if m:
                get_veh(m.group(1))["sent"] = int(m.group(2))

            # Consensus Latency
            m = patterns["latency"].search(line)
            if m:
                get_veh(m.group(1))["latency"] = float(m.group(2))

            # Wait Time
            m = patterns["wait_time"].search(line)
            if m:
                get_veh(m.group(1))["wait_time"] = float(m.group(2))

            # Stop Time
            m = patterns["stop_time"].search(line)
            if m:
                get_veh(m.group(1))["stop_time"] = float(m.group(2))

            # Resume Time
            m = patterns["resume_time"].search(line)
            if m:
                get_veh(m.group(1))["resume_time"] = float(m.group(2))

            # Success/Failure
            if patterns["success"].search(line):
                run_successes += 1
            if patterns["failure"].search(line):
                run_failures += 1
                m_fail = patterns["failure"].search(line)
                if m_fail:
                    get_veh(m_fail.group(1))["status"] = "FALLBACK"

    return vehicles, run_successes, run_failures

def process_directory(dir_path):
    """Process all log files in a directory and compute averages"""
    log_files = sorted([f for f in os.listdir(dir_path) if f.endswith('.log')])

    if not log_files:
        return None

    all_metrics = {
        'latencies': [],
        'messages_sent': [],
        'messages_recv': [],
        'wait_times': [],
        'throughputs': []
    }

    for log_file in log_files:
        log_path = os.path.join(dir_path, log_file)
        vehicles, successes, failures = parse_log_file(log_path)

        # Collect metrics from this run
        run_latencies = []
        run_msgs_sent = []
        run_msgs_recv = []
        run_wait_times = []

        stop_times = []
        resume_times = []

        for rid, data in vehicles.items():
            if 'latency' in data:
                run_latencies.append(data['latency'])
            if 'sent' in data:
                run_msgs_sent.append(data['sent'])
            if 'recv' in data:
                run_msgs_recv.append(data['recv'])
            if 'wait_time' in data:
                run_wait_times.append(data['wait_time'])
            if 'stop_time' in data:
                stop_times.append(data['stop_time'])
            if 'resume_time' in data:
                resume_times.append(data['resume_time'])

        # Calculate throughput for this run
        if stop_times and resume_times:
            first_arrival = min(stop_times)
            last_resume = max(resume_times)
            last_exit = last_resume + CLEARANCE_TIME
            duration_sec = last_exit - first_arrival
            if duration_sec > 0:
                num_cars = len(vehicles)
                throughput_vpm = (num_cars / duration_sec) * 60
                all_metrics['throughputs'].append(throughput_vpm)

        # Add run averages to overall metrics
        if run_latencies:
            all_metrics['latencies'].append(np.mean(run_latencies))
        if run_msgs_sent:
            all_metrics['messages_sent'].append(np.sum(run_msgs_sent))
        if run_msgs_recv:
            all_metrics['messages_recv'].append(np.sum(run_msgs_recv))
        if run_wait_times:
            all_metrics['wait_times'].append(np.mean(run_wait_times))

    # Compute averages across all runs
    result = {
        'avg_latency': np.mean(all_metrics['latencies']) if all_metrics['latencies'] else 0,
        'std_latency': np.std(all_metrics['latencies']) if all_metrics['latencies'] else 0,
        'avg_msgs_sent': np.mean(all_metrics['messages_sent']) if all_metrics['messages_sent'] else 0,
        'std_msgs_sent': np.std(all_metrics['messages_sent']) if all_metrics['messages_sent'] else 0,
        'avg_msgs_recv': np.mean(all_metrics['messages_recv']) if all_metrics['messages_recv'] else 0,
        'std_msgs_recv': np.std(all_metrics['messages_recv']) if all_metrics['messages_recv'] else 0,
        'avg_throughput': np.mean(all_metrics['throughputs']) if all_metrics['throughputs'] else 0,
        'std_throughput': np.std(all_metrics['throughputs']) if all_metrics['throughputs'] else 0,
        'num_runs': len(log_files)
    }

    return result

def parse_collision_data(collision_file):
    """Parse collision statistics from the collision file"""
    collision_data = {}

    with open(collision_file, 'r') as f:
        content = f.read()

    # Split by network size sections
    sections = re.split(r'^(\d+):\s*$', content, flags=re.MULTILINE)

    for i in range(1, len(sections), 2):
        n = int(sections[i])
        section_text = sections[i + 1]

        # Parse each node's data
        node_pattern = r'--- Node\[(\d+)\] ---\s+' + \
                      r'Sent: (\d+) packets\s+' + \
                      r'Received: Unicast=\d+, Broadcast=(\d+)\s+' + \
                      r'PHY Collisions: (\d+)\s+' + \
                      r'MAC Collisions: (\d+)\s+' + \
                      r'SNIR Lost \(bad signal\): (\d+)\s+' + \
                      r'RXTX Lost \(simultaneous RX/TX\): (\d+)\s+' + \
                      r'Total Lost: (\d+)'

        nodes = re.findall(node_pattern, section_text)

        total_sent = 0
        total_recv = 0
        total_phy_collisions = 0
        total_mac_collisions = 0
        total_snir_lost = 0
        total_rxtx_lost = 0
        total_lost = 0

        for node in nodes:
            total_sent += int(node[1])
            total_recv += int(node[2])
            total_phy_collisions += int(node[3])
            total_mac_collisions += int(node[4])
            total_snir_lost += int(node[5])
            total_rxtx_lost += int(node[6])
            total_lost += int(node[7])

        num_nodes = len(nodes)

        if num_nodes > 0:
            collision_data[n] = {
                'avg_phy_collisions': total_phy_collisions / num_nodes,
                'avg_mac_collisions': total_mac_collisions / num_nodes,
                'avg_snir_lost': total_snir_lost / num_nodes,
                'avg_rxtx_lost': total_rxtx_lost / num_nodes,
                'avg_total_lost': total_lost / num_nodes,
                'avg_sent': total_sent / num_nodes,
                'avg_recv': total_recv / num_nodes,
                'packet_loss_rate': (total_lost / (total_sent + total_lost)) * 100 if (total_sent + total_lost) > 0 else 0
            }

    return collision_data

def main():
    benchmark_dir = '/home/yash/omnetpp/fourway/benchmarks'
    collision_file = '/home/yash/omnetpp/fourway/collions.txt'

    # Parse collision data
    print("Parsing collision data...")
    collision_data = parse_collision_data(collision_file)

    for n, data in sorted(collision_data.items()):
        print(f"n={n}: Avg PHY Collisions={data['avg_phy_collisions']:.1f}, "
              f"Avg Total Lost={data['avg_total_lost']:.1f}, "
              f"Packet Loss Rate={data['packet_loss_rate']:.2f}%")

    # Find all benchmark directories
    dirs = sorted([d for d in os.listdir(benchmark_dir)
                   if os.path.isdir(os.path.join(benchmark_dir, d))])

    # Parse directory names to extract configuration
    configs = {}

    for dir_name in dirs:
        match = re.match(r'(\d+)run_(BYZ|Honest)', dir_name)
        if match:
            n = int(match.group(1))
            config_type = match.group(2)

            print(f"\nProcessing {dir_name}...")
            dir_path = os.path.join(benchmark_dir, dir_name)
            metrics = process_directory(dir_path)

            if metrics:
                if n not in configs:
                    configs[n] = {}
                configs[n][config_type] = metrics

                print(f"  - {metrics['num_runs']} runs processed")
                print(f"  - Avg Latency: {metrics['avg_latency']:.3f}s (±{metrics['std_latency']:.3f})")
                print(f"  - Avg Msgs Sent: {metrics['avg_msgs_sent']:.1f} (±{metrics['std_msgs_sent']:.1f})")
                print(f"  - Avg Msgs Recv: {metrics['avg_msgs_recv']:.1f} (±{metrics['std_msgs_recv']:.1f})")
                print(f"  - Avg Throughput: {metrics['avg_throughput']:.2f} VPM (±{metrics['std_throughput']:.2f})")

    # Prepare data for plotting
    n_values = sorted(configs.keys())

    # Separate BYZ and Honest data
    byz_latency = [configs[n].get('BYZ', {}).get('avg_latency', 0) for n in n_values]
    byz_latency_std = [configs[n].get('BYZ', {}).get('std_latency', 0) for n in n_values]
    honest_latency = [configs[n].get('Honest', {}).get('avg_latency', 0) for n in n_values]
    honest_latency_std = [configs[n].get('Honest', {}).get('std_latency', 0) for n in n_values]

    byz_msgs_sent = [configs[n].get('BYZ', {}).get('avg_msgs_sent', 0) for n in n_values]
    byz_msgs_sent_std = [configs[n].get('BYZ', {}).get('std_msgs_sent', 0) for n in n_values]
    honest_msgs_sent = [configs[n].get('Honest', {}).get('avg_msgs_sent', 0) for n in n_values]
    honest_msgs_sent_std = [configs[n].get('Honest', {}).get('std_msgs_sent', 0) for n in n_values]

    byz_msgs_recv = [configs[n].get('BYZ', {}).get('avg_msgs_recv', 0) for n in n_values]
    byz_msgs_recv_std = [configs[n].get('BYZ', {}).get('std_msgs_recv', 0) for n in n_values]
    honest_msgs_recv = [configs[n].get('Honest', {}).get('avg_msgs_recv', 0) for n in n_values]
    honest_msgs_recv_std = [configs[n].get('Honest', {}).get('std_msgs_recv', 0) for n in n_values]

    byz_throughput = [configs[n].get('BYZ', {}).get('avg_throughput', 0) for n in n_values]
    byz_throughput_std = [configs[n].get('BYZ', {}).get('std_throughput', 0) for n in n_values]
    honest_throughput = [configs[n].get('Honest', {}).get('avg_throughput', 0) for n in n_values]
    honest_throughput_std = [configs[n].get('Honest', {}).get('std_throughput', 0) for n in n_values]

    # Collision data
    collision_phy = [collision_data.get(n, {}).get('avg_phy_collisions', 0) for n in n_values]
    collision_total_lost = [collision_data.get(n, {}).get('avg_total_lost', 0) for n in n_values]
    collision_snir = [collision_data.get(n, {}).get('avg_snir_lost', 0) for n in n_values]
    collision_rxtx = [collision_data.get(n, {}).get('avg_rxtx_lost', 0) for n in n_values]
    collision_loss_rate = [collision_data.get(n, {}).get('packet_loss_rate', 0) for n in n_values]

    # Create the plot with 4 subplots
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    fig.suptitle('Byzantine vs Honest Comparison (n=4, 8, 16, 32)', fontsize=16, fontweight='bold')

    # Plot 1: Consensus Latency
    ax1 = axes[0, 0]
    ax1.errorbar(n_values, byz_latency, yerr=byz_latency_std, marker='o', linestyle='-',
                 linewidth=2, markersize=8, capsize=5, label='Byzantine', color='#d62728')
    ax1.errorbar(n_values, honest_latency, yerr=honest_latency_std, marker='s', linestyle='-',
                 linewidth=2, markersize=8, capsize=5, label='Honest', color='#1f77b4')
    ax1.set_title('Consensus Latency', fontsize=14, fontweight='bold')
    ax1.set_xlabel('Number of Nodes (n)', fontsize=12)
    ax1.set_ylabel('Average Latency (seconds)', fontsize=12)
    ax1.set_xticks(n_values)
    ax1.legend(fontsize=11, loc='best')
    ax1.grid(True, alpha=0.3)

    # Plot 2: Throughput
    ax2 = axes[0, 1]
    ax2.errorbar(n_values, byz_throughput, yerr=byz_throughput_std, marker='o', linestyle='-',
                 linewidth=2, markersize=8, capsize=5, label='Byzantine', color='#d62728')
    ax2.errorbar(n_values, honest_throughput, yerr=honest_throughput_std, marker='s', linestyle='-',
                 linewidth=2, markersize=8, capsize=5, label='Honest', color='#1f77b4')
    ax2.set_title('Throughput', fontsize=14, fontweight='bold')
    ax2.set_xlabel('Number of Nodes (n)', fontsize=12)
    ax2.set_ylabel('Vehicles Per Minute (VPM)', fontsize=12)
    ax2.set_xticks(n_values)
    ax2.legend(fontsize=11, loc='best')
    ax2.grid(True, alpha=0.3)

    # Plot 3: Messages Sent vs Received (Combined)
    ax3 = axes[1, 0]
    x_pos = np.arange(len(n_values))
    width = 0.2

    # BYZ sent and received
    ax3.bar(x_pos - 1.5*width, byz_msgs_sent, width, yerr=byz_msgs_sent_std,
            label='BYZ Sent', color='#d62728', alpha=0.8, capsize=3)
    ax3.bar(x_pos - 0.5*width, byz_msgs_recv, width, yerr=byz_msgs_recv_std,
            label='BYZ Recv', color='#ff9896', alpha=0.8, capsize=3)

    # Honest sent and received
    ax3.bar(x_pos + 0.5*width, honest_msgs_sent, width, yerr=honest_msgs_sent_std,
            label='Honest Sent', color='#1f77b4', alpha=0.8, capsize=3)
    ax3.bar(x_pos + 1.5*width, honest_msgs_recv, width, yerr=honest_msgs_recv_std,
            label='Honest Recv', color='#aec7e8', alpha=0.8, capsize=3)

    ax3.set_title('Messages Sent vs Received (Total per Run)', fontsize=14, fontweight='bold')
    ax3.set_xlabel('Number of Nodes (n)', fontsize=12)
    ax3.set_ylabel('Average Messages', fontsize=12)
    ax3.set_xticks(x_pos)
    ax3.set_xticklabels(n_values)
    ax3.legend(fontsize=10, loc='upper left', ncol=2)
    ax3.grid(True, alpha=0.3, axis='y')

    # Plot 4: Collision Statistics
    ax4 = axes[1, 1]
    ax4_twin = ax4.twinx()

    # Stacked bar chart for collision types
    ax4.bar(n_values, collision_snir, label='SNIR Lost', color='#ff7f0e', alpha=0.8)
    ax4.bar(n_values, collision_rxtx, bottom=collision_snir,
            label='RXTX Lost', color='#2ca02c', alpha=0.8)
    ax4.bar(n_values, collision_phy,
            bottom=[collision_snir[i] + collision_rxtx[i] for i in range(len(n_values))],
            label='PHY Collisions', color='#9467bd', alpha=0.8)

    # Packet loss rate on secondary axis
    ax4_twin.plot(n_values, collision_loss_rate, marker='D', linestyle='--',
                  linewidth=2, markersize=8, color='black', label='Loss Rate %')

    ax4.set_title('Collision & Packet Loss Statistics', fontsize=14, fontweight='bold')
    ax4.set_xlabel('Number of Nodes (n)', fontsize=12)
    ax4.set_ylabel('Average Packets Lost/Collisions per Node', fontsize=12)
    ax4_twin.set_ylabel('Packet Loss Rate (%)', fontsize=12)
    ax4.set_xticks(n_values)

    # Combine legends
    lines1, labels1 = ax4.get_legend_handles_labels()
    lines2, labels2 = ax4_twin.get_legend_handles_labels()
    ax4.legend(lines1 + lines2, labels1 + labels2, fontsize=10, loc='upper left')

    ax4.grid(True, alpha=0.3, axis='y')

    plt.tight_layout(rect=[0, 0, 1, 0.97])
    output_file = os.path.join(benchmark_dir, 'benchmark_comparison_v2.png')
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"\n✓ Plot saved to: {output_file}")

    # Save numerical results to CSV
    csv_data = []
    for n in n_values:
        for config_type in ['BYZ', 'Honest']:
            if config_type in configs[n]:
                m = configs[n][config_type]
                row = {
                    'n': n,
                    'type': config_type,
                    'avg_latency': m['avg_latency'],
                    'std_latency': m['std_latency'],
                    'avg_msgs_sent': m['avg_msgs_sent'],
                    'std_msgs_sent': m['std_msgs_sent'],
                    'avg_msgs_recv': m['avg_msgs_recv'],
                    'std_msgs_recv': m['std_msgs_recv'],
                    'avg_throughput': m['avg_throughput'],
                    'std_throughput': m['std_throughput'],
                    'num_runs': m['num_runs']
                }

                # Add collision data (same for both BYZ and Honest)
                if n in collision_data:
                    row.update({
                        'avg_phy_collisions': collision_data[n]['avg_phy_collisions'],
                        'avg_total_lost': collision_data[n]['avg_total_lost'],
                        'avg_snir_lost': collision_data[n]['avg_snir_lost'],
                        'avg_rxtx_lost': collision_data[n]['avg_rxtx_lost'],
                        'packet_loss_rate': collision_data[n]['packet_loss_rate']
                    })

                csv_data.append(row)

    df = pd.DataFrame(csv_data)
    csv_file = os.path.join(benchmark_dir, 'benchmark_summary_v2.csv')
    df.to_csv(csv_file, index=False)
    print(f"✓ Summary data saved to: {csv_file}")

    # Print summary table
    print("\n" + "="*100)
    print("BENCHMARK SUMMARY")
    print("="*100)
    print(df.to_string(index=False))
    print("="*100)

if __name__ == "__main__":
    main()
