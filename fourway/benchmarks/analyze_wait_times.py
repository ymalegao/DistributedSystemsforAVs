import os
import re
import matplotlib.pyplot as plt
import seaborn as sns

# 1. Define your folder paths
base_dir = "/home/yash/omnetpp/fourway/benchmarks"
configs = {
    "4 Vehicles": os.path.join(base_dir, "2phase4Honest"),
    "8 Vehicles": os.path.join(base_dir, "2phase8Honest"),
    "12 Vehicles": os.path.join(base_dir, "2phase12Honest"),
    "16 Vehicles": os.path.join(base_dir, "2phase16Honest")
}

# 2. Regex to catch per-epoch average wait time written by analyze_log.py --save-to.
# Example line:
#   [ROUND-METRICS] Epoch 2 Avg_Total_Consensus_Duration: 7.150000 seconds (goCars=4)
duration_regex = re.compile(
    r"\[ROUND-METRICS\]\s+Epoch\s+\d+\s+Avg_Total_Consensus_Duration:\s*([\d.]+)\s+seconds"
)

# 3. Parse the data
plot_data = {}

for title, folder in configs.items():
    times = []
    if os.path.exists(folder):
        print(f"Parsing {title} from {folder}...")
        for filename in os.listdir(folder):
            filepath = os.path.join(folder, filename)
            # Make sure we only read files, not subdirectories
            if os.path.isfile(filepath):
                with open(filepath, 'r', errors='ignore') as f:
                    for line in f:
                        match = duration_regex.search(line)
                        if match:
                            times.append(float(match.group(1)))
    else:
        print(f"WARNING: Folder not found: {folder}")
        
    plot_data[title] = times
    print(f"  -> Found {len(times)} data points for {title}")

# 4. Generate the Academic eCDF Plot
# We use a 1x4 grid since you have 4, 8, 12, and 16 vehicle configs
sns.set_theme(style="white") # Clean white background
fig, axes = plt.subplots(1, 4, figsize=(20, 5), sharey=True)
fig.suptitle('CDF of Total Wait Time per Vehicle (BFT-SMaRt coordinated)\nTime from vehicle first stopping to crossing the intersection', 
             fontsize=16, fontweight='bold', y=1.05)

# Setup the 4 panels
for idx, (title, times) in enumerate(plot_data.items()):
    ax = axes[idx]
    
    # Add light grid
    ax.grid(True, linestyle='-', alpha=0.4, color='lightgray')
    
    if len(times) > 0:
        # Plot the CDF step-graph (Matching your image's purple style)
        sns.ecdfplot(data=times, ax=ax, color='#9b59b6', linewidth=2.5, 
                     label=f'WAVE (IEEE 802.11p) (n={len(times)})')
        
        # Format the axes
        ax.set_title(title, fontweight='bold', fontsize=14)
        ax.set_xlabel('Total Wait Time (s)', fontsize=12)
        ax.legend(loc='lower right', fontsize=10)
    else:
        ax.set_title(title + " (NO DATA)", color='red')

# Set the shared Y-axis label and limits
axes[0].set_ylabel('CDF', fontsize=12)
axes[0].set_ylim(0, 1.05)

plt.tight_layout()
plt.show()