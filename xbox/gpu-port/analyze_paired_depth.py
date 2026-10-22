import re,struct,json,sys
from pathlib import Path
s=Path(sys.argv[1]).read_text(errors='replace');meta=None;data={}
for line in s.splitlines():
 if 'external_paired_depth ' in line:
  meta=dict(re.findall(r'(\w+)=([^ ]+)',line));continue
 m=re.search(r'external_host_color sample=(\d+) frame=(\d+) point=(\d+) lane=(\d+) host_id=\d+ rgba=([^ ]+)',line)
 if not m or not meta:continue
 sample,frame,point,lane,rgba=m.groups()
 if sample!=meta['sample'] or frame!=meta['frame']:continue
 key=(sample,frame,point,lane,meta['ib'],meta['count']);entry=data.setdefault(key,{})
 v=[float(x) for x in rgba.split(',')]
 if meta['kind']=='incoming':
  if all(0<=x<=255 and x==int(x) for x in v) and any(v):entry['incoming']=struct.unpack('<f',bytes(int(x) for x in v))[0]
  else:entry['unwritten_or_invalid']=v
 else:entry['stored']=v[0]
rows=[]
for k,v in data.items():
 if 'incoming' in v and 'stored' in v:
  v.update(sample=int(k[0]),frame=int(k[1]),point=int(k[2]),lane=int(k[3]),ib=k[4],indices=int(k[5]));v['delta']=v['incoming']-v['stored'];v['passes_greater_equal']=v['incoming']>=v['stored'];rows.append(v)
print(json.dumps(dict(exit=re.findall(r'probe_exit=\d+',s),pair_metadata=s.count('external_paired_depth'),valid_pairs=rows,unwritten_or_invalid=sum('unwritten_or_invalid' in x for x in data.values()),limitations='CompareAlways may reveal genuinely occluded geometry; differing depths alone do not prove incorrect rejection.'),indent=2))
