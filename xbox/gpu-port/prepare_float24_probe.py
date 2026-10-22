"""Opt-in existing fragment float24 conversion path for A/B testing."""
from pathlib import Path
import subprocess
root=Path.cwd();p=root/'work/references/rexglue-sdk/src/graphics/vulkan/render_target_cache.cpp';s=p.read_text()
if 'ME_DIAGNOSTIC_FLOAT24' not in s:
 anchor='    depth_float24_convert_in_pixel_shader_ = REXCVAR_GET(depth_float24_convert_in_pixel_shader);'
 block='''
    if(std::getenv("ME_DIAGNOSTIC_FLOAT24")){
      depth_float24_round_=true;
      depth_float24_convert_in_pixel_shader_=true;
      REXGPU_WARN("external_float24_diagnostic round=true fragment_conversion=true not_a_fix=true");
    }
'''
 assert s.count(anchor)==1;s=s.replace(anchor,anchor+block);p.write_text(s)
scope={};s=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(s[:s.index('def build')],scope)
q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(p),'-o',str(root/'work/gpu-port/objects/vulkan_render_target_cache.cpp.o')],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace')[-4000:])
raise SystemExit(q.returncode)
