import re
import numpy as np
import matplotlib.pyplot as plt
from collections import defaultdict

# ==========================================
# CONFIGURATION
# ==========================================
LOG_FILE_PATH = "simulation_output.log" # Replace with your log file name
CARS_PER_BATCH = 4 # Batch size for throughput calculation

def parse_log_file(filepath):
    """Parses the log file and extracts valid metrics per consensus block."""
    data_blocks = []
    current_block = {}
    
    # Regex to match: [METRICS 11] Metric_Name: 1.234 seconds
    metric_pattern = re.compile(r'\[METRICS\s+\d+\]\s+([A-Za-z0-9_]+):\s+([\-\d\.]+)')

    try:
        with open(filepath, 'r') as f:
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
        print(f"File {filepath} not found. Please check the path.")
        return []

    return data_blocks

def clean_and_group_data(data_blocks):
    """Filters negative values and groups replicas into 'Rounds' by start time."""
    # Dictionary to hold rounds: { start_time_cluster: { 'view': [], 'order': [], 'total': [] } }
    rounds_data = defaultdict(lambda: {'view': [], 'order': [], 'total': []})
    
    for block in data_blocks:
        start_time = block.get("Total_Consensus_Start", None)
        view_lat = block.get("View_Consensus_Latency", -1)
        order_lat = block.get("Order_Consensus_Latency", -1)
        total_dur = block.get("Total_Consensus_Duration", -1)
        
        # 1. Clean data: Ignore race condition negatives and missing fields
        if start_time is None or view_lat < 0 or order_lat < 0 or total_dur < 0:
            continue
            
        # 2. Cluster by Start Time (Rounds replicas to nearest 0.5s to group them)
        # E.g., 11.4 and 11.45 will group together into the same round
        cluster_key = round(start_time * 2) / 2 
        
        rounds_data[cluster_key]['view'].append(view_lat)
        rounds_data[cluster_key]['order'].append(order_lat)
        rounds_data[cluster_key]['total'].append(total_dur)
        
    # Sort the rounds sequentially by time
    sorted_rounds = sorted(rounds_data.items())
    
    # Calculate averages for each round
    processed_rounds = []
    for time_key, metrics in sorted_rounds:
        avg_view = np.mean(metrics['view'])
        avg_order = np.mean(metrics['order'])
        avg_total = np.mean(metrics['total'])
        
        # Throughput = Cars Processed / Total Duration of the round (converted to Cars/Minute)
        throughput_cars_per_min = (CARS_PER_BATCH / avg_total) * 60 
        
        processed_rounds.append({
            'start_time': time_key,
            'avg_view': avg_view,
            'avg_order': avg_order,
            'throughput': throughput_cars_per_min
        })
        
    return processed_rounds

def plot_multi_metrics(rounds):
    """Generates a multi-panel grouped bar chart."""
    if not rounds:
        print("No valid data to plot.")
        return
        
    num_rounds = len(rounds)
    x = np.arange(num_rounds)
    
    # Extract data for plotting
    round_labels = [f"Round {i+1}\n(t≈{r['start_time']}s)" for i, r in enumerate(rounds)]
    view_lats = [r['avg_view'] for r in rounds]
    order_lats = [r['avg_order'] for r in rounds]
    throughputs = [r['throughput'] for r in rounds]

    # Create a 1x3 grid of subplots (Multi-Multi)
    fig, (ax1, ax2, ax3) = plt.subplots(1, 3, figsize=(15, 5))
    width = 0.5
    
    # --- Graph 1: View Consensus Latency ---
    ax1.bar(x, view_lats, width, color='#4C72B0')
    ax1.set_title('Average View Consensus Latency', fontweight='bold')
    ax1.set_ylabel('Latency (seconds)')
    ax1.set_xticks(x)
    ax1.set_xticklabels(round_labels)
    ax1.yaxis.grid(True, linestyle='--', alpha=0.7)
    
    # --- Graph 2: Order Consensus Latency ---
    ax2.bar(x, order_lats, width, color='#DD8452')
    ax2.set_title('Average Order Consensus Latency', fontweight='bold')
    ax2.set_ylabel('Latency (seconds)')
    ax2.set_xticks(x)
    ax2.set_xticklabels(round_labels)
    ax2.yaxis.grid(True, linestyle='--', alpha=0.7)
    
    # --- Graph 3: Throughput ---
    ax3.bar(x, throughputs, width, color='#55A868')
    ax3.set_title(f'Throughput per Round\n(Batch Size = {CARS_PER_BATCH})', fontweight='bold')
    ax3.set_ylabel('Throughput (Cars / Minute)')
    ax3.set_xticks(x)
    ax3.set_xticklabels(round_labels)
    ax3.yaxis.grid(True, linestyle='--', alpha=0.7)

    plt.tight_layout()
    plt.show()
    # plt.savefig('bft_consensus_metrics.png', dpi=300) # Uncomment to save

if __name__ == "__main__":
    # Create a dummy log file if you want to test it immediately (optional)
    # with open("simulation_output.log", "w") as f: f.write(...)
    
    print("Parsing logs...")
    raw_blocks = parse_log_file(LOG_FILE_PATH)
    
    print(f"Found {len(raw_blocks)} metrics blocks. Cleaning data...")
    rounds_data = clean_and_group_data(raw_blocks)
    
    print(f"Aggregated into {len(rounds_data)} distinct consensus rounds. Plotting...")
    plot_multi_metrics(rounds_data)