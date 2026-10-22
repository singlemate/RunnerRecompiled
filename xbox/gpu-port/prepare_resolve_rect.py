"""Bounded original resolve rectangle metadata before packed coordinate reduction."""
from pathlib import Path
import subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd()
source=root/'work/references/rexglue-sdk/src/graphics/util/draw.cpp';s=source.read_text()
if 'ME_DIAGNOSTIC_RESOLVE_RECT' not in s:
 s=s.replace('#include <cmath>','#include <cmath>\n#include <cstdlib>',1)
 anchor='  info_out.copy_dest_base = copy_dest_base_adjusted;'
 assert s.count(anchor)==1
 block='''  static unsigned external_hdr_rect_index=0;
  if(std::getenv("ME_DIAGNOSTIC_RESOLVE_RECT") && uint32_t(rb_copy_dest_info.copy_dest_format)==32 && rb_copy_dest_pitch.copy_dest_pitch==1280 && external_hdr_rect_index<64){
    REXGPU_INFO("external_resolve_rect hdr_index={} raw_base={:08X} adjusted_base={:08X} extent_start={:08X} extent_length={} x0={} y0={} x1={} y1={} dest_offset_x={} dest_offset_y={} pitch={} height={} source_color={:08X} source_depth={:08X}",
      ++external_hdr_rect_index,rb_copy_dest_base,copy_dest_base_adjusted,copy_dest_extent_start,copy_dest_extent_end-copy_dest_extent_start,
      x0,y0,x1,y1,uint32_t(info_out.copy_dest_coordinate_info.offset_x_div_8)<<3,uint32_t(info_out.copy_dest_coordinate_info.offset_y_div_8)<<3,
      uint32_t(rb_copy_dest_pitch.copy_dest_pitch),uint32_t(rb_copy_dest_pitch.copy_dest_height),regs.Get<reg::RB_COLOR_INFO>().value,regs.Get<reg::RB_DEPTH_INFO>().value);
  }
'''
 s=s.replace(anchor,block+anchor);source.write_text(s)
scope={};compiler=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(compiler[:compiler.index('def build')],scope)
q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(source),'-o',str(root/'work/gpu-port/objects/util_draw.cpp.o')],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace')[-4000:])
raise SystemExit(q.returncode)
