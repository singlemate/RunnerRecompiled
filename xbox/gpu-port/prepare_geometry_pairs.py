"""Bounded opt-in per-draw geometry correspondence trace."""
from pathlib import Path
import subprocess
root=Path.cwd();p=root/'work/references/rexglue-sdk/src/graphics/vulkan/command_processor.cpp';s=p.read_text()
if 'ME_DIAGNOSTIC_GEOMETRY_PAIRS' not in s:
 anchor='  const uint32_t draw_depth=regs.Get<reg::RB_DEPTHCONTROL>().value;'
 block=r'''  if(std::getenv("ME_DIAGNOSTIC_GEOMETRY_PAIRS")){
    static unsigned geometry_sequence=0;
    static bool geometry_scene_started=false;
    geometry_scene_started |= draw_vs_hash==0xBF8AD3FD7A465985ull;
    if(geometry_scene_started && geometry_sequence<320){
      const unsigned sequence=++geometry_sequence;
      uint64_t constants_hash=14695981039346656037ull;
      for(unsigned i=0;i<36;++i){constants_hash^=regs[XE_GPU_REG_SHADER_CONSTANT_000_X+i];constants_hash*=1099511628211ull;}
      const auto position_fetch=regs.GetVertexFetch(95);
      REXGPU_INFO("external_geometry_pair sequence={} frame={} vs={:016X} ps={:016X} count={} ib={:08X} dma={:08X} offset={:08X} position={:08X},{:08X} c0_8={:016X} depth={:08X} target={:08X}",sequence,frame_current_,draw_vs_hash,draw_ps_hash,index_count,regs[XE_GPU_REG_VGT_DMA_BASE],regs[XE_GPU_REG_VGT_DMA_SIZE],regs[XE_GPU_REG_VGT_INDX_OFFSET],position_fetch.dword_0,position_fetch.dword_1,constants_hash,normalized_depth_control.value,regs.Get<reg::RB_COLOR_INFO>().value);
      for(const auto& binding:vertex_shader->vertex_bindings()){
        const auto fetch=regs.GetVertexFetch(binding.fetch_constant);
        REXGPU_INFO("external_geometry_binding sequence={} slot={} raw={:08X},{:08X}",sequence,binding.fetch_constant,fetch.dword_0,fetch.dword_1);
      }
    }
  }
'''
 assert s.count(anchor)==1;s=s.replace(anchor,block+anchor);p.write_text(s)
s=p.read_text()
if 'geometry_scene_started' not in s:
 s=s.replace('    static unsigned geometry_sequence=0;\n    if(geometry_sequence<320){','    static unsigned geometry_sequence=0;\n    static bool geometry_scene_started=false;\n    geometry_scene_started |= draw_vs_hash==0xBF8AD3FD7A465985ull;\n    if(geometry_scene_started && geometry_sequence<320){');p.write_text(s)
scope={};s=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(s[:s.index('def build')],scope)
q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(p),'-o',str(root/'work/gpu-port/objects/vulkan_command_processor.cpp.o')],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace')[-4000:])
raise SystemExit(q.returncode)
