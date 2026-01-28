import os
import re
import csv
import sys
import pandas as pd
import matplotlib.pyplot as plt
# Regex patterns matching your C++ logging EXACTLY
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

CAR_SPEED = 14.0          # m/s
INTERSECTION_WIDTH = 15.0 # meters
CLEARANCE_TIME = INTERSECTION_WIDTH / CAR_SPEED

def parse_log_file(filename):
    # Dictionary to store data per vehicle (Replica ID)
    # Format: { "0": { "sent": 0, "recv": 0, ... }, "1": ... }
    vehicles = {}
    
    # Global stats for this run
    run_successes = 0
    run_failures = 0

    with open(filename, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            line = line.strip()
            
            # Helper to ensure vehicle dict exists
            def get_veh(rid):
                if rid not in vehicles: vehicles[rid] = {}
                return vehicles[rid]

            # 1. Messages Received
            m = patterns["msg_recv"].search(line)
            if m: get_veh(m.group(1))["recv"] = int(m.group(2))

            # 2. Messages Sent
            m = patterns["msg_sent"].search(line)
            if m: get_veh(m.group(1))["sent"] = int(m.group(2))

            # 3. Consensus Latency
            m = patterns["latency"].search(line)
            if m: get_veh(m.group(1))["latency"] = float(m.group(2))

            # 4. Wait Time
            m = patterns["wait_time"].search(line)
            if m: get_veh(m.group(1))["wait_time"] = float(m.group(2))
            
            # 5. Stop Time (Arrival)
            m = patterns["stop_time"].search(line)
            if m: get_veh(m.group(1))["stop_time"] = float(m.group(2))

            # 6. Resume Time (Departure)
            m = patterns["resume_time"].search(line)
            if m: get_veh(m.group(1))["resume_time"] = float(m.group(2))

            # 7. Consensus Success Count (Global for run)
            if patterns["success"].search(line):
                run_successes += 1

            # 8. Consensus Failure Count (Global for run)
            if patterns["failure"].search(line):
                run_failures += 1
                # Mark vehicle as failed for this run
                m_fail = patterns["failure"].search(line) 
                if m_fail: get_veh(m_fail.group(1))["status"] = "FALLBACK"

    return vehicles, run_successes, run_failures

def generate_plots(csv_file):
    print("Generating plots...")
    
    # Load the data
    try:
        df = pd.read_csv(csv_file, na_values=['', ' ', 'nan', 'NaN', 'None'])
    except pd.errors.EmptyDataError:
        print("CSV is empty. Cannot generate plots.")
        return

    # Ensure numeric columns
    numeric_cols = ['Msgs_Sent', 'Msgs_Recv', 'Consensus_Latency_s', 'Total_Wait_Time_s', 'Stop_Time_s', 'Resume_Time_s']
    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors='coerce')

    # Extract Run Number for sorting (assuming format 'run_X.log')
    try:
        df['Run_Num'] = df['Run_ID'].apply(lambda x: int(re.search(r'(\d+)', str(x)).group(1)) if re.search(r'(\d+)', str(x)) else 0)
    except:
        df['Run_Num'] = df.index 

    df = df.sort_values('Run_Num')
    
    # --- Aggregations per Run ---
    runs = df['Run_Num'].unique()
    agg_data = []

    for run in runs:
        run_df = df[df['Run_Num'] == run]
        
        # 1. Throughput Calculation
        # First Arrival = Min Stop Time (filter out NaN values)
        stop_times = run_df['Stop_Time_s'].dropna()
        resume_times = run_df['Resume_Time_s'].dropna()
        
        throughput_vpm = 0
        if len(stop_times) > 0 and len(resume_times) > 0:
            first_arrival = stop_times.min()
            last_resume = resume_times.max()
            last_exit = last_resume + CLEARANCE_TIME
            duration_sec = last_exit - first_arrival
            if duration_sec > 0:
                num_cars = len(run_df) 
                throughput_vpm = (num_cars / duration_sec) * 60
        
        # 2. Messages
        total_msgs_sent = run_df['Msgs_Sent'].sum()
        
        # 3. Wait Time
        avg_wait_time = run_df['Total_Wait_Time_s'].mean()
        
        # 4. Consensus Latency
        avg_latency = run_df['Consensus_Latency_s'].mean()
        
        agg_data.append({
            'Run_Num': run,
            'Throughput_VPM': throughput_vpm,
            'Total_Msgs_Sent': total_msgs_sent,
            'Avg_Wait_Time_s': avg_wait_time,
            'Avg_Latency_s': avg_latency
        })

    agg_df = pd.DataFrame(agg_data)
    
    # --- Calculate per-replica message averages ---
    replica_agg = df.groupby('Replica_ID').agg({
        'Msgs_Sent': 'mean',
        'Msgs_Recv': 'mean'
    }).reset_index()
    replica_agg = replica_agg.sort_values('Replica_ID')
    
    # Print calculated stats to console
    print("\n=== Calculated Metrics per Run ===")
    print(agg_df.to_string())
    print("\n=== Average Messages per Replica (across all runs) ===")
    print(replica_agg.to_string(index=False))

    # --- Plotting ---
    if agg_df.empty:
        print("No aggregate data to plot.")
        return

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle('Performance Metrics Summary', fontsize=16)

    # Plot 1: Consensus Latency
    axes[0, 0].plot(agg_df['Run_Num'], agg_df['Avg_Latency_s'], marker='o', linestyle='-', color='b')
    axes[0, 0].set_title('Avg Consensus Latency per Run')
    axes[0, 0].set_xlabel('Run Number')
    axes[0, 0].set_ylabel('Seconds')
    axes[0, 0].grid(True)

    # Plot 2: Throughput
    axes[0, 1].bar(agg_df['Run_Num'], agg_df['Throughput_VPM'], color='g', alpha=0.7)
    axes[0, 1].set_title('Estimated Throughput')
    axes[0, 1].set_xlabel('Run Number')
    axes[0, 1].set_ylabel('Vehicles Per Minute (VPM)')
    axes[0, 1].grid(axis='y')

    # Plot 3: Average Wait Time
    axes[1, 0].bar(agg_df['Run_Num'], agg_df['Avg_Wait_Time_s'], color='orange', alpha=0.7)
    axes[1, 0].set_title('Avg Total Wait Time per Vehicle')
    axes[1, 0].set_xlabel('Run Number')
    axes[1, 0].set_ylabel('Seconds')
    axes[1, 0].grid(axis='y')

    # Plot 4: Average Messages per Replica (grouped bar chart)
    x = range(len(replica_agg))
    width = 0.35
    axes[1, 1].bar([i - width/2 for i in x], replica_agg['Msgs_Sent'], width, 
                    label='Sent', color='purple', alpha=0.7)
    axes[1, 1].bar([i + width/2 for i in x], replica_agg['Msgs_Recv'], width,
                    label='Received', color='teal', alpha=0.7)
    axes[1, 1].set_title('Average Messages per Replica (across all runs)')
    axes[1, 1].set_xlabel('Replica ID')
    axes[1, 1].set_ylabel('Average Count')
    axes[1, 1].set_xticks(x)
    axes[1, 1].set_xticklabels(replica_agg['Replica_ID'])
    axes[1, 1].legend()
    axes[1, 1].grid(axis='y')

    plt.tight_layout(rect=[0, 0.03, 1, 0.95])
    output_png = 'performance_metrics_summaryv2.png'
    plt.savefig(output_png)
    print(f"\nPlots saved to {output_png}")
