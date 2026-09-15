import subprocess, sys
from pathlib import Path
root=Path(__file__).resolve().parent
trace=root/(sys.argv[1] if len(sys.argv)>1 else 'prefetch-60-instruments.trace')
for schema in ['ca-client-present-request','ca-client-presented-handler','displayed-surfaces-interval','display-surface-swap','display-vsyncs-interval']:
    output=trace.with_name(trace.stem+'-'+schema+'.xml')
    subprocess.run(['xcrun','xctrace','export','--input',str(trace),'--xpath',
        '/trace-toc/run[@number="1"]/data/table[@schema="'+schema+'"]','--output',str(output)],check=True)
