from pathlib import Path
import subprocess,concurrent.futures,json,time
r=Path.cwd();g=r/'work/xbox-game-private/generated-fourth-final';out=r/'work/xbox-game-private/arm64-objects-mmio';out.mkdir(exist_ok=True)
cc=r/'work/toolchains/android-ndk-r30/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
h=r/'outputs/mirrors-edge-native/xbox/startup/guest_memory_access.h'
files=sorted(g.glob('*.cpp'));start=time.monotonic()
def build(p):
 obj=out/(p.stem+'.o')
 # Incremental: skip units whose object is newer than the source and the shared header.
 if obj.exists() and obj.stat().st_mtime>=max(p.stat().st_mtime,h.stat().st_mtime):return dict(file=p.name,exit=0,error='',skipped=True)
 q=subprocess.run([str(cc),'--target=aarch64-linux-android35','-std=c++17','-DSIMDE_ENABLE_NATIVE_ALIASES','-I'+str(r/'work/references/XenonRecomp/thirdparty/simde'),'-O2','-fPIC','-Werror=asm-operand-widths','-include',str(h),'-c',str(p),'-o',str(obj)],capture_output=True)
 return dict(file=p.name,exit=q.returncode,error=q.stderr.decode(errors='replace')[-1500:] if q.returncode else '')
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:rows=list(pool.map(build,files))
report=dict(units=len(rows),passed=sum(x['exit']==0 for x in rows),elapsed_seconds=round(time.monotonic()-start,1),results=rows)
if report['passed']==report['units']:
 rsp=out/'archive.rsp';rsp.write_text('\n'.join('"'+str(out/(p.stem+'.o')).replace('\\','/')+'"' for p in files))
 q=subprocess.run([str(cc.parent/'llvm-ar.exe'),'rcs',str(out.parent/'libmirrorsedge_ppc_mmio.a'),'@'+str(rsp)],capture_output=True)
 report['archive_exit']=q.returncode
(r/'outputs/mirrors-edge-native/xbox/gpu-port/mmio-rebuild-result.json').write_text(json.dumps(report,indent=2))
print(json.dumps({k:v for k,v in report.items() if k!='results'}))
for x in rows:
 if x['exit']:print(x)
raise SystemExit(0 if report['passed']==len(rows) and report.get('archive_exit')==0 else 1)

