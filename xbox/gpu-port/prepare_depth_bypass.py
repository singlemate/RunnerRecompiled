"""Opt-in per-pixel-shader depth/stencil bypass; diagnostic only."""
from pathlib import Path
import subprocess
root=Path.cwd();p=root/'work/references/rexglue-sdk/src/graphics/vulkan/command_processor.cpp';s=p.read_text()
anchor='  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);'
if 'ME_DIAGNOSTIC_DEPTH_BYPASS_PS' not in s:
 assert s.count(anchor)==1
 s=s.replace(anchor,anchor+'''
  if(const char* diagnostic_ps=std::getenv("ME_DIAGNOSTIC_DEPTH_BYPASS_PS")){
    char* end=nullptr;const uint64_t wanted=std::strtoull(diagnostic_ps,&end,16);
    if(end && !*end && pixel_shader && pixel_shader->ucode_data_hash()==wanted){
      normalized_depth_control.z_enable=0;
      normalized_depth_control.z_write_enable=0;
      normalized_depth_control.stencil_enable=0;
      static unsigned diagnostic_count=0;
      if(++diagnostic_count<=4)REXGPU_WARN("external_depth_bypass ps={:016X} not_a_fix=true",wanted);
    }
  }
''');p.write_text(s)
scope={};s=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(s[:s.index('def build')],scope)
subprocess.run([str(scope['cc']),*scope['common'],'-c',str(p),'-o',str(root/'work/gpu-port/objects/vulkan_command_processor.cpp.o')],check=True)
