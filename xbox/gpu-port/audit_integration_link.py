from pathlib import Path
import subprocess,json
r=Path.cwd();p=r/'work/gpu-port';bin=r/'work/toolchains/android-ndk-r30/toolchains/llvm/prebuilt/windows-x86_64/bin'
objects=[]
for folder in ['objects','support','vulkan-bootstrap','integration-support']:objects+=sorted((p/folder).glob('*.o'))
q=subprocess.run([str(bin/'clang++.exe'),'--target=aarch64-linux-android35','-shared',*map(str,objects),'-Wl,--start-group',*map(str,(p/'deps-build').rglob('*.a')),'-Wl,--end-group','-static-libstdc++','-lvulkan','-landroid','-ldl','-Wl,--no-undefined','-Wl,--error-limit=0','-o',str(p/'librexgpu_xenos_external.so')],capture_output=True)
(p/'link-integration-extended.log').write_bytes(q.stderr+q.stdout)
errors=[x.split('undefined symbol: ',1)[1] for x in q.stderr.decode(errors='replace').splitlines() if 'undefined symbol:' in x]
report={'exit_code':q.returncode,'units':len(objects),'undefined_symbols':len(errors),'symbols':errors,'linked':q.returncode==0}
(r/'outputs/mirrors-edge-native/xbox/gpu-port/link-integration-extended-result.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report,indent=2))
