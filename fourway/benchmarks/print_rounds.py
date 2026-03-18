import os
import sys
sys.path.append('.')
import analyze_two_phase_v2 as analyzer

dirs = ['2Phase8Honest', '2phase12Honest', '2phase16Honest', '2phase4Honest']
for d in dirs:
    dp = os.path.join(analyzer.BENCHMARK_DIR, d)
    if os.path.exists(dp):
        res = analyzer.process_directory(dp)
        if res:
            print(f"--- N={d} ---")
            for i, r in enumerate(res):
                print(f" Round {i+1}: View={r['avg_view']:.3f}s, Order={r['avg_order']:.3f}s, Total={r['avg_total']:.3f}s, Throughput={r['throughput']:.3f}")
