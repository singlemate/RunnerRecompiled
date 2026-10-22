"""Compare source position ALU for same captured inputs, using double arithmetic."""
from pathlib import Path
import re,struct,json,sys
s=Path(sys.argv[1]).read_text();city=Path(sys.argv[2]).read_text()
f=lambda h:struct.unpack('>f',int(h,16).to_bytes(4,'big'))[0]
c={int(i):[f(x) for x in v.split()] for i,v in re.findall(r'^vc(\d+) ([0-9A-F ]+)$',s,re.M)}
mem={int(a,16):int(v,16) for a,v in re.findall(r'^mem=(\w+) raw=(\w+)$',s,re.M)}
fetch=re.search(r'fetch=95 raw=(\w+),(\w+)',s);base=int(fetch[1],16)&~3
assert int(fetch[2],16)&3==2

def evaluate(text,xyz,initial):
 r={initial:xyz+[1.]}
 def get(t):
  a,_,sw=t.partition('.');v=c[int(a[1:])] if a.startswith('c') else r[a]
  return [v['xyzw'.index(x)] for x in sw] if sw else v[:]
 for op,dst,args in re.findall(r'\*/\s+(mul|mad|max) (\w+), ([^\n]+)',text):
  v=[get(x.strip()) for x in args.split(',')]
  r[dst]=[max(v[0][k],v[1][k]) if op=='max' else v[0][k]*v[1][k]+(v[2][k] if op=='mad' else 0.) for k in range(4)]
  if dst=='oPos':return r[dst]
 raise ValueError('no position export')
rows=[]
for i in range(4):
 xyz=[struct.unpack('>f',mem[base+i*12+j*4].to_bytes(4,'little'))[0] for j in range(3)]
 a=evaluate(s,xyz,'r1');b=evaluate(city,xyz,'r3');rows.append(dict(vertex=i,depth=a,color=b,max_abs_difference=max(abs(x-y) for x,y in zip(a,b))))
print(json.dumps(dict(vertices=rows,limitations='Source ALU only; doubles; no translated SPIR-V, depth quantization or GPU execution'),indent=2))
