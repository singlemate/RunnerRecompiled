import re,struct,json,sys,collections
from pathlib import Path
s=Path(sys.argv[1]).read_text(errors='replace')
rows=re.findall(r'external_host_color sample=(\d+) frame=(\d+) point=0 lane=(\d+) host_id=\d+ rgba=([^\r\n]+)',s)
result=[]
for sample,frame,lane,rgba in rows:
 v=[float(x) for x in rgba.split(',')]
 if all(0<=x<=255 and x==int(x) for x in v) and any(v):
  result.append(dict(sample=int(sample),frame=int(frame),lane=int(lane),bytes=[int(x) for x in v],depth=struct.unpack('<f',bytes(int(x) for x in v))[0]))
print(json.dumps(dict(mode_markers=s.count('external_incoming_depth_compare_always'),exit=re.findall(r'probe_exit=\d+',s),decoded=result,limitations='Byte-shaped colors alone do not prove a new pixel write; inspect shader and geometry context. CompareAlways can expose legitimately occluded surfaces.'),indent=2))
