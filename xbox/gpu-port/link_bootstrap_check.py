from pathlib import Path
import subprocess,json
r=Path.cwd();scope={};source=Path(__file__).with_name('compile_gpu_objects.py').read_text();exec(source[:source.index('def build')],scope)
cc=scope['cc'];out=r/'work/gpu-port';obj=out/'bootstrap-check.o'
q=subprocess.run([str(cc),*scope['common'],'-c',str(r/'outputs/mirrors-edge-native/xbox/gpu-port/vulkan_bootstrap_check.cpp'),'-o',str(obj)],capture_output=True)
assert not q.returncode,q.stderr.decode(errors='replace')
libs=list((out/'deps-build').rglob('*.a'))
args=[str(cc),'--target=aarch64-linux-android35',str(obj),*map(str,sorted((out/'support').glob('*.o'))),*map(str,sorted((out/'vulkan-bootstrap').glob('*.o'))),'-Wl,--start-group',*map(str,libs),'-Wl,--end-group','-static-libstdc++','-ldl','-Wl,--gc-sections','-Wl,--error-limit=0','-o',str(out/'vulkan_bootstrap_check')]
q=subprocess.run(args,capture_output=True);(out/'bootstrap-check-link.log').write_bytes(q.stderr+q.stdout)
print(json.dumps(dict(exit=q.returncode,errors=[x for x in q.stderr.decode(errors='replace').splitlines() if 'undefined symbol:' in x]),indent=2))

if q.returncode:raise SystemExit(q.returncode)
