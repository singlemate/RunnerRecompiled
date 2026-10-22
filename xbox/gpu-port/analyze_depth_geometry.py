"""Evaluate captured BF8 shader ALU swizzles; excludes host viewport fixups."""
import re,struct,json,sys
from pathlib import Path
s=Path(sys.argv[1]).read_text()
f=lambda x:struct.unpack('>f',int(x,16).to_bytes(4,'big'))[0]
c={int(i):[f(x) for x in v.split()] for i,v in re.findall(r'^vc(\d+) ([0-9A-F ]+)$',s,re.M)}
m={int(a,16):int(v,16) for a,v in re.findall(r'^mem=(\w+) raw=(\w+)$',s,re.M)}
regs={int(a,16):int(v,16) for a,v in re.findall(r'^reg=(\w+) value=(\w+)$',s,re.M)}
fetch=re.search(r'fetch=95 raw=(\w+),(\w+)',s);base=int(fetch[1],16)&~3
endian=int(fetch[2],16)&3
assert endian==2,'Only captured 8-in-32 vertex endian supported'
result=[]
for i in range(4):
 xyz=[struct.unpack('>f',m[base+i*12+j*4].to_bytes(4,'little'))[0] for j in range(3)]
 r={'r1':xyz+[1.]}
 def get(t):
  a,_,sw=t.partition('.');v=c[int(a[1:])] if a.startswith('c') else r[a]
  return [v['xyzw'.index(x)] for x in sw] if sw else v[:]
 for op,dst,args in [('mul','r0',['r1.wwww','c8.xwzy']),('mad','r0',['r1.zzzz','c7.xwzy','r0']),('mad','r0',['r1.yyyy','c6.xzyw','r0.xzwy']),('mad','r1',['r1.xxxx','c5.xywz','r0.xzwy']),('mul','r0',['r1.zzzz','c3.xwzy']),('mad','r0',['r1.wwww','c2.xwzy','r0']),('mad','r0',['r1.yyyy','c1.xzyw','r0.xzwy']),('mad','pos',['r1.xxxx','c0','r0.xzyw'])]:
  v=[get(x) for x in args];r[dst]=[v[0][k]*v[1][k]+(v[2][k] if op=='mad' else 0.) for k in range(4)]
 pos=r['pos'];ndc=[x/pos[3] for x in pos[:3]]
 vp=[f(f'{regs[x]:08X}') for x in range(0x210f,0x2115)]
 screen=[ndc[k]*vp[k*2]+vp[k*2+1] for k in range(3)]
 result.append(dict(vertex=i,input=xyz,clip=pos,ndc=ndc,viewport=screen))
print(json.dumps(dict(vertex_endian=endian,clip_control=hex(regs[0x2204]),vte_control=hex(regs[0x2206]),vertices=result),indent=2))
