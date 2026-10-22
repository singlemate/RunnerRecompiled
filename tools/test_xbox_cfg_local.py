from pathlib import Path
import ctypes,os,re,struct,subprocess,json,random
from unicorn import Uc,UC_ARCH_PPC,UC_MODE_32,UC_MODE_BIG_ENDIAN
from unicorn.ppc_const import UC_PPC_REG_3,UC_PPC_REG_4,UC_PPC_REG_5,UC_PPC_REG_LR
r=Path.cwd();g=r/'work/xbox-game-private/generated-third';functions=[0x823741d8,0x82e50868,0x82a594d0];source=['#include "ppc_context.h"']
for a in functions:
 marker=f'PPC_FUNC_IMPL(__imp__sub_{a:X})';t=next(p.read_text() for p in g.glob('ppc_recomp.*.cpp') if marker in p.read_text());start=t.index(marker);opening=t.index('{',start);level=1;end=opening+1
 while level:level+=(t[end]=='{')-(t[end]=='}');end+=1
 fn=t[start:end];assert '__builtin_trap' not in fn and '// ERROR' not in fn;source.append(fn)
source+=['extern "C" __declspec(dllexport) uint32_t check(unsigned which,uint8_t* memory,uint32_t a,uint32_t b,uint32_t c){PPCContext ctx{};ctx.r3.u64=a;ctx.r4.u64=b;ctx.r5.u64=c; switch(which){']
for i,a in enumerate(functions):source.append(f'case {i}:__imp__sub_{a:X}(ctx,memory);break;')
source.append('}return ctx.r3.u32;}');p=r/'work/xbox-game-private/cfg-test.cpp';p.write_text('\n'.join(source));cc=r/'work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe';dll=p.with_suffix('.dll')
z=subprocess.run([str(cc),'-shared','-O2','-std=c++17','-DSIMDE_ENABLE_NATIVE_ALIASES','-I'+str(g),'-I'+str(r/'work/references/XenonRecomp/thirdparty/simde'),str(p),'-o',str(dll)],capture_output=True);assert z.returncode==0,z.stderr.decode(errors='replace')
h=os.add_dll_directory(str(cc.parent));f=ctypes.CDLL(str(dll)).check;f.argtypes=[ctypes.c_uint,ctypes.c_void_p]+[ctypes.c_uint]*3;f.restype=ctypes.c_uint
k=ctypes.WinDLL('kernel32',use_last_error=True);k.VirtualAlloc.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32,ctypes.c_uint32];k.VirtualAlloc.restype=ctypes.c_void_p
memory=k.VirtualAlloc(None,0x100000000,0x2000,4);assert memory
assert k.VirtualAlloc(memory+0x82000000,0xec0000,0x1000,4)
u=Uc(UC_ARCH_PPC,UC_MODE_32|UC_MODE_BIG_ENDIAN);u.mem_map(0x1000,0x1000);u.mem_write(0x1000,b'\x60\x00\x00\x00');u.mem_map(0x82000000,0xec0000)
for base in (0x82000400,0x82230000):
 data=next((r/'work/xbox-game-private').glob(f'section.{base}.bin')).read_bytes();u.mem_write(base,data);ctypes.memmove(memory+base,data,len(data))
rng=random.Random(441);rows=[]
for which,entry in enumerate(functions):
 inputs=[(a&0xffffffff,b&0xffffffff,c&0xffffffff) for a in range(-4,70) for b in (-1,0,1) for c in range(-2,8)]
 inputs += [tuple(rng.getrandbits(32) for _ in range(3)) for i in range(1000)]
 for a,b,c in inputs:
  u.reg_write(UC_PPC_REG_3,a);u.reg_write(UC_PPC_REG_4,b);u.reg_write(UC_PPC_REG_5,c);u.reg_write(UC_PPC_REG_LR,0x1000)
  u.emu_start(entry,0x1000,count=200)
  original=u.reg_read(UC_PPC_REG_3)&0xffffffff;candidate=f(which,memory,a,b,c)
  assert original==candidate,(hex(entry),a,b,c,original,candidate)
 rows.append(dict(address=hex(entry),cases=len(inputs),passed=True))
result=dict(passed=True,cases=sum(x['cases'] for x in rows),functions=rows,reference='original PPC32 big-endian execution in Unicorn with original code and rdata',candidate='actual generated C++ compiled for Windows x64',limitations='Compare return r3 only; not full-state equivalence, Xbox hardware or Android execution; only 3 of 66 CFG overrides exercised')
(r/'outputs/mirrors-edge-native/xbox/cfg-functions-test.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
