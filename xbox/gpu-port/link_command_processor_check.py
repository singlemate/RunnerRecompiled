from pathlib import Path
import subprocess,json
r=Path.cwd();scope={};source=Path(__file__).with_name('compile_gpu_objects.py').read_text();exec(source[:source.index('def build')],scope)
cc=scope['cc'];out=r/'work/gpu-port';obj=out/'command-processor-check.o'
q=subprocess.run([str(cc),*scope['common'],'-c',str(r/'outputs/mirrors-edge-native/xbox/gpu-port/command_processor_check.cpp'),'-o',str(obj)],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace'));raise SystemExit(q.returncode)
objects=[]
for folder in ['objects','support','vulkan-bootstrap','integration-support']:objects+=sorted((out/folder).glob('*.o'))
q=subprocess.run([str(cc),'--target=aarch64-linux-android35',str(obj),*map(str,objects),'-Wl,--start-group',*map(str,(out/'deps-build').rglob('*.a')),'-Wl,--end-group','-static-libstdc++','-lvulkan','-landroid','-ldl','-Wl,--gc-sections','-Wl,--no-undefined','-Wl,--error-limit=0','-o',str(out/'command_processor_check')],capture_output=True)
(out/'command-processor-check-link.log').write_bytes(q.stdout+q.stderr)
print(json.dumps(dict(exit=q.returncode,errors=q.stderr.decode(errors='replace')[-4000:] if q.returncode else '')))
raise SystemExit(q.returncode)
