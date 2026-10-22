from pathlib import Path
import re,subprocess,os,ctypes,json,struct
from unicorn import Uc,UC_ARCH_PPC,UC_MODE_32,UC_MODE_BIG_ENDIAN
from unicorn.ppc_const import UC_PPC_REG_3,UC_PPC_REG_4,UC_PPC_REG_LR
r=Path.cwd();g=r/'work/xbox-game-private/generated'
t=next(p.read_text() for p in g.glob('ppc_recomp.*.cpp') if 'PPC_FUNC_IMPL(__imp__sub_8223A5D8)' in p.read_text())
a=t.index('PPC_FUNC_IMPL(__imp__sub_8223A5D8)'); opening=t.index('{',a); depth=1; end=opening+1
while depth:
 depth+=(t[end]=='{')-(t[end]=='}');end+=1
fn=t[a:end]
assert 'ERROR:' not in fn and '__builtin_trap' not in fn
source='#include "ppc_context.h"\n'+fn+'''\nextern "C" __declspec(dllexport) int check(unsigned char* memory,int month,int year,int day) {

  memory[0x1004]=month; memory[0x1006]=(year>>8)&255; memory[0x1007]=year&255;
  PPCContext ctx{};ctx.r3.u64=0x1000;ctx.r4.s64=day;
  __imp__sub_8223A5D8(ctx,memory);return ctx.r3.s32;
}\n'''
p=r/'work/xbox-game-private/check-date.cpp';p.write_text(source)
cc=r/'work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe';dll=p.with_suffix('.dll')
cmd=[str(cc),'-shared','-O2','-std=c++17','-DSIMDE_ENABLE_NATIVE_ALIASES','-I'+str(g),'-I'+str(r/'work/references/XenonRecomp/thirdparty/simde'),str(p),'-o',str(dll)]
z=subprocess.run(cmd,capture_output=True);(r/'work/xbox-audit/date-build.log').write_bytes(z.stdout+z.stderr);assert z.returncode==0,z.stderr.decode(errors='replace')
handle=os.add_dll_directory(str(cc.parent));lib=ctypes.CDLL(str(dll));check=lib.check;check.argtypes=[ctypes.c_void_p]+[ctypes.c_int]*3;check.restype=ctypes.c_int
u=Uc(UC_ARCH_PPC,UC_MODE_32|UC_MODE_BIG_ENDIAN);u.mem_map(0x1000,0x2000);u.mem_map(0x82230000,0x10000)
b=(r/'work/xbox-game-private/code.bin').read_bytes();u.mem_write(0x8223a5d8,b[0xa5d8:0xa5d8+0xdc]);u.mem_write(0x2000,b'\x60\x00\x00\x00')
rdata=next((r/'work/xbox-game-private').glob('section.2181039104.bin')).read_bytes()
u.mem_map(0x82000000,0x1d2000);u.mem_write(0x82000400,rdata)
k=ctypes.WinDLL('kernel32',use_last_error=True);k.VirtualAlloc.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint32,ctypes.c_uint32];k.VirtualAlloc.restype=ctypes.c_void_p
memory=k.VirtualAlloc(None,0x100000000,0x2000,4);assert memory
assert k.VirtualAlloc(memory+0x1000,0x1000,0x1000,4)
assert k.VirtualAlloc(memory+0x82000000,0x1d2000,0x1000,4)
ctypes.memmove(memory+0x82000400,rdata,len(rdata))
cases=0
for month in range(0,14):
 for year in [-32768,-400,-100,-4,-1,0,1,4,100,400,1900,2000,2024,2100,32767]:
  for day in range(-2,35):
   u.mem_write(0x1000,b'\0'*4+bytes([month,0])+struct.pack('>h',year))
   u.reg_write(UC_PPC_REG_3,0x1000);u.reg_write(UC_PPC_REG_4,day&0xffffffff);u.reg_write(UC_PPC_REG_LR,0x2000)
   u.emu_start(0x8223a5d8,0x2000,count=200)
   original=ctypes.c_int32(u.reg_read(UC_PPC_REG_3)).value;compiled=check(memory,month,year,day)
   assert original==compiled,(month,year,day,original,compiled)
   cases+=1
result=dict(function='0x8223A5D8',size=0xdc,cases=cases,passed=True,reference='original PPC32 big-endian instructions in Unicorn',candidate='actual generated C++ compiled for Windows x64',limitations='Tests output r3 only; not ARM64 execution or full-game behavior')
(r/'outputs/mirrors-edge-native/xbox/date-function-test.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
