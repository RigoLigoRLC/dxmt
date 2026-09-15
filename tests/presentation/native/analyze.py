import csv, json, statistics, sys
from pathlib import Path

def quantiles(values):
    values=sorted(values)
    if not values: return None
    def q(p): return values[min(len(values)-1, int(p*(len(values)-1)))]
    return dict(mean=statistics.mean(values), p01=q(.01), median=q(.5), p95=q(.95), p99=q(.99), maximum=values[-1])

for arg in sys.argv[1:]:
    path=Path(arg)
    rows=[{k:float(v) for k,v in row.items()} for row in csv.DictReader(path.open())]
    meta=json.loads(path.with_name(path.stem+'-meta.json').read_text())
    start=rows[0]['input_s']
    warm=[r for r in rows if r['input_s']>=start+3]
    shown=[r for r in warm if r['displayed_s']>0]
    displayed=sorted(r['displayed_s'] for r in shown)
    intervals=[1000*(b-a) for a,b in zip(displayed,displayed[1:])]
    result={'name':path.stem,'submitted':len(rows),'warm_submitted':len(warm),'warm_displayed':len(shown),
        'missing_callbacks':sum(r['callback_s']==0 for r in rows),'zero_presented_timestamps':sum(r['callback_s']>0 and r['displayed_s']==0 for r in rows),
        'gpu_not_completed':sum(r['gpu_status']!=4 for r in rows),
        'display_before_gpu_end':sum(r['displayed_s']>0 and r['displayed_s']<r['gpu_end_s'] for r in rows),
        'duplicate_display_times':len(displayed)-len(set(displayed)),
        'display_order_errors':sum(b['displayed_s']<=a['displayed_s'] for a,b in zip(shown,shown[1:])),
        'fps':(len(displayed)-1)/(displayed[-1]-displayed[0]),
        'display_interval_ms':quantiles(intervals), 'meta':meta,
        'measured_intervals':len(intervals),
        'intervals_within_0_2ms_of_60fps':sum(abs(v-1000/60)<.2 for v in intervals)}
    for label,end,beg in [('input_to_display_ms','displayed_s','input_s'),('present_delay_ms','displayed_s','present_call_s'),
        ('request_to_display_ms','displayed_s','request_s'),('gpu_ms','gpu_end_s','gpu_start_s'),
        ('gpu_end_to_display_ms','displayed_s','gpu_end_s'),('callback_after_display_ms','callback_s','displayed_s'),
        ('drawable_wait_ms','acquire_end_s','acquire_start_s'),('input_to_commit_ms','commit_s','input_s'),
        ('scheduled_after_commit_ms','scheduled_s','commit_s')]:
        result[label]=quantiles([1000*(r[end]-r[beg]) for r in shown if r[end]>0 and r[beg]>0])
    # A zero timestamp is unknown/dropped, not a frame outstanding forever.
    # Retire it at callback arrival and report these separately above.
    result['older_frames_unshown_at_input']=quantiles([sum(p['input_s']<r['input_s'] and
        (p['displayed_s']>r['input_s'] or (p['displayed_s']==0 and (p['callback_s']==0 or p['callback_s']>r['input_s']))) for p in rows) for r in shown])
    linked=[r for r in shown if r.get('link_sequence',0)>0]
    result['missed_link_updates']=sum(max(0,b['link_sequence']-a['link_sequence']-1) for a,b in zip(linked,linked[1:]))
    path.with_name(path.stem+'-summary.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:(v.get('median') if isinstance(v,dict) and 'median' in v else v) for k,v in result.items() if k!='meta'},indent=2))
