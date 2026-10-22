import concurrent.futures,subprocess,json
from pathlib import Path
r=Path.cwd();g=r/'work/xbox-game-private/generated-fourth-final';out=r/'work/xbox-game-private/arm64-objects-fourth-final'
cc=r/'work/toolchains/android-ndk-r30/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
files=[p for p in g.glob('*.cpp') if '__rdtsc' in p.read_text()]
def build(p):
 q=subprocess.run([str(cc),'--target=aarch64-linux-android35','-std=c++17','-DSIMDE_ENABLE_NATIVE_ALIASES','-I'+str(r/'work/references/XenonRecomp/thirdparty/simde'),'-O0','-fPIC','-Werror=asm-operand-widths','-c',str(p),'-o',str(out/(p.stem+'.o'))],capture_output=True)
 return dict(file=p.name,exit=q.returncode,error=q.stderr.decode(errors='replace')[:1000])
with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool: rows=list(pool.map(build,files))
assert all(x['exit']==0 for x in rows),rows
q=subprocess.run([str(cc.parent/'llvm-ar.exe'),'rcs',str(out.parent/'libmirrorsedge_ppc_fourth.a'),'@'+str(out/'archive.rsp')],capture_output=True)
assert q.returncode==0,q.stderr
(r/'work/startup/timebase-rebuild.json').write_text(json.dumps(rows,indent=2))
print(json.dumps(rows))
