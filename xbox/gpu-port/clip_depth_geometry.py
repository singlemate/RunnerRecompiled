"""CPU D3D homogeneous clipping for captured float3 quad; diagnostic only."""
import json,re,struct,sys
from pathlib import Path
s=Path(sys.argv[1]).read_text();a=json.loads(Path(sys.argv[2]).read_text(encoding='utf-8-sig'))
mem={int(x,16):int(y,16) for x,y in re.findall(r'^mem=(\w+) raw=(\w+)$',s,re.M)}
base=int(re.search(r'processed type=1 base=(\w+) format=0 count=6',s)[1],16)
raw=b''.join(mem[base+i*4].to_bytes(4,'little') for i in range(3));indices=list(struct.unpack('>6H',raw))
vertices=[x['clip'] for x in a['vertices']]
planes=[lambda v:v[3]+v[0],lambda v:v[3]-v[0],lambda v:v[3]+v[1],lambda v:v[3]-v[1],lambda v:v[2],lambda v:v[3]-v[2]]
polys=[]
for t in range(0,6,3):
 poly=[vertices[i] for i in indices[t:t+3]]
 for plane in planes:
  out=[]
  for j,b in enumerate(poly):
   prev=poly[j-1];da,db=plane(prev),plane(b)
   if (da>=0)!=(db>=0):
    u=da/(da-db);out.append([x+(y-x)*u for x,y in zip(prev,b)])
   if db>=0:out.append(b)
  poly=out
  if not poly:break
 screen=[[640+v[0]/v[3]*640,360-v[1]/v[3]*360,1-v[2]/v[3]] for v in poly]
 def inside(x,y):
  signs=[(screen[(i+1)%len(screen)][0]-v[0])*(y-v[1])-(screen[(i+1)%len(screen)][1]-v[1])*(x-v[0]) for i,v in enumerate(screen)]
  return bool(signs) and (min(signs)>=-1e-7 or max(signs)<=1e-7)
 polys.append(dict(indices=indices[t:t+3],screen_polygon=screen,target_center_covered=inside(128.5,468.5),control_center_covered=inside(640.5,180.5)))
print(json.dumps(dict(indices=indices,triangles=polys,limitations='CPU doubles; no culling/scissor/MSAA/host shader fixups; center coverage only'),indent=2))
