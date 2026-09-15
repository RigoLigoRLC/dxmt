import argparse, os, subprocess, sys
from pathlib import Path

root=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description='Native display-driven pacing reference and independent-timer control')
parser.add_argument('mode',choices=['display60','producer60'],nargs='?',default='display60')
parser.add_argument('--seconds',type=float,default=14)
parser.add_argument('--name',default=None)
parser.add_argument('--output-dir',type=Path,default=root/'out')
parser.add_argument('--fixed-60hz',action='store_true',help='Temporarily select fixed physical 60 Hz, then restore the original mode')
args=parser.parse_args()
args.output_dir.mkdir(parents=True,exist_ok=True)
output=(args.output_dir/(args.name or args.mode)).resolve()
env=dict(os.environ,MTL_HUD_ENABLED='1',REFERENCE_PSYCH_WINDOW='1')
with output.with_suffix('.log').open('w') as log:
    result=subprocess.run(['caffeinate','-i',str(root/'out/Reference.app/Contents/MacOS/reference'),args.mode,str(output),str(args.seconds),'1',str(int(args.fixed_60hz))],env=env,stdout=log,stderr=log,timeout=args.seconds+20)
if result.returncode:
    print(output.with_suffix('.log').read_text(),file=sys.stderr)
    raise SystemExit(result.returncode)
subprocess.run([sys.executable,str(root/'analyze.py'),str(output)+'.csv'],check=True)