#use for throughput calculation

def main():
    # Folder containing your log files (e.g., 'results' or '.')
    log_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    
    output_file = "metrics_summaryv2.csv"
    
    print(f"Scanning '{log_dir}' for .log files...")
    
    with open(output_file, 'w', newline='') as csvfile:
        fieldnames = [
            'Run_ID', 'Replica_ID', 'Status', 
            'Msgs_Sent', 'Msgs_Recv', 
            'Consensus_Latency_s', 'Total_Wait_Time_s', 
            'Stop_Time_s', 'Resume_Time_s',
            'Run_Total_Successes', 'Run_Total_Failures'
        ]
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()

        files = [f for f in os.listdir(log_dir) if f.endswith(".log") or f.endswith(".txt")]
        # Sort numerically by extracting the run number from filename (e.g., "run_10.log" -> 10)
        def get_run_number(filename):
            match = re.search(r'run_(\d+)\.log', filename)
            return int(match.group(1)) if match else 0
        files.sort(key=get_run_number)

        if not files:
            print("No log files found! Make sure they end in .log or .txt")
            return

        for filename in files:
            path = os.path.join(log_dir, filename)
            print(f"Processing {filename}...")
            
            vehicles, successes, failures = parse_log_file(path)
            
            for rid, data in vehicles.items():
                row = {
                    'Run_ID': filename,
                    'Replica_ID': rid,
                    'Status': data.get("status", "SUCCESS"),
                    'Msgs_Sent': data.get("sent", 0),
                    'Msgs_Recv': data.get("recv", 0),
                    'Consensus_Latency_s': data.get("latency", ""),
                    'Total_Wait_Time_s': data.get("wait_time", ""),
                    'Stop_Time_s': data.get("stop_time", ""),
                    'Resume_Time_s': data.get("resume_time", ""),
                    'Run_Total_Successes': successes,
                    'Run_Total_Failures': failures
                }
                writer.writerow(row)

    print(f"Done! Results written to {output_file}")
    generate_plots(output_file)

if __name__ == "__main__":
    main()