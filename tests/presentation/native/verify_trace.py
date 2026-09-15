import csv,json,statistics,sys
import xml.etree.ElementTree as E
from pathlib import Path
def quantiles(values):
    values=sorted(values)
    if not values:return None
    def q(p):return values[min(len(values)-1,int(p*(len(values)-1)))]
    return dict(mean=statistics.mean(values),p01=q(.01),median=q(.5),p95=q(.95),p99=q(.99),maximum=values[-1])

base=Path(sys.argv[1])
def table(schema):
    root=E.parse(str(base)+'-'+schema+'.xml').getroot()
    refs={e.attrib['id']:e for e in root.iter() if 'id' in e.attrib}
    def val(e):
        if 'ref' in e.attrib:e=refs[e.attrib['ref']]
        return e.text or e.attrib.get('fmt','')
    cols=[e.text for e in root.findall('.//schema/col/mnemonic')]
    return [dict(zip(cols,[val(e) for e in row])) for row in root.findall('.//row')]

frames=[{k:float(v) for k,v in row.items()} for row in csv.DictReader(open(str(base)+'.csv'))]
requests=table('ca-client-present-request')
surfaces=table('displayed-surfaces-interval')
swaps=table('display-surface-swap')
assert len(requests)==len(frames),(len(requests),len(frames))
offsets=[float(req['timestamp'])/1e9-frame['present_call_s'] for req,frame in zip(requests,frames)]
offset=statistics.median(offsets)
warm=[(req,frame) for req,frame in zip(requests,frames) if frame['input_s']>frames[0]['input_s']+3 and frame['displayed_s']>0]
matches=[]
for req,frame in warm:
    called=float(req['timestamp'])/1e9
    candidates=[s for s in surfaces if s['surface-id']==req['surface-id'] and float(s['start'])/1e9>=called]
    if not candidates:continue
    displayed=min(candidates,key=lambda s:float(s['start']))
    actual=float(displayed['start'])/1e9
    if actual-called>.2:continue
    matches.append({'frame':int(frame['frame']),'surface_id':req['surface-id'],
        'direct':displayed['direct-to-display']=='1',
        'trace_displayed_s':actual,
        'trace_present_delay_ms':1000*(actual-called),
        'trace_minus_csv_display_ms':1000*(actual-frame['displayed_s']-offset)})
result={'warm_frames':len(warm),'hardware_surface_matches':len(matches),'direct_matches':sum(m['direct'] for m in matches),
    'unmatched_frame_ids':[int(f['frame']) for _,f in warm if int(f['frame']) not in {m['frame'] for m in matches}],
    'request_clock_offset_s':offset,'request_alignment_error_ms':quantiles([1000*abs(x-offset) for x in offsets]),
    'hardware_present_delay_ms':quantiles([m['trace_present_delay_ms'] for m in matches]),
    'hardware_minus_csv_display_ms':quantiles([m['trace_minus_csv_display_ms'] for m in matches])}
hardware_times=[m['trace_displayed_s'] for m in matches]
hardware_intervals=[1000*(b-a) for a,b in zip(hardware_times,hardware_times[1:])]
result['hardware_display_interval_ms']=quantiles(hardware_intervals)
result['hardware_unique_display_events']=len(set(hardware_times))
result['hardware_intervals_within_0_2ms_of_60fps']=sum(abs(v-1000/60)<.2 for v in hardware_intervals)
result['hardware_fps']=(len(hardware_times)-1)/(hardware_times[-1]-hardware_times[0]) if len(hardware_times)>1 else None
# The final displayed surface can lack a closed interval when recording ends.
# Check discrete hardware swap events as well, without inferring a Direct flag.
swap_matches=[]
for req,frame in warm:
    called=float(req['timestamp'])/1e9
    candidates=[s for s in swaps if s['surface-id']==req['surface-id'] and float(s['timestamp'])/1e9>=called]
    if not candidates:continue
    swap=min(candidates,key=lambda s:float(s['timestamp']))
    actual=float(swap['timestamp'])/1e9
    if actual-called>.2:continue
    swap_matches.append({'frame':int(frame['frame']),'surface_id':req['surface-id'],
        'trace_displayed_s':actual,'trace_present_delay_ms':1000*(actual-called),
        'trace_minus_csv_display_ms':1000*(actual-frame['displayed_s']-offset)})
result['discrete_hardware_swap_matches']=len(swap_matches)
result['discrete_hardware_unique_events']=len({s['trace_displayed_s'] for s in swap_matches})
result['discrete_hardware_minus_csv_display_ms']=quantiles([s['trace_minus_csv_display_ms'] for s in swap_matches])
result['discrete_hardware_present_delay_ms']=quantiles([s['trace_present_delay_ms'] for s in swap_matches])
Path(str(base)+'-swap-matches.json').write_text(json.dumps(swap_matches,indent=2)+'\n')
Path(str(base)+'-verification.json').write_text(json.dumps(result,indent=2)+'\n')
Path(str(base)+'-surface-matches.json').write_text(json.dumps(matches,indent=2)+'\n')
print(json.dumps(result,indent=2))
