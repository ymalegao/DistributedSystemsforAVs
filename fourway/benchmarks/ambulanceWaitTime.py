import os
import re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# --- CONFIGURATION ---
BASE_DIR = "/home/yash/DistributedSystemsforAVs/fourway/benchmarks"  # Change this to your base log directory
OUTPUT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIGS = ["NoAmbulanceHonest", "AmbulanceHonest", "NoAmbulanceByz", "AmbulanceByz"]
N_VALUES = [4, 8, 12, 16]

# --- REGEX PATTERNS ---
# Extracts: [RUN-METRICS] Wait_Intersection_Normal_Mean: 3.354545 seconds
mean_wait_regex = re.compile(r"\[RUN-METRICS\] Wait_Intersection_Normal_Mean:\s*([\d\.]+)")
ambulance_wait_regex = re.compile(r"\[RUN-METRICS\] Wait_Intersection_Ambulance_Mean:\s*([\d\.]+)")

# Extracts: [ROUND-METRICS] Epoch 0 Arrival_To_Resume_Time veh1: 2.100000 seconds
# Using this for the CDF to get a distribution of individual vehicle waits
indiv_wait_regex = re.compile(r"Arrival_To_Resume_Time veh\d+:\s*([\d\.]+)")

# --- DATA STRUCTURES ---
mean_waits = {config: [] for config in CONFIGS}
ambulance_waits = {config: [] for config in CONFIGS}
cdf_data_by_n = {n: {config: [] for config in CONFIGS} for n in N_VALUES}


def resolve_log_path(base_dir, n, config):
    """Find the correct log path for a given n/config despite naming variations."""
    priority_dir = os.path.join(base_dir, f"Priority{n}cars")
    if not os.path.isdir(priority_dir):
        return None

    candidates_by_config = {
        "NoAmbulanceHonest": [
            f"{n}veh_NoAmbulanceHonest.log",
            f"{n}veh_HonestNoAmbulance.log",
        ],
        "AmbulanceHonest": [
            f"{n}veh_AmbulanceHonest.log",
            f"{n}veh_HonestAmbulance.log",
        ],
        "NoAmbulanceByz": [
            f"{n}veh_NoAmbulanceByz.log",
            f"{n}veh_NoAmbulaceByz.log",  # observed typo in some logs
            f"{n}veh_ByzantineNoAmbulance.log",
        ],
        "AmbulanceByz": [
            f"{n}veh_AmbulanceByz.log",
        ],
    }

    for filename in candidates_by_config.get(config, []):
        candidate_path = os.path.join(priority_dir, filename)
        if os.path.exists(candidate_path):
            return candidate_path

    return None

# --- PARSING LOGIC ---
for config in CONFIGS:
    for n in N_VALUES:
        filepath = resolve_log_path(BASE_DIR, n, config)
        
        current_mean = None
        current_ambulance_mean = None
        individual_times = []
        
        if filepath and os.path.exists(filepath):
            with open(filepath, 'r') as file:
                for line in file:
                    # Check for mean wait
                    mean_match = mean_wait_regex.search(line)
                    if mean_match:
                        current_mean = float(mean_match.group(1))

                    # Check for ambulance mean wait
                    ambulance_match = ambulance_wait_regex.search(line)
                    if ambulance_match:
                        current_ambulance_mean = float(ambulance_match.group(1))
                    
                    # Check for individual vehicle wait
                    indiv_match = indiv_wait_regex.search(line)
                    if indiv_match:
                        individual_times.append(float(indiv_match.group(1)))
        else:
            print(f"Warning: File not found for n={n}, config={config}")
            
        # Append data for the line graph
        mean_waits[config].append(current_mean if current_mean is not None else np.nan)
        ambulance_waits[config].append(
            current_ambulance_mean if current_ambulance_mean is not None else np.nan
        )

        # Save individual data for the CDF panel plot
        cdf_data_by_n[n][config] = individual_times

# --- PLOTTING LOGIC ---

# Setup plot styling
colors = ['blue', 'green', 'red', 'purple']
markers = ['o', 's', '^', 'D']
linestyles = ['-', '--', '-.', ':']

# 1. Line Graph: Mean Wait Time vs Vehicle Count
plt.figure(figsize=(8, 6))
for idx, config in enumerate(CONFIGS):
    plt.plot(N_VALUES, mean_waits[config], marker=markers[idx], 
             color=colors[idx], linestyle=linestyles[idx], 
             linewidth=2, markersize=8, label=config)

