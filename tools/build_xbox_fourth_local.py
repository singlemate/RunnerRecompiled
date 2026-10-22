import concurrent.futures,json,subprocess,time
from pathlib import Path
r=Path.cwd(); g=r/'work/xbox-game-private/generated-fourth-final'; out=r/'work/xbox-game-private/arm64-objects-fourth-final';out.mkdir(exist_ok=True)
cc=r/'work/toolchains/android-ndk-r30/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
files=sorted(g.glob('*.cpp'))
def build(p):
    o=out/(p.stem+'.o')
    command=[str(cc),'--target=aarch64-linux-android35','-std=c++17','-DSIMDE_ENABLE_NATIVE_ALIASES','-I'+str(r/'work/references/XenonRecomp/thirdparty/simde'),'-O0','-fPIC','-Werror=asm-operand-widths','-c',str(p),'-o',str(o)]
    q=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (out/(p.name+'.log')).write_bytes(q.stdout)
    return dict(file=p.name,exit_code=q.returncode,bytes=o.stat().st_size if q.returncode==0 else 0)
t=time.monotonic()
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool: rows=list(pool.map(build,files))
result=dict(generated_directory=str(g),object_directory=str(out),target='aarch64-linux-android35',optimization='O0',position_independent=True,units=len(rows),passed=sum(x['exit_code']==0 for x in rows),failed=sum(x['exit_code']!=0 for x in rows),elapsed_seconds=round(time.monotonic()-t,1),object_bytes=sum(x['bytes'] for x in rows),linked_runtime=False,game_executed=False,results=rows)
if not result['failed']:
    objects=[out/(p.stem+'.o') for p in files]; rsp=out/'archive.rsp';rsp.write_text('\n'.join('"'+str(p).replace('\\','/')+'"' for p in objects))
    lib=out.parent/'libmirrorsedge_ppc_fourth.a'
    if lib.exists():lib.unlink()
    q=subprocess.run([str(cc.parent/'llvm-ar.exe'),'rcs',str(lib),'@'+str(rsp)],capture_output=True)
    result['archive_exit_code']=q.returncode
    if q.returncode==0:result['archive_bytes']=lib.stat().st_size
(r/'outputs/mirrors-edge-native/xbox/arm64-build-fourth-result.json').write_text(json.dumps(result,indent=2))
print(json.dumps({k:v for k,v in result.items() if k!='results'},indent=2),flush=True)
