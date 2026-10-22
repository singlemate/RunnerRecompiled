from pathlib import Path
import subprocess,json
r=Path.cwd();scope={};source=Path(__file__).with_name('compile_gpu_objects.py').read_text();exec(source[:source.index('def build')],scope)
cc=scope['cc'];common=scope['common'];out=r/'work/gpu-port';obj=out/'shader-check.o'
q=subprocess.run([str(cc),*common,'-c',str(r/'outputs/mirrors-edge-native/xbox/gpu-port/shader_translation_check.cpp'),'-o',str(obj)],capture_output=True)
(out/'shader-check-compile.log').write_bytes(q.stderr);assert not q.returncode,q.stderr.decode(errors='replace')
libs=list((out/'deps-build').rglob('*.a'))
args=[str(cc),'--target=aarch64-linux-android35',str(obj),*map(str,sorted((out/'support').glob('*.o'))),'-Wl,--start-group',str(out/'librexgpu_xenos_android_objects.a'),*map(str,libs),'-Wl,--end-group','-static-libstdc++','-lvulkan','-Wl,--gc-sections','-Wl,--error-limit=0','-o',str(out/'shader_translation_check')]
q=subprocess.run(args,capture_output=True);(out/'shader-check-link.log').write_bytes(q.stderr+q.stdout)
errors=[x for x in q.stderr.decode(errors='replace').splitlines() if 'undefined symbol:' in x]
print(json.dumps(dict(exit=q.returncode,libraries=len(libs),errors=errors),indent=2))

if q.returncode:raise SystemExit(q.returncode)
