import sys, os
sys.path.append('.')
import analyze_two_phase_v2 as analyzer

dirs = ['2Phase8Honest', '2phase12Honest', '2phase16Honest']
for d in dirs:
    dp = os.path.join(analyzer.BENCHMARK_DIR, d)
    if os.path.exists(dp):
        res = analyzer.process_directory(dp)
        if res:
            print(f"--- N={d} ---")
            for i, r in enumerate(res):
                print(f" Round {i+1}: View={r['avg_view']:.3f}s, Sent={r['avg_sent']:.1f}, Recv={r['avg_recv']:.1f}")
