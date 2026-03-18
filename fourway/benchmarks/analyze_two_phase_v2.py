import os
import re
import numpy as np
import matplotlib.pyplot as plt
from collections import defaultdict
from matplotlib.patches import Patch

# ==========================================
# CONFIGURATION
# ==========================================
BENCHMARK_DIR = '/home/yash/omnetpp/fourway/benchmarks'
OUTPUT_FILE = os.path.join(BENCHMARK_DIR, 'two_phase_comparison.png')
CARS_PER_BATCH = 4

def parse_log_file(filepath):
    """Parses the log file and extracts valid metrics per consensus block."""
    data_blocks = []
    current_block = {}
    
    # Regex to match: [METRICS 11] Metric_Name: 1.234
    metric_pattern = re.compile(r'\[METRICS\s+\d+\]\s+([A-Za-z0-9_]+):\s+([\-\d\.]+)')

    try:
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                if "========== CONSENSUS METRICS" in line:
                    current_block = {} # Start a new block
                
                match = metric_pattern.search(line)
                if match:
                    metric_name = match.group(1)
                    metric_value = float(match.group(2))
                    current_block[metric_name] = metric_value
                
                if "========================================================" in line:
                    if current_block:
                        data_blocks.append(current_block)
                        current_block = {}
    except FileNotFoundError:
        print(f"File {filepath} not found.")
        return []

    return data_blocks
def clean_and_group_data(data_blocks):
    """Filters negative values and groups replicas into 'Rounds' using a time threshold."""
    
    # 1. Filter out invalid blocks and store them in a list
    valid_blocks = []
    for block in data_blocks:
        st = block.get("Total_Consensus_Start")
        vl = block.get("View_Consensus_Latency", -1)
        ol = block.get("Order_Consensus_Latency", -1)
        td = block.get("Total_Consensus_Duration", -1)
        
        if st is not None and vl >= 0 and ol >= 0 and td >= 0:
            valid_blocks.append(block)
            
    if not valid_blocks:
        return []

    # 2. Sort all blocks chronologically by start time
    valid_blocks.sort(key=lambda x: x["Total_Consensus_Start"])
    
    # 3. Cluster into rounds based on a time gap threshold
    rounds_data = [] 
    current_round_blocks = []
    round_start_reference = None
    
    # Any cars starting within 2.5 seconds of the first car belong to the same batch/round.
    # The gap between actual physical rounds will be much larger (e.g., 4-5 seconds)
    TIME_THRESHOLD = 2.5 
    
    for block in valid_blocks:
        st = block["Total_Consensus_Start"]
        
        if round_start_reference is None:
            round_start_reference = st
            
        if (st - round_start_reference) <= TIME_THRESHOLD:
            current_round_blocks.append(block)
        else:
            # The gap was larger than the threshold, so this starts a new round
            rounds_data.append(current_round_blocks)
            current_round_blocks = [block]
            round_start_reference = st
            
    if current_round_blocks:
        rounds_data.append(current_round_blocks)
        
    # 4. Calculate the averages for each clustered round
    processed_rounds = []
    for r_idx, r_blocks in enumerate(rounds_data):
        views = [b.get("View_Consensus_Latency", 0) for b in r_blocks]
        orders = [b.get("Order_Consensus_Latency", 0) for b in r_blocks]
        totals = [b.get("Total_Consensus_Duration", 0) for b in r_blocks]
        sents = [b.get("Messages_Sent", 0) for b in r_blocks if "Messages_Sent" in b]
        recvs = [b.get("Messages_Received", 0) for b in r_blocks if "Messages_Received" in b]
        
        avg_view = np.mean(views) if views else 0
        avg_order = np.mean(orders) if orders else 0
        avg_total = np.mean(totals) if totals else 0
        avg_sent = np.mean(sents) if sents else 0
        avg_recv = np.mean(recvs) if recvs else 0
        
        # Use the start time of the first car in the batch as the label
        time_key = r_blocks[0]["Total_Consensus_Start"]
        
        throughput_cars_per_min = (CARS_PER_BATCH / avg_total)  if avg_total > 0 else 0
        
        processed_rounds.append({
            'start_time': time_key,
            'avg_view': avg_view,
            'avg_order': avg_order,
            'avg_total': avg_total,
            'avg_sent': avg_sent,
            'avg_recv': avg_recv,
            'throughput': throughput_cars_per_min
        })
        
    return processed_rounds

def process_directory(dir_path):
    log_files = sorted([f for f in os.listdir(dir_path) if f.endswith('.log')])
    if not log_files:
        return None
        
    all_blocks = []
    for log_file in log_files:
        filepath = os.path.join(dir_path, log_file)
        blocks = parse_log_file(filepath)
        all_blocks.extend(blocks)
        
    return clean_and_group_data(all_blocks)

