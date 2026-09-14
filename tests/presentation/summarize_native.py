#!/usr/bin/env python3
"""Summarize the native Metal timing demo, rejecting insufficient display data."""
import csv,json,statistics,sys
from pathlib import Path

def summarize(path):
    all_rows=list(csv.DictReader(Path(path).open()))
    shown=[r for r in all_rows if float(r['presented_s'])>0]
    if len(shown)<120:
        raise ValueError('Insufficient displayed frames: check display power and window visibility')
    start=float(shown[0]['frame_start_s'])
    rows=[r for r in shown if float(r['frame_start_s'])>=start+3]
    if len(rows)<120: raise ValueError('Insufficient frames after warmup')
    stamps=sorted(float(r['presented_s']) for r in rows)
    elapsed=stamps[-1]-stamps[0]
    if elapsed<8: raise ValueError('Less than eight seconds of usable display data')
    def delta(a,b):return statistics.mean((float(r[a])-float(r[b]))*1000 for r in rows)
    result=dict(frames=len(rows),measurement_seconds=elapsed,presentation_rate=(len(stamps)-1)/elapsed,
        present_delay_ms=delta('presented_s','request_s'),input_sample_to_display_ms=delta('presented_s','input_sample_s'),
        cpu_budget_ms=delta('commit_deadline_s','frame_start_s'),
        deadline_to_predicted_display_ms=delta('predicted_display_s','commit_deadline_s'),
        actual_minus_predicted_display_ms=delta('presented_s','predicted_display_s'),
        missed_cpu_deadlines=sum(float(r['request_s'])>float(r['commit_deadline_s']) for r in rows),
        dropped_including_startup_and_shutdown=len(all_rows)-len(shown))
    result['gpu_ms_by_workload']={}
    for work in sorted({int(r.get('workload_loops',0)) for r in rows}):
        times=sorted((float(r['gpu_end_s'])-float(r['gpu_start_s']))*1000 for r in rows if int(r.get('workload_loops',0))==work and float(r['gpu_end_s'])>0)
        if times:result['gpu_ms_by_workload'][work]={'mean':statistics.mean(times),'p95':times[int(.95*len(times))]}
    return result
if __name__=='__main__':
    print(json.dumps(summarize(sys.argv[1]),indent=2))
