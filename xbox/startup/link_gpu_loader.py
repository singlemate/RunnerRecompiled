from pathlib import Path
import subprocess,os,sys
r=Path(sys.argv[1]).resolve();os.chdir(r)
scope={};script=r/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py';source=script.read_text();exec(source[:source.index('def build')],scope)
cc=scope['cc'];out=r/'work/startup/current';obj=out/'gpu-bridge.o'
q=subprocess.run([str(cc),*scope['common'],'-c',str(r/'outputs/mirrors-edge-native/xbox/startup/gpu_bridge.cpp'),'-o',str(obj)],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace'));raise SystemExit(q.returncode)
gpu=r/'work/gpu-port';objects=[]
for folder in ['objects','support','vulkan-bootstrap','integration-support']:objects+=sorted((gpu/folder).glob('*.o'))
# XMA decoder (xbox/audio/build_xma.py) and its FFmpeg (build_ffmpeg_android.py).
audio=r/'work/audio';objects+=sorted((audio/'xma').glob('*.o'))+[audio/'ffmpeg/libavcodec.a',audio/'ffmpeg/libavutil.a']
def link(entry,output,extra):
 return subprocess.run([str(cc),'--target=aarch64-linux-android35',*extra,str(out/entry),str(out/'entry.o'),str(out/'imports.o'),str(obj),*map(str,objects),str(r/'work/xbox-game-private/libmirrorsedge_ppc_mmio.a'),'-Wl,--start-group',*map(str,(gpu/'deps-build').rglob('*.a')),'-Wl,--end-group','-static-libstdc++','-lvulkan','-laaudio','-landroid','-llog','-ldl','-lm','-Wl,--gc-sections','-Wl,--no-undefined','-Wl,--error-limit=0','-o',str(out/output)],capture_output=True)
# ADB executable (self-tests, automated routes) and the in-app library (never in the APK).
q=link('exe-main.o','me_memory_probe',[])
(out/'gpu-link.log').write_bytes(q.stdout+q.stderr)
if q.returncode:print(q.stderr.decode(errors='replace')[-6000:]);raise SystemExit(q.returncode)
q=link('jni-main.o','libmeruntime.so',['-shared','-Wl,-soname,libmeruntime.so','-Wl,-z,max-page-size=16384'])
(out/'gpu-link-so.log').write_bytes(q.stdout+q.stderr)
if q.returncode:print(q.stderr.decode(errors='replace')[-6000:])
raise SystemExit(q.returncode)
