import re,json,sys,collections
from pathlib import Path
s=Path(sys.argv[1]).read_text(errors='replace')
rows=[dict(re.findall(r'(\w+)=([^ ]+)',line)) for line in s.splitlines() if 'external_geometry_pair ' in line]
keys=['frame','count','ib','dma','offset','position','c0_8']
by=collections.defaultdict(list)
for r in rows:by[tuple(r[k] for k in keys)].append(r)
pairs=[];missing=[]
for r in rows:
 if r['vs']!='BF8AD3FD7A465985':continue
 matches=[x for x in by[tuple(r[k] for k in keys)] if x['ps']!='0000000000000000']
 entry=dict(depth_sequence=r['sequence'],frame=r['frame'],count=r['count'],position=r['position'],colors=[dict(sequence=x['sequence'],vs=x['vs'],ps=x['ps']) for x in matches])
 (pairs if matches else missing).append(entry)
print(json.dumps(dict(records=len(rows),exact_geometry_pairs=pairs,depth_without_exact_color_match=missing,exit=re.findall(r'probe_exit=\d+',s)),indent=2))
