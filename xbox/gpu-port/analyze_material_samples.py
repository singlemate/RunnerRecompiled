from pathlib import Path
import re,json,sys,collections,math
s=Path(sys.argv[1]).read_text(errors='replace');rows=re.findall(r'external_host_color sample=(\d+) frame=(\d+) point=(\d+) lane=(\d+) host_id=\d+ rgba=([^\r\n]+)',s)
a={}
for point in ['0','1']:
 v=[tuple(float(y) for y in x[4].split(',')) for x in rows if x[2]==point];valid=[x for x in v if x!=(511.,511.,511.,511.)]
 a[point]=dict(records=len(v),unwritten=len(v)-len(valid),nonfinite=sum(not all(math.isfinite(y) for y in x) for x in valid),values=[dict(rgba=list(x),count=n) for x,n in collections.Counter(valid).most_common(20)])
print(json.dumps(dict(exit=re.findall(r'probe_exit=\d+',s),points=a),indent=2))