def main():
    print(f"Scanning {BENCHMARK_DIR} for '2phaseNHonest' directories...")
    dirs = sorted([d for d in os.listdir(BENCHMARK_DIR) if os.path.isdir(os.path.join(BENCHMARK_DIR, d))])
    
    configs = {}
    pattern = re.compile(r'2phase(\d+)Honest', re.IGNORECASE)
    
    for dir_name in dirs:
        match = pattern.match(dir_name)
        if match:
            n = int(match.group(1))
            dir_path = os.path.join(BENCHMARK_DIR, dir_name)
            print(f"Processing {dir_name} (N={n})...")
            
            rounds_data = process_directory(dir_path)
            if rounds_data:
                configs[n] = rounds_data
                print(f"  -> Found {len(rounds_data)} distinct consensus rounds.")
            else:
                print(f"  -> No valid data found.")
                
    if not configs:
        print("No valid configurations found. Exiting.")
        return
        
    n_values = sorted(configs.keys())
    
    # Pre-calculate to ensure there are enough colors
    max_rounds = max(len(rounds) for rounds in configs.values())
    if max_rounds == 0:
        print("No rounds to plot.")
        return
        
    # Standard matplotlib colors
    colors = plt.rcParams['axes.prop_cycle'].by_key()['color']
    
    fig, axes = plt.subplots(2, 2, figsize=(16, 12))
    fig.suptitle('Two Phase Consensus Metrics by Network Size (N)', fontsize=18, fontweight='bold')
    
    ax_lat = axes[0, 0]
    ax_tput = axes[0, 1]
    ax_msg = axes[1, 0]
    ax_breakdown = axes[1, 1] # 4th plot: View vs Order Latency Breakdown
    
    x_positions = np.arange(len(n_values))
    group_width = 0.8  # width allocated for each N on the x-axis
    
    lat_labels = set()
    
    for i, n in enumerate(n_values):
        rounds = configs[n]
        num_rounds = len(rounds)
        if num_rounds == 0: continue
        
        # Width of individual round bar
        bar_width = group_width / max_rounds
        
        # Calculate centers for the round bars so they align nicely inside the N group
        start_x = x_positions[i] - group_width / 2 + bar_width / 2
        
        for r_idx, r_data in enumerate(rounds):
            pos = start_x + r_idx * bar_width
            color = colors[r_idx % len(colors)]
            label = f"Round {r_idx+1}"
            
            # Plot 1: Total Latency
            if label not in lat_labels:
                ax_lat.bar(pos, r_data['avg_total'], width=bar_width*0.9, color=color, label=label)
                lat_labels.add(label)
            else:
                ax_lat.bar(pos, r_data['avg_total'], width=bar_width*0.9, color=color)
                
            # Plot 2: Throughput
            ax_tput.bar(pos, r_data['throughput'], width=bar_width*0.9, color=color)
            
            # Plot 3: Messages Sent vs Received (Stacked internally by Round)
            ax_msg.bar(pos, r_data['avg_sent'], width=bar_width*0.9, color=color, hatch='//', edgecolor='black')
            ax_msg.bar(pos, r_data['avg_recv'], width=bar_width*0.9, bottom=r_data['avg_sent'], color=color, alpha=0.5, edgecolor='black')
            
            # Plot 4: Latency Breakdown (Stacked View vs Order Latency internally by Round)
            ax_breakdown.bar(pos, r_data['avg_view'], width=bar_width*0.9, color=color, hatch='\\\\', edgecolor='black')
            ax_breakdown.bar(pos, r_data['avg_order'], width=bar_width*0.9, bottom=r_data['avg_view'], color=color, alpha=0.5, edgecolor='black')

    # Format Plot 1 (Latency)
    ax_lat.set_title('Total Consensus Latency per Round', fontsize=14, fontweight='bold')
    ax_lat.set_ylabel('Latency (seconds)', fontsize=12)
    ax_lat.set_xticks(x_positions)
    ax_lat.set_xticklabels([f"N={n}" for n in n_values])
    ax_lat.legend(loc='best')
    ax_lat.grid(True, linestyle='--', alpha=0.7, axis='y')
    
    # Format Plot 2 (Throughput)
    ax_tput.set_title(f'Throughput per Round (Batch={CARS_PER_BATCH})', fontsize=14, fontweight='bold')
    ax_tput.set_ylabel('Throughput (Cars / Second)', fontsize=12)
    ax_tput.set_xticks(x_positions)
    ax_tput.set_xticklabels([f"N={n}" for n in n_values])
    ax_tput.grid(True, linestyle='--', alpha=0.7, axis='y')
    
    # Format Plot 3 (Messages)
    ax_msg.set_title('Messages Sent & Received per Node (Stacked)', fontsize=14, fontweight='bold')
    ax_msg.set_ylabel('Total Messages', fontsize=12)
    ax_msg.set_xticks(x_positions)
    ax_msg.set_xticklabels([f"N={n}" for n in n_values])
    msg_legend_elements = [
        Patch(facecolor='gray', hatch='//', edgecolor='black', label='Messages Sent'),
        Patch(facecolor='gray', alpha=0.5, edgecolor='black', label='Messages Received')
    ]
    ax_msg.legend(handles=msg_legend_elements, loc='best')
    ax_msg.grid(True, linestyle='--', alpha=0.7, axis='y')
    
    # Format Plot 4 (Latency Breakdown)
    ax_breakdown.set_title('Latency Breakdown (View vs Order stack)', fontsize=14, fontweight='bold')
    ax_breakdown.set_ylabel('Latency (seconds)', fontsize=12)
    ax_breakdown.set_xticks(x_positions)
    ax_breakdown.set_xticklabels([f"N={n}" for n in n_values])
    breakdown_legend = [
        Patch(facecolor='gray', hatch='\\\\', edgecolor='black', label='View Consensus Latency'),
        Patch(facecolor='gray', alpha=0.5, edgecolor='black', label='Order Consensus Latency')
    ]
    ax_breakdown.legend(handles=breakdown_legend, loc='best')
    ax_breakdown.grid(True, linestyle='--', alpha=0.7, axis='y')
                    
    plt.tight_layout(rect=[0, 0, 1, 0.96])
    plt.savefig(OUTPUT_FILE, dpi=300)
    print(f"✓ Graph successfully saved to {OUTPUT_FILE}")

if __name__ == "__main__":
    main()
    
