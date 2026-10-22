from pathlib import Path
import concurrent.futures,json,re,subprocess,time
r=Path.cwd();sdk=r/'work/references/rexglue-sdk';out=r/'work/gpu-port/integration-support';out.mkdir(parents=True,exist_ok=True)
files=[sdk/"src"/name for name in ['core/timer_queue.cpp', 'ui/vulkan/ui_samplers.cpp', 'core/ring_buffer.cpp', 'core/byte_stream.cpp', 'core/clock.cpp', 'core/clock_posix.cpp', 'core/threading.cpp', 'core/threading_posix.cpp', 'core/mutex.cpp', 'core/atomic_posix.cpp', 'core/memory_posix.cpp', 'core/system_posix.cpp', 'ui/graphics_upload_buffer_pool.cpp', 'ui/graphics_util.cpp', 'ui/presenter.cpp', 'ui/immediate_drawer.cpp', 'ui/vulkan/linked_type_descriptor_set_allocator.cpp', 'ui/vulkan/single_layout_descriptor_set_pool.cpp', 'ui/vulkan/spirv_tools_context.cpp', 'ui/vulkan/vulkan_mem_alloc.cpp', 'ui/vulkan/vulkan_submission_tracker.cpp', 'ui/vulkan/vulkan_upload_buffer_pool.cpp', 'ui/vulkan/vulkan_util.cpp', 'ui/vulkan/vulkan_provider.cpp', 'ui/vulkan/vulkan_immediate_drawer.cpp', 'graphics/xenos.cpp', 'graphics/pipeline/texture/info.cpp', 'graphics/pipeline/texture/info_formats.cpp', 'graphics/pipeline/texture/extent.cpp', 'graphics/pipeline/texture/util.cpp', 'graphics/pipeline/texture/conversion.cpp']]
includes=[sdk/'include',sdk,sdk/'thirdparty',sdk/'thirdparty/fmt/include',sdk/'thirdparty/spdlog/include',sdk/'thirdparty/simde',sdk/'thirdparty/simde/simde',sdk/'thirdparty/xxHash',sdk/'thirdparty/vulkan-headers/include',sdk/'thirdparty/vulkan-memory-allocator/include',sdk/'thirdparty/glslang',sdk/'thirdparty/spirv-tools/include',sdk/'thirdparty/spirv-headers/include',sdk/'thirdparty/imgui',sdk/'thirdparty/utfcpp/source',sdk/'thirdparty/tomlplusplus/include',sdk/'thirdparty/sdl3/include',sdk/'thirdparty/renderdoc',sdk/'thirdparty/disruptorplus/include']
cc=r/'work/toolchains/android-ndk-r30/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
common=['--target=aarch64-linux-android35','-std=c++23','-O0','-fPIC','-ffunction-sections','-fdata-sections','-fno-char8_t','-fno-strict-aliasing','-DSIMDE_ENABLE_NATIVE_ALIASES','-DSPDLOG_FMT_EXTERNAL','-DNDEBUG','-DREX_GPU_EXTERNAL_HOST=1','-DREX_HAS_VULKAN=1','-DREX_HAS_D3D12=0','-DVK_USE_PLATFORM_ANDROID_KHR']+['-I'+str(p) for p in includes]
def build(p):
 name=str(p.relative_to(sdk/'src')).replace('\\','_').replace('/','_');obj=out/(name+'.o')
 q=subprocess.run([str(cc),*common,'-c',str(p),'-o',str(obj)],capture_output=True)
 log=q.stdout+q.stderr;(out/(name+'.log')).write_bytes(log)
 return dict(file=str(p.relative_to(sdk)),exit=q.returncode,bytes=obj.stat().st_size if not q.returncode else 0,error=log.decode(errors='replace')[-3500:] if q.returncode else '')
t=time.monotonic()
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:rows=list(pool.map(build,files))
report=dict(target='aarch64-linux-android35',units=len(rows),passed=sum(not x['exit'] for x in rows),elapsed_seconds=round(time.monotonic()-t,1),linked=False,results=rows)
(out.parent/'integration-support-result.json').write_text(json.dumps(report,indent=2))
print(json.dumps({k:v for k,v in report.items() if k!='results'}))
for x in rows:
 if x['exit']:print(x['file'],x['error'][-900:])

if report['passed'] != report['units']:raise SystemExit(1)
