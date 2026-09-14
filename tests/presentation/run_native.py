#!/usr/bin/env python3
"""Run only the standalone native demos; never launch a game or modify a prefix."""
from pathlib import Path
import json,os,subprocess,sys
from summarize_native import summarize
binary=Path(sys.argv[1]).resolve()
output=Path(sys.argv[2]).resolve();output.mkdir(parents=True,exist_ok=True)
env=os.environ.copy();env['MTL_HUD_ENABLED']='1';env.pop('DYLD_INSERT_LIBRARIES',None)
awake=subprocess.Popen(['caffeinate','-d','-u','-t','150'])
try:
    for name,rate,nth,sync in [('baseline',60,1,1),('candidate',120,2,0)]:
        csv=output/(name+'.csv')
        with (output/(name+'.log')).open('w') as log:
            subprocess.run([str(binary),str(csv),'1','0',str(rate),str(nth),str(sync),'1'],env=env,stdout=log,stderr=log,timeout=60,check=True)
        result=summarize(csv);(output/(name+'.json')).write_text(json.dumps(result,indent=2)+'\n')
        print(name,json.dumps(result),flush=True)
finally:
    awake.terminate();awake.wait()
