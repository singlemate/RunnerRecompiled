from pathlib import Path
import re,subprocess,os,json
r=Path.cwd();g=r/'work/xbox-game-private/generated';records=[]
for p in g.glob('ppc_recomp.*.cpp'):
 records.extend(re.findall(r'// (vandc|vnor|eqv) ([^\n]+)\n([^\n]+)',p.read_text()))
assert len(records)==190,len(records)
source=['#include "ppc_context.h"','#include <cstdio>','uint64_t state=0x12345678; uint64_t rnd(){state^=state<<13;state^=state>>7;return state^=state<<17;}','int main(){unsigned checks=0;']
for idx,(op,operands,statement) in enumerate(records):
 d,a,b=re.findall(r'[vr](\d+)',operands)
 source.append('{ PPCContext ctx{}; for(int test=0;test<100;++test) {')
 if op=='eqv':
  source.extend([f'ctx.r{a}.u64=rnd();ctx.r{b}.u64=rnd();',f'uint64_t x=ctx.r{a}.u64,y=ctx.r{b}.u64,expected=0;','for(int bit=0;bit<64;++bit) if(((x>>bit)&1)==((y>>bit)&1))expected|=uint64_t(1)<<bit;',statement,f'if(ctx.r{d}.u64!=expected)return {idx+1};'])
 else:
  source.extend([f'for(int i=0;i<16;++i){{ctx.v{a}.u8[i]=rnd();ctx.v{b}.u8[i]=rnd();}}','uint8_t expected[16]{};',f'for(int i=0;i<16;++i)for(int bit=0;bit<8;++bit){{bool x=(ctx.v{a}.u8[i]>>bit)&1,y=(ctx.v{b}.u8[i]>>bit)&1;',('if(x&&!y)' if op=='vandc' else 'if(!x&&!y)')+'expected[i]|=1<<bit;}',statement,f'for(int i=0;i<16;++i)if(ctx.v{d}.u8[i]!=expected[i])return {idx+1};'])
 source.append('++checks; }}')
source.append('printf("%u checks passed\\n",checks);return 0;}')
p=r/'work/xbox-game-private/bitwise-test.cpp';p.write_text('\n'.join(source));exe=p.with_suffix('.exe')
cc=r/'work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64/bin/clang++.exe'
z=subprocess.run([str(cc),'-O2','-std=c++17','-DSIMDE_ENABLE_NATIVE_ALIASES','-I'+str(g),'-I'+str(r/'work/references/XenonRecomp/thirdparty/simde'),str(p),'-o',str(exe)],capture_output=True)
assert z.returncode==0,z.stderr.decode(errors='replace')
env=os.environ.copy();env['PATH']=str(cc.parent)+';'+env['PATH'];q=subprocess.run([str(exe)],env=env,capture_output=True);assert q.returncode==0,q.returncode
result=dict(passed=True,generated_instruction_sites=len(records),cases=len(records)*100,operations=['vandc','vnor','eqv'],reference='independent per-bit truth table; includes original operand aliases',candidate='actual emitted statements compiled for Windows x64',limitations='No PPC hardware equivalence, ARM64 execution or EQV record-form flag test')
(r/'outputs/mirrors-edge-native/xbox/bitwise-test.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
