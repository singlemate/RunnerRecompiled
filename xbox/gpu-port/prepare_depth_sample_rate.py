"""Opt-in explicit sample shading for synthesized float24 depth-only shaders."""
from pathlib import Path
import subprocess
root=Path.cwd();p=root/'work/references/rexglue-sdk/src/graphics/vulkan/pipeline_cache.cpp';s=p.read_text()
if 'ME_DIAGNOSTIC_DEPTH_SAMPLE_RATE' not in s:
 s='#include <cstdlib>\n'+s
 anchor='  description_out.render_pass_key = render_pass_key;'
 block='''  if(!pixel_shader && std::getenv("ME_DIAGNOSTIC_DEPTH_SAMPLE_RATE") &&
      render_target_cache_.GetPath()==RenderTargetCache::Path::kHostRenderTargets &&
      render_target_cache_.depth_float24_convert_in_pixel_shader() &&
      (render_pass_key.depth_and_color_used&1) &&
      render_pass_key.depth_format==xenos::DepthRenderTargetFormat::kD24FS8 &&
      (normalized_depth_control.z_enable || normalized_depth_control.z_write_enable)){
    description_out.sample_rate_shading=1;
    static unsigned count=0;
    if(++count<=4)REXGPU_WARN("external_depth_sample_rate_explicit enabled=true not_a_fix=true");
  }
'''
 assert s.count(anchor)==1;s=s.replace(anchor,block+anchor);p.write_text(s)
scope={};s=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(s[:s.index('def build')],scope)
q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(p),'-o',str(root/'work/gpu-port/objects/vulkan_pipeline_cache.cpp.o')],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace')[-4000:])
raise SystemExit(q.returncode)
