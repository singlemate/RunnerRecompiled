from pathlib import Path
import struct,re,json,hashlib
r=Path.cwd();private=r/'work/xbox-game-private';g=private/'generated-fourth-final'
exports={}
for library,file in [('xboxkrnl.exe','xboxkrnl_table.inc'),('xam.xex','xam_table.inc')]:
 text=(r/'work/references/XenonRecomp/XenonUtils/xbox'/file).read_text()
 exports[library]={int(o,16):(name,kind) for o,name,kind in re.findall(r'XE_EXPORT\([^,]+,\s*(0x[0-9A-Fa-f]+),\s*(\w+),\s*(\w+)\)',text)}
xex=(private/'default.xex').read_bytes();n=struct.unpack_from('>I',xex,20)[0];opts=dict(struct.unpack_from('>II',xex,24+8*i) for i in range(n));off=opts[0x103ff];total,stringsize,count=struct.unpack_from('>III',xex,off);table=xex[off+12:off+12+stringsize];names=[];cur=0
for i in range(count):end=table.index(b'\0',cur);names.append(table[cur:end].decode());cur=(end+4)&~3
sections=[]
for p in sorted(private.glob('section.*.bin')):
 addr=int(p.name.split('.')[1]);data=p.read_bytes()
 if data and 0x82000000<=addr<0x83170000 and addr+len(data)<=0x83170000:sections.append((addr,data))
def readword(addr):
 for base,data in sections:
  if base<=addr<=base+len(data)-4:return struct.unpack_from('<I',data,addr-base)[0]
 raise ValueError(hex(addr))
guards=[];cur=12+stringsize
while cur<total:
 length=struct.unpack_from('>I',xex,off+cur)[0];index,records=struct.unpack_from('>HH',xex,off+cur+36);lib=names[index&255];assert 40+records*4<=length
 for i in range(records):
  addr=struct.unpack_from('>I',xex,off+cur+40+4*i)[0];value=readword(addr);ordinal=value&65535
  item=exports.get(lib,{}).get(ordinal)
  if value>>24==0 and item and item[1]=='kVariable':guards.append(dict(iat=addr,guard=0x50000000+len(guards)*0x1000,ordinal=ordinal,library=lib,name=item[0]))
 cur+=length
assert any(x['iat']==0x8200062c and x['name']=='XexExecutableModuleHandle' for x in guards)
p=private/'startup-sections.bin'
with p.open('xb') as f:
 f.write(b'MEBOOT01'+struct.pack('<I',len(sections)))
 for addr,data in sections:f.write(struct.pack('<II',addr,len(data)));f.write(data)
 f.write(struct.pack('<I',len(guards)))
 for x in guards:f.write(struct.pack('<III',x['iat'],x['guard'],x['ordinal'])+x['name'].encode().ljust(96,b'\0'))
mapping=(g/'ppc_func_mapping.cpp').read_text();imports=sorted(set(re.findall(r'\{ 0x[0-9A-F]+, (__imp__\w+) \}',mapping)))
stubs=['#include "ppc_recomp_shared.h"','[[noreturn]] void me_stop_import(const char*,PPCContext&);']
for name in imports:stubs.append(f'PPC_FUNC({name}) {{ me_stop_import("{name}",ctx); }}')
(r/'work/startup/import-stops.cpp').write_text('\n'.join(stubs))
summary=dict(sections=[dict(base=hex(a),size=len(b)) for a,b in sections],variable_guards=guards,function_import_stops=len(imports),bundle_bytes=p.stat().st_size,bundle_sha256=hashlib.sha256(p.read_bytes()).hexdigest(),semantics='Unimplemented variable exports are bound to inaccessible guard pages; function exports always terminate with a diagnostic, never return fake success')
(r/'outputs/mirrors-edge-native/xbox/startup/preparation.json').write_text(json.dumps(summary,indent=2));print(json.dumps(dict(sections=len(sections),variable_guards=len(guards),function_import_stops=len(imports),bundle_bytes=p.stat().st_size),indent=2))
