from pathlib import Path
import subprocess,json
r=Path.cwd();scope={};source=Path(__file__).with_name('compile_gpu_objects.py').read_text();exec(source[:source.index('def build')],scope)
cc=scope['cc'];out=r/'work/gpu-port';obj=out/'host-services-check.o'
q=subprocess.run([str(cc),*scope['common'],'-c',str(Path(__file__).with_name('host_services_check.cpp')),'-o',str(obj)],capture_output=True)
assert not q.returncode,q.stderr.decode(errors='replace')
objects=sorted((out/'support').glob('*.o'))+sorted((out/'integration-support').glob('core_*.o'))
q=subprocess.run([str(cc),'--target=aarch64-linux-android35',str(obj),*map(str,objects),'-Wl,--start-group',*map(str,(out/'deps-build').rglob('*.a')),'-Wl,--end-group','-static-libstdc++','-landroid','-ldl','-Wl,--gc-sections','-Wl,--error-limit=0','-o',str(out/'host_services_check')],capture_output=True)
(out/'host-services-link.log').write_bytes(q.stderr+q.stdout)
print(json.dumps({'exit':q.returncode,'errors':[x for x in q.stderr.decode(errors='replace').splitlines() if 'undefined symbol:' in x]}))
if q.returncode:raise SystemExit(q.returncode)
