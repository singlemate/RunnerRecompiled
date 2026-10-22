from pathlib import Path
import re,subprocess,os,ctypes,json,struct,math,random
r=Path.cwd();g=r/'work/xbox-game-private/generated-fourth-final';records=[]
for p in g.glob('ppc_recomp.*.cpp'):records+=re.findall(r'// (vcfpuxws128) ([^\n]+)\n([^\n]+)',p.read_text())
assert len(records)==31
s=['#include "ppc_context.h"','extern "C" __declspec(dllexport) uint32_t helper(uint32_t* values,unsigned scale,uint32_t flags){PPCVRegister a{};memcpy(a.u32,values,16);a=me_float_to_u32_fixed(a,scale,flags);memcpy(values,a.u32,16);return flags;}', 'extern "C" __declspec(dllexport) uint32_t check(unsigned site,unsigned char* bank,uint32_t flags){PPCContext ctx{};memcpy(&ctx.v0,bank,2048);ctx.vscr=flags;switch(site){']
for i,(op,ops,stmt) in enumerate(records):s.extend([f'case {i}:{{',stmt,'break;}'])
s.append('}memcpy(bank,&ctx.v0,2048);return ctx.vscr;}');p=r/'work/xbox-game-private/float-test.cpp';p.write_text('\n'.join(s));cc=r/'work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe';dll=p.with_suffix('.dll')
z=subprocess.run([str(cc),'-shared','-O2','-std=c++17','-DSIMDE_ENABLE_NATIVE_ALIASES','-I'+str(g),'-I'+str(r/'work/references/XenonRecomp/thirdparty/simde'),str(p),'-o',str(dll)],capture_output=True);assert z.returncode==0,z.stderr.decode(errors='replace')
h=os.add_dll_directory(str(cc.parent));lib=ctypes.CDLL(str(dll));helper=lib.helper;helper.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_uint];helper.restype=ctypes.c_uint;check=lib.check;check.argtypes=[ctypes.c_uint,ctypes.c_void_p,ctypes.c_uint];check.restype=ctypes.c_uint
# Independent reference uses host double's exact representation of binary32 and arbitrary precision truncation.
def reference(bits,scale):
 f=struct.unpack('<f',struct.pack('<I',bits))[0]
 if math.isnan(f):return 0,False
 if math.isinf(f):return (0 if f<0 else 0xffffffff),True
 n=math.trunc(math.ldexp(f,scale))
 return max(0,min(n,0xffffffff)),not 0<=n<=0xffffffff
fractions=[0,1,0x3fffff,0x400000,0x7ffffe,0x7fffff];lanes=0;vectors=0
for scale in range(32):
 for exponent in range(256):
  for sign in range(2):
   for frac in fractions:
    bits=(sign<<31)|(exponent<<23)|frac
    values=[bits,bits^0x80000000,bits^1,0x4f800000];flags=0x10000|(vectors&1);expected=[];ef=flags
    for v in values:
     val,sat=reference(v,scale);expected.append(val)
     if sat:ef|=1
    raw=(ctypes.c_uint32*4)(*values);actual=helper(raw,scale,flags)
    assert list(raw)==expected and actual==ef,(hex(bits),scale,list(raw),expected,actual,ef)
    lanes+=4;vectors+=1
rng=random.Random(932);cases=0
for site,(op,ops,stmt) in enumerate(records):
 dest,src,scale=map(int,re.findall(r'\d+',ops))
 assert stmt.strip()==f'ctx.v{dest} = me_float_to_u32_fixed(ctx.v{src}, {scale}, ctx.vscr);'
 for trial in range(200):
  bank=bytearray(rng.randbytes(2048));flags=[0,1,0x10000][trial%3];expected=bank.copy();ef=flags
  if trial<30:
   boundaries=[0,0x80000000,0x3f000000,0xbf000000,0x3f800000,0xbf800000,0x4f7fffff,0x4f800000,0x7f800000,0xff800000,0x7fc00000,0x7f800001,1,0x80000001]
   for lane in range(4):struct.pack_into('<I',bank,src*16+lane*4,boundaries[(trial+lane)%len(boundaries)])
   expected=bank.copy()
  values=struct.unpack_from('<4I',bank,src*16)
  for lane,v in enumerate(values):
   out,sat=reference(v,scale);struct.pack_into('<I',expected,dest*16+lane*4,out)
   if sat:ef|=1
  raw=(ctypes.c_ubyte*2048).from_buffer_copy(bank);actual=check(site,raw,flags)
  assert bytes(raw)==bytes(expected) and actual==ef,(site,trial,actual,ef)
  cases+=1
result=dict(passed=True,helper_vectors=vectors,helper_lanes=lanes,immediate_scales=32,generated_sites=len(records),generated_cases=cases,reference='binary32 unpacked to exact double, ldexp, arbitrary precision truncation and clamp',checks=['all exponent fields','both signs','NaNs including signaling encodings','infinities','subnormals','signed zero','saturation after truncation','sticky VSCR','all vector registers unchanged except destination'],limitations='Windows x64 candidate execution; generic PowerPC conversion semantics, VMX128 hardware equivalence not established')
(r/'outputs/mirrors-edge-native/xbox/float-fixed-test.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
