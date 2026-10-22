import re,json,sys
from pathlib import Path
s=Path(sys.argv[1]).read_text(errors='replace');out=[]
for line in s.splitlines():
 if 'external_lightmap_bc1 ' not in line:continue
 d=dict(re.findall(r'(\w+)=([^ ]+)',line));b=bytearray.fromhex(d['bytes']);endian=int(d['endian'])
 if endian==1:
  for i in range(0,8,2):b[i:i+2]=b[i:i+2][::-1]
 elif endian==2:
  for i in range(0,8,4):b[i:i+4]=b[i:i+4][::-1]
 elif endian==3:
  for i in range(0,8,4):b[i:i+4]=b[i+2:i+4]+b[i:i+2]
 c0,c1=int.from_bytes(b[:2],'little'),int.from_bytes(b[2:4],'little');bits=int.from_bytes(b[4:],'little')
 rgb=lambda c:[((c>>11)&31)/31,((c>>5)&63)/63,(c&31)/31]
 a,z=rgb(c0),rgb(c1);palette=[a+[1.],z+[1.]]
 if c0>c1:palette += [[(2*x+y)/3 for x,y in zip(a,z)]+[1.],[(x+2*y)/3 for x,y in zip(a,z)]+[1.]]
 else:palette += [[(x+y)/2 for x,y in zip(a,z)]+[1.],[0.,0.,0.,0.]]
 pixels=[palette[(bits>>(i*2))&3] for i in range(16)]
 out.append(dict(slot=int(d['slot']),x=int(d['x']),y=int(d['y']),source=d['bytes'],endian=endian,all_source_zero=not any(b),first_texel=pixels[0],rgb_min=[min(p[k] for p in pixels) for k in range(3)],rgb_max=[max(p[k] for p in pixels) for k in range(3)],transparent_pixels=sum(p[3]==0 for p in pixels)))
print(json.dumps(dict(exit=re.findall(r'probe_exit=\d+',s),blocks=out,limitations='BC1 reference decode of selected source blocks only; does not validate sampled UV, mip choice or uploaded image.'),indent=2))
