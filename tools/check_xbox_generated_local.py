import collections, concurrent.futures, json, re, subprocess, time
from pathlib import Path
root=Path.cwd()
generated=root/'work/xbox-game-private/generated'
files=sorted(generated.glob('ppc_recomp.*.cpp'))
compiler=root/'work/toolchains/android-ndk-r30/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
include=root/'work/references/XenonRecomp/thirdparty/simde'
logfolder=root/'work/xbox-audit/syntax'
logfolder.mkdir(exist_ok=True)
def check(p):
    cmd=[str(compiler),'--target=aarch64-linux-android35','-std=c++17','-DSIMDE_ENABLE_NATIVE_ALIASES','-I'+str(include),'-fsyntax-only','-Wno-asm-operand-widths',str(p)]
    result=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    logfolder.joinpath(p.name+'.log').write_bytes(result.stdout)
    return dict(file=p.name,exit_code=result.returncode,errors=result.stdout.count(b' error:'))
start=time.monotonic()
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    results=list(pool.map(check,files))
log=(root/'work/xbox-audit/xenon-recompile.log').read_text(errors='replace')
shared=(generated/'ppc_recomp_shared.h').read_text()
summary=dict(generator_commit='ddd128bcca99fe8bfbb99bea583c972351fa6ace',xex_sha256='3ed882fd1900ff2bbccf1de745fb7e92c7d2168bb06069cbab311ac8eff28e1b',generated_files=len(list(generated.iterdir())),generated_bytes=sum(p.stat().st_size for p in generated.iterdir()),function_declarations=shared.count('PPC_EXTERN_FUNC('),translation_units=len(files),syntax_pass=sum(r['exit_code']==0 for r in results),syntax_fail=sum(r['exit_code']!=0 for r in results),elapsed_seconds=round(time.monotonic()-start,1),diagnostic_counts=dict(collections.Counter('outside_function_switch' if 'trying to jump outside function' in l else 'missing_switch_table' if 'with no switch table entry' in l else 'unrecognized_instruction' if 'Unrecognized instruction' in l else 'unspecified_helper' if 'address is unspecified' in l else 'other_error' for l in log.splitlines() if 'ERROR:' in l or 'Unrecognized instruction' in l or 'with no switch table entry' in l)),unsupported_instructions=re.findall(r'Unrecognized instruction at (0x[0-9A-Fa-f]+): (\w+)',log),results=results,game_executed=False,linked_runtime=False)
(root/'outputs/mirrors-edge-native/xbox/recompilation-result.json').write_text(json.dumps(summary,indent=2))
print(json.dumps({k:v for k,v in summary.items() if k!='results'},indent=2),flush=True)
