from pathlib import Path
import concurrent.futures,json,re,subprocess,time
r=Path.cwd();sdk=r/'work/references/rexglue-sdk';out=r/'work/gpu-port/vulkan-bootstrap';out.mkdir(parents=True,exist_ok=True)
files=[sdk/'src'/x for x in ['ui/vulkan/vulkan_instance.cpp','ui/vulkan/vulkan_device.cpp','ui/vulkan/vulkan_presenter.cpp','ui/vulkan/vulkan_presenter_window.cpp','ui/vulkan/vulkan_moltenvk.cpp','ui/renderdoc_api.cpp','core/dynlib_posix.cpp']]
includes=[sdk/'include',sdk,sdk/'thirdparty',sdk/'thirdparty/fmt/include',sdk/'thirdparty/spdlog/include',sdk/'thirdparty/simde',sdk/'thirdparty/simde/simde',sdk/'thirdparty/xxHash',sdk/'thirdparty/vulkan-headers/include',sdk/'thirdparty/vulkan-memory-allocator/include',sdk/'thirdparty/glslang',sdk/'thirdparty/spirv-tools/include',sdk/'thirdparty/spirv-headers/include',sdk/'thirdparty/imgui',sdk/'thirdparty/utfcpp/source',sdk/'thirdparty/tomlplusplus/include',sdk/'thirdparty/sdl3/include',sdk/'thirdparty/renderdoc']
cc=r/'work/toolchains/android-ndk-r30/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
common=['--target=aarch64-linux-android35','-std=c++23','-O2','-fPIC','-ffunction-sections','-fdata-sections','-fno-char8_t','-fno-strict-aliasing','-DSIMDE_ENABLE_NATIVE_ALIASES','-DSPDLOG_FMT_EXTERNAL','-DNDEBUG','-DREX_GPU_EXTERNAL_HOST=1','-DREX_HAS_VULKAN=1','-DREX_HAS_D3D12=0','-DVK_USE_PLATFORM_ANDROID_KHR']+['-I'+str(p) for p in includes]
def build(p):
 name=str(p.relative_to(sdk/'src')).replace('\\','_').replace('/','_');obj=out/(name+'.o')
 q=subprocess.run([str(cc),*common,'-c',str(p),'-o',str(obj)],capture_output=True)
 log=q.stdout+q.stderr;(out/(name+'.log')).write_bytes(log)
 return dict(file=str(p.relative_to(sdk)),exit=q.returncode,bytes=obj.stat().st_size if not q.returncode else 0,error=log.decode(errors='replace')[-3500:] if q.returncode else '')
t=time.monotonic()
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:rows=list(pool.map(build,files))
report=dict(target='aarch64-linux-android35',units=len(rows),passed=sum(not x['exit'] for x in rows),elapsed_seconds=round(time.monotonic()-t,1),linked=False,results=rows)
(out.parent/'vulkan-bootstrap-result.json').write_text(json.dumps(report,indent=2))
print(json.dumps({k:v for k,v in report.items() if k!='results'}))
for x in rows:
 if x['exit']:print(x['file'],x['error'][-900:])

if report['passed'] != report['units']:raise SystemExit(1)
