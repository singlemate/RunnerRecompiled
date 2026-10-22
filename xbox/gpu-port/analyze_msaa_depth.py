from pathlib import Path
import re,json,sys,collections
s=Path(sys.argv[1]).read_text(errors='replace')
a=re.findall(r'external_host_color sample=(\d+) frame=(\d+) point=(\d+) lane=(\d+) host_id=(\d+) rgba=([^\r\n]+)',s)
context={x:(f,v,p) for x,f,v,p in re.findall(r'external_host_draw_context sample=(\d+) frame=(\d+) vs=(\w+) ps=(\w+) depth=\w+',s)}
result={}
for point in ['0','1']:
 for lane in ['0','1']:
  values=[float(x[5].split(',')[0]) for x in a if x[2]==point and x[3]==lane]
  result[point+':'+lane]=dict(count=len(values),min=min(values) if values else None,max=max(values) if values else None,values=dict(collections.Counter(values)))
print(json.dumps(dict(source_markers=s.count('external_host_depth_source'),exit=re.findall(r'probe_exit=\d+',s),points=result,first_records=[dict(sample=x[0],context=context.get(x[0]),point=x[2],lane=x[3],depth=x[5].split(',')[0]) for x in a[:12]]),indent=2))