plt.title('Mean Normal Vehicle Wait Time vs Total Vehicles', fontsize=14)
plt.xlabel('Number of Vehicles (n)', fontsize=12)
plt.ylabel('Mean Wait Time (seconds)', fontsize=12)
plt.xticks(N_VALUES)
plt.grid(True, linestyle='--', alpha=0.7)
plt.legend(fontsize=10)
plt.tight_layout()
mean_wait_output = os.path.join(OUTPUT_DIR, 'mean_wait_time_comparison.pdf')
mean_wait_output_png = os.path.join(OUTPUT_DIR, 'mean_wait_time_comparison.png')
plt.savefig(mean_wait_output)  # Saves a high-res PDF for papers
plt.savefig(mean_wait_output_png, dpi=300)  # PNG for easy preview in editors
plt.close()

# 2. Multi-panel CDF Graph: Individual per-vehicle waits for N = 4, 8, 12, 16
fig, axes = plt.subplots(2, 2, figsize=(12, 10), sharex=False, sharey=True)
axes = axes.flatten()

for ax_idx, n in enumerate(N_VALUES):
    ax = axes[ax_idx]
    for idx, config in enumerate(CONFIGS):
        data = cdf_data_by_n[n][config]
        if not data:
            continue

        data_sorted = np.sort(data)
        if len(data) == 1:
            p = np.array([1.0])
        else:
            p = 1.0 * np.arange(len(data)) / (len(data) - 1)

        ax.step(
            data_sorted,
            p,
            color=colors[idx],
            linestyle=linestyles[idx],
            linewidth=2,
            where="post",
            label=config,
        )

    ax.set_title(f'n={n}', fontsize=12)
    ax.set_xlabel('Wait Time (seconds)', fontsize=10)
    ax.set_ylabel('Cumulative Probability', fontsize=10)
    ax.grid(True, linestyle='--', alpha=0.7)
    ax.legend(fontsize=8)

fig.suptitle(
    'Empirical CDF from Individual Vehicle Waits (Arrival_To_Resume_Time)',
    fontsize=14,
)
fig.tight_layout(rect=[0, 0, 1, 0.97])
cdf_output = os.path.join(OUTPUT_DIR, 'wait_time_cdf_all_n.pdf')
cdf_output_png = os.path.join(OUTPUT_DIR, 'wait_time_cdf_all_n.png')
fig.savefig(cdf_output)  # Saves a high-res PDF for papers
fig.savefig(cdf_output_png, dpi=300)  # PNG for easy preview in editors
plt.close()

# 3. Multi-panel bar graph styled like your reference (one panel per n)
fig, axes = plt.subplots(2, 2, figsize=(12, 8), sharey=True)
axes = axes.flatten()
group_labels = ['Honest', 'Byz']
x = np.arange(len(group_labels))
width = 0.32

for ax_idx, n in enumerate(N_VALUES):
    ax = axes[ax_idx]
    n_idx = N_VALUES.index(n)

    normal_vals = np.array(
        [mean_waits["AmbulanceHonest"][n_idx], mean_waits["AmbulanceByz"][n_idx]],
        dtype=float,
    )
    ambulance_vals = np.array(
        [ambulance_waits["AmbulanceHonest"][n_idx], ambulance_waits["AmbulanceByz"][n_idx]],
        dtype=float,
    )

    normal_bars = ax.bar(
        x + width / 2,
        normal_vals,
        width,
        label='Normal vehicles' if ax_idx == 0 else None,
        color='#95A5A6',
        edgecolor='#34495E',
    )
    ambulance_bars = ax.bar(
        x - width / 2,
        ambulance_vals,
        width,
        label='Ambulance' if ax_idx == 0 else None,
        color='#E74C3C',
        edgecolor='#7B241C',
    )

    # Hatch Byzantine bars to visually separate Honest vs Byz, similar to reference styling.
    normal_bars[1].set_hatch('///')
    ambulance_bars[1].set_hatch('///')

    ax.set_title(f'{n} Vehicles', fontsize=12, fontweight='bold')
    ax.set_xticks(x)
    ax.set_xticklabels(group_labels)
    ax.grid(True, axis='y', linestyle='--', alpha=0.35)
    if ax_idx % 2 == 0:
        ax.set_ylabel('Mean Wait Time (s)', fontsize=10)

fig.suptitle('Wait Time at Intersection: Ambulance vs Normal Vehicles', fontsize=14, fontweight='bold')
fig.legend(loc='upper center', ncol=2, bbox_to_anchor=(0.5, 0.96))
fig.tight_layout(rect=[0, 0, 1, 0.93])

bar_output = os.path.join(OUTPUT_DIR, 'ambulance_vs_normal_bar.pdf')
bar_output_png = os.path.join(OUTPUT_DIR, 'ambulance_vs_normal_bar.png')
fig.savefig(bar_output)
fig.savefig(bar_output_png, dpi=300)
plt.close(fig)

print("Plots generated successfully!")
print(f"Saved: {mean_wait_output}")
print(f"Saved: {cdf_output}")
print(f"Saved: {bar_output}")
print(f"Saved: {mean_wait_output_png}")
print(f"Saved: {cdf_output_png}")
print(f"Saved: {bar_output_png}")