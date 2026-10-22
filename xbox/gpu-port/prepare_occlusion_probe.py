"""Opt-in visibility-query experiment, deliberately changes zero query results."""
from pathlib import Path
import subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd()
source=root/'work/references/rexglue-sdk/src/graphics/vulkan/command_processor.cpp';s=source.read_text()
if 'ME_DIAGNOSTIC_OCCLUSION_VISIBLE' not in s:
 anchor='  uint32_t clamped = samples > uint64_t(UINT32_MAX) ? UINT32_MAX : uint32_t(samples);'
 assert s.count(anchor)==1
 block='''  if(std::getenv("ME_DIAGNOSTIC_OCCLUSION_VISIBLE")){
    static unsigned observed=0;const auto original=samples;
    if(!samples)samples=1000;
    if(observed++<32)REXGPU_WARN("external_occlusion_visibility_probe address={:08X} actual={} returned={} not_a_fix=true",sample_count_address,original,samples);
  }
'''
 s=s.replace(anchor,block+anchor);source.write_text(s)
scope={};compiler=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(compiler[:compiler.index('def build')],scope)
q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(source),'-o',str(root/'work/gpu-port/objects/vulkan_command_processor.cpp.o')],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace')[-4000:])
raise SystemExit(q.returncode)
