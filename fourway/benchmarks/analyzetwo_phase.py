import re
import matplotlib.pyplot as plt

def parse_log_file(file_path):
    """Parses the BFT-SMaRt log file and extracts consensus latencies."""
    metrics_data = {}
    current_replica = None
    
    with open(file_path, 'r') as file:
        for line in file:
            # Detect new replica block
            replica_match = re.search(r"========== CONSENSUS METRICS \(Replica (\d+)\) ==========", line)
            if replica_match:
                current_replica = replica_match.group(1)
                metrics_data[current_replica] = {
                    'view_sig_dur': None,
                    'view_bft_dur': 0.0,
                    'order_sig_dur': None,
                    'order_bft_dur': 0.0
                }
                continue
                
            if current_replica is None:
                continue

            # Extract durations
            view_sig_match = re.search(r"View_Signature_Collection_Duration:\s*([\d.]+)", line)
            if view_sig_match:
                metrics_data[current_replica]['view_sig_dur'] = float(view_sig_match.group(1))
                
            view_bft_match = re.search(r"View_Consensus_Latency:\s*([\d.]+)", line)
            if view_bft_match:
                metrics_data[current_replica]['view_bft_dur'] = float(view_bft_match.group(1))

            order_sig_match = re.search(r"Order_Signature_Collection_Duration:\s*([\d.]+)", line)
            if order_sig_match:
                metrics_data[current_replica]['order_sig_dur'] = float(order_sig_match.group(1))

            order_bft_match = re.search(r"Order_Consensus_Latency:\s*([\d.]+)", line)
            if order_bft_match:
                metrics_data[current_replica]['order_bft_dur'] = float(order_bft_match.group(1))

    # Apply defaults for missing pre-filter logs (Phase 1a and 2a)
    for rep, data in metrics_data.items():
        if data['view_sig_dur'] is None:
            data['view_sig_dur'] = 0.1
        if data['order_sig_dur'] is None:
            data['order_sig_dur'] = 0.1

    return metrics_data

def generate_stacked_bar_chart(metrics_data):
    """Generates a stacked bar chart from the parsed metrics."""
    if not metrics_data:
        print("No metrics found to plot!")
        return

    replicas = list(metrics_data.keys())
    
    # Extract data arrays for the plot
    phase_1a = [metrics_data[r]['view_sig_dur'] for r in replicas]
    phase_1b = [metrics_data[r]['view_bft_dur'] for r in replicas]
    phase_2a = [metrics_data[r]['order_sig_dur'] for r in replicas]
    phase_2b = [metrics_data[r]['order_bft_dur'] for r in replicas]

    # Set up the stack bases
    bottom_1b = phase_1a
    bottom_2a = [phase_1a[i] + phase_1b[i] for i in range(len(replicas))]
    bottom_2b = [bottom_2a[i] + phase_2a[i] for i in range(len(replicas))]

    # Create the plot
    fig, ax = plt.subplots(figsize=(10, 6))

    width = 0.6
    
    # We use cool colors to represent View (Phase 1) and warm colors for Order (Phase 2)
    p1 = ax.bar(replicas, phase_1a, width, label='Phase 1a: View Pre-Filter (f+1)', color='#87CEFA')
    p2 = ax.bar(replicas, phase_1b, width, bottom=bottom_1b, label='Phase 1b: View BFT Consensus', color='#4682B4')
    p3 = ax.bar(replicas, phase_2a, width, bottom=bottom_2a, label='Phase 2a: Order Pre-Filter (f+1)', color='#FFA07A')
    p4 = ax.bar(replicas, phase_2b, width, bottom=bottom_2b, label='Phase 2b: Order BFT Consensus', color='#CD5C5C')

    # Formatting
    ax.set_ylabel('Latency (Seconds)', fontsize=12)
    ax.set_xlabel('Replica ID', fontsize=12)
    ax.set_title('Intersection Consensus Protocol: Latency Breakdown per Replica', fontsize=14, fontweight='bold')
    ax.legend(loc='upper right')
    
    # Add a subtle grid behind the bars for readability
    ax.grid(axis='y', linestyle='--', alpha=0.7)
    
    plt.tight_layout()
    plt.show()

if __name__ == "__main__":
    # ---> SET YOUR LOG PATH HERE <---
    log_file_path = "2Phase8Honest/run1.log" 
    
    print(f"Parsing {log_file_path}...")
    data = parse_log_file(log_file_path)
    
    print(f"Found metrics for {len(data)} replicas. Generating plot...")
    generate_stacked_bar_chart(data)