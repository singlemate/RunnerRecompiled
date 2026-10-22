from pathlib import Path
import re,subprocess,os,ctypes,json,random,struct
r=Path.cwd();g=r/'work/xbox-game-private/generated';records=[]
for p in g.glob('ppc_recomp.*.cpp'):
 records += re.findall(r'// (vpkuwus128|vpkuhus128|vpkswss128|vsel128|mulhd) ([^\n]+)\n([^\n]+)',p.read_text())
assert len(records)>=73,len(records)
s=['#include "ppc_context.h"','extern "C" __declspec(dllexport) uint32_t check(unsigned site,unsigned char* bank,uint64_t* regs,uint32_t flags){PPCContext ctx{};memcpy(&ctx.v0,bank,2048);GPR_INPUTctx.vscr=flags;switch(site){']
for i,(op,ops,statement) in enumerate(records):s += [f'case {i}: {{',statement,'break;}']
s+=['}memcpy(bank,&ctx.v0,2048);GPR_OUTPUTreturn ctx.vscr;}']
p=r/'work/xbox-game-private/pack-test.cpp';p.write_text('\n'.join(s).replace('GPR_INPUT',''.join(f'ctx.r{i}.u64=regs[{i}];' for i in range(32))).replace('GPR_OUTPUT',''.join(f'regs[{i}]=ctx.r{i}.u64;' for i in range(32))));cc=r/'work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe';dll=p.with_suffix('.dll')
z=subprocess.run([str(cc),'-shared','-O2','-std=c++17','-DSIMDE_ENABLE_NATIVE_ALIASES','-I'+str(g),'-I'+str(r/'work/references/XenonRecomp/thirdparty/simde'),str(p),'-o',str(dll)],capture_output=True);assert z.returncode==0,z.stderr.decode(errors='replace')
h=os.add_dll_directory(str(cc.parent));f=ctypes.CDLL(str(dll)).check;f.argtypes=[ctypes.c_uint,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_uint];f.restype=ctypes.c_uint
rng=random.Random(4871);cases=0;mask=(1<<64)-1
# PPCRegister is a union of 64-bit scalar views; assert wrapper uses its actual stride.
# Raw register storage is 32 * 8, matching the generated context header.
for site,(op,ops,stmt) in enumerate(records):
 ids=list(map(int,re.findall(r'[vr](\d+)',ops)))
 for trial in range(150):
  bank=bytearray(rng.randbytes(2048));regs=[rng.getrandbits(64) for _ in range(32)];flags=[0,1,0x10000][trial%3];expected=bank.copy();expectedregs=regs.copy();ef=flags
  if op=='mulhd':
   d,a,b=ids
   edges=[0,1,mask,1<<63,(1<<63)-1,2,mask-1]
   if trial<len(edges)**2:regs[a]=edges[trial%len(edges)];regs[b]=edges[trial//len(edges)]
   expectedregs=regs.copy();x=ctypes.c_int64(regs[a]).value;y=ctypes.c_int64(regs[b]).value;expectedregs[d]=((x*y)>>64)&mask
  elif op=='vsel128':
   d,a,b,c=ids
   for j in range(16):expected[d*16+j]=(bank[a*16+j]&(~bank[c*16+j]&255))|(bank[b*16+j]&bank[c*16+j])
  else:
   d,a,b=ids;bits=16 if op=='vpkuhus128' else 32;outbits=bits//2;signed=op=='vpkswss128';lo=-(1<<(outbits-1)) if signed else 0;hi=(1<<(outbits-1))-1 if signed else (1<<outbits)-1
   # Explicit boundary values as guest big-endian elements; reverse host vector representation.
   if trial<30:
    values=[0,1,hi-1,hi,hi+1,(1<<bits)-1,1<<(bits-1),65536,255,256]
    for reg,shift in [(a,0),(b,3)]:
     guest=b''.join((values[(trial+shift+j)%len(values)]&((1<<bits)-1)).to_bytes(bits//8,'big') for j in range(128//bits));bank[reg*16:(reg+1)*16]=guest[::-1]
   expected=bank.copy();packed=bytearray()
   for reg in (a,b):
    guest=bank[reg*16:(reg+1)*16][::-1]
    for j in range(0,16,bits//8):
     value=int.from_bytes(guest[j:j+bits//8],'big',signed=signed)
     if value<lo or value>hi:ef|=1
     value=max(lo,min(value,hi));packed.extend(value.to_bytes(outbits//8,'big',signed=signed))
   expected[d*16:(d+1)*16]=packed[::-1]
  cbank=(ctypes.c_ubyte*2048).from_buffer_copy(bank);cregs=(ctypes.c_uint64*32)(*regs);actual=f(site,cbank,cregs,flags)
  assert bytes(cbank)==bytes(expected) and list(cregs)==expectedregs and actual==ef,(site,op,ops,trial,actual,ef)
  cases+=1
result=dict(passed=True,sites=len(records),cases=cases,operations=sorted(set(x[0] for x in records)),reference='guest big-endian scalar clamp/pack, per-byte select, Python arbitrary precision signed product',candidate='actual emitted C++ statements; Windows x64',checks=['all vector registers','all GPRs','sticky VSCR bit','operand aliasing','boundary and seeded random values'],limitations='No Xbox hardware comparison; no ARM64 execution; local PPCContext extended with VSCR')
(r/'outputs/mirrors-edge-native/xbox/pack-select-mul-test.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
