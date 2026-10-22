from pathlib import Path
import re,subprocess,os,ctypes,json,struct,random
from unicorn import *
import unicorn.ppc_const as pc
r=Path.cwd();g=r/'work/xbox-game-private/generated-fourth-final';functions=[0x82747578,0x82747588,0x82d37bc8,0x82d37bd4];source=['#include "ppc_context.h"']+[f'PPC_EXTERN_FUNC(sub_{a:X});' for a in functions]
for a in functions:
 marker=f'PPC_FUNC_IMPL(__imp__sub_{a:X})';t=next(p.read_text() for p in g.glob('ppc_recomp.*.cpp') if marker in p.read_text());start=t.index(marker);i=t.index('{',start)+1;level=1
 while level:level+=(t[i]=='{')-(t[i]=='}');i+=1
 fn=t[start:i];assert '__builtin_trap' not in fn and '// ERROR' not in fn;source+=[fn,f'__attribute__((alias("__imp__sub_{a:X}"))) PPC_WEAK_FUNC(sub_{a:X});']
source+=['extern "C" __declspec(dllexport) void check(unsigned which,uint8_t* memory,uint64_t* regs){PPCContext ctx{};ctx.lr=0x9000;']+[f'ctx.r{i}.u64=regs[{i}];' for i in range(32)]+['switch(which){']
source += [f'case {i}:__imp__sub_{a:X}(ctx,memory);break;' for i,a in enumerate(functions)]
source+=['}']+[f'regs[{i}]=ctx.r{i}.u32;' for i in range(32)]+['}']
p=r/'work/xbox-game-private/continuation-test.cpp';p.write_text('\n'.join(source));cc=r/'work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe';dll=p.with_suffix('.dll')
z=subprocess.run([str(cc),'-shared','-O2','-std=c++17','-DSIMDE_ENABLE_NATIVE_ALIASES','-I'+str(g),'-I'+str(r/'work/references/XenonRecomp/thirdparty/simde'),str(p),'-o',str(dll)],capture_output=True);assert z.returncode==0,z.stderr.decode(errors='replace')
h=os.add_dll_directory(str(cc.parent));f=ctypes.CDLL(str(dll)).check;f.argtypes=[ctypes.c_uint,ctypes.c_void_p,ctypes.c_void_p]
k=ctypes.WinDLL('kernel32');k.VirtualAlloc.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32,ctypes.c_uint32];k.VirtualAlloc.restype=ctypes.c_void_p
memory=k.VirtualAlloc(None,0x100000000,0x2000,4);assert memory
assert k.VirtualAlloc(memory+0x1000,0x9000,0x1000,4)
assert k.VirtualAlloc(memory+0x82000000,0xec0000,0x1000,4)
u=Uc(UC_ARCH_PPC,UC_MODE_32|UC_MODE_BIG_ENDIAN);u.mem_map(0x1000,0x9000);u.mem_map(0x82000000,0xec0000)
for base in (0x82000400,0x82230000):
 data=next((r/'work/xbox-game-private').glob(f'section.{base}.bin')).read_bytes();u.mem_write(base,data);ctypes.memmove(memory+base,data,len(data))
rng=random.Random(137);rows=[]
for which,entry in enumerate(functions):
 for trial in range(1000):
  regs=[rng.getrandbits(32) for _ in range(32)];regs[1]=0x8000;regs[3]=0x1000;regs[4]=0x3000
  page=bytearray(rng.randbytes(0x4000))
  for offset in (0x18,0x1c,0x20,0x24):struct.pack_into('>I',page,0x2000+offset,[-1,0,1,2,3,4,5,6,7,8,9][(trial+offset)%11]&0xffffffff)
  u.mem_write(0x1000,bytes(page));ctypes.memmove(memory+0x1000,bytes(page),len(page))
  for i,value in enumerate(regs):u.reg_write(getattr(pc,f'UC_PPC_REG_{i}'),value)
  u.reg_write(pc.UC_PPC_REG_LR,0x9000);u.reg_write(pc.UC_PPC_REG_CTR,0);u.reg_write(pc.UC_PPC_REG_CR,0);u.reg_write(pc.UC_PPC_REG_XER,0)
  u.emu_start(entry,0x9000,count=1000)
  original=[u.reg_read(getattr(pc,f'UC_PPC_REG_{i}'))&0xffffffff for i in range(32)]
  cregs=(ctypes.c_uint64*32)(*regs);f(which,memory,cregs)
  assert list(cregs)==original,(hex(entry),trial,[(i,original[i],cregs[i]) for i in range(32) if original[i]!=cregs[i]])
  assert ctypes.string_at(memory+0x1000,len(page))==bytes(u.mem_read(0x1000,len(page))),(hex(entry),trial,'memory')
 rows.append(dict(address=hex(entry),cases=1000,passed=True))
result=dict(passed=True,cases=4000,functions=rows,reference='original PPC32 big-endian instructions in Unicorn',candidate='actual emitted continuation entries compiled for Windows x64',checks=['all 32 low GPR words','16 KiB data memory'],limitations='CR/XER/LR/vector state not compared; no ARM64 execution or full caller-chain proof')
(r/'outputs/mirrors-edge-native/xbox/continuation-entries-test.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
