"""Opt-in bounded resolve metadata. No readback or rendering change."""
from pathlib import Path
import subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd()
source=root/'work/references/rexglue-sdk/src/graphics/vulkan/command_processor.cpp';s=source.read_text()
if 'ME_DIAGNOSTIC_RESOLVE_TRACE' not in s:
 old='''    return render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                         written_address, written_length);'''
 assert s.count(old)==1
 new='''    const bool resolved=render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,written_address,written_length);
    if(resolved && written_length && std::getenv("ME_DIAGNOSTIC_RESOLVE_TRACE")){
      const auto& regs=*register_file_;
      const auto color=regs.Get<reg::RB_COLOR_INFO>();
      const auto info=regs.Get<reg::RB_COPY_DEST_INFO>();
      const auto pitch=regs.Get<reg::RB_COPY_DEST_PITCH>();
      static std::set<std::array<uint32_t,5>> observed;
      std::array<uint32_t,5> key{written_address,written_length,color.value,info.value,pitch.value};
      if(observed.size()<128 && observed.insert(key).second)
        REXGPU_INFO("external_resolve_trace frame={} address={:08X} length={} source_color={:08X} source_depth={:08X} copy_control={:08X} dest_info={:08X} dest_format={} dest_endian={} dest_exp_bias={} pitch={} height={}",
          frame_current_,written_address,written_length,color.value,regs.Get<reg::RB_DEPTH_INFO>().value,regs.Get<reg::RB_COPY_CONTROL>().value,
          info.value,uint32_t(info.copy_dest_format),uint32_t(info.copy_dest_endian),int32_t(info.copy_dest_exp_bias),uint32_t(pitch.copy_dest_pitch),uint32_t(pitch.copy_dest_height));
    }
    return resolved;'''
 s=s.replace(old,new);source.write_text(s)
scope={};compiler=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(compiler[:compiler.index('def build')],scope)
q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(source),'-o',str(root/'work/gpu-port/objects/vulkan_command_processor.cpp.o')],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace')[-4000:])
raise SystemExit(q.returncode)
