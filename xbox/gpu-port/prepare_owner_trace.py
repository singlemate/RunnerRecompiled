"""Observe actual host render target owners for four known diagnostic tiles."""
from pathlib import Path
import subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd()
source=root/'work/references/rexglue-sdk/src/graphics/vulkan/render_target_cache.cpp';s=source.read_text()
if 'ME_DIAGNOSTIC_OWNER_TRACE' not in s:
 anchor='  // Clear previously set temporary indices.'
 assert s.count(anchor)==1
 block='''  if(std::getenv("ME_DIAGNOSTIC_OWNER_TRACE")){
    static unsigned owner_observations=0;
    const uint32_t wanted_tiles[]={1185,904,1953,1384};
    for(const auto& rectangle:dump_rectangles_){
      auto* rt=static_cast<VulkanRenderTarget*>(rectangle.render_target);if(!rt)continue;
      const auto key=rt->key();if(key.is_depth || key.msaa_samples==xenos::MsaaSamples::k1X)continue;
      ResolveCopyDumpRectangle::Dispatch dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
      const auto count=rectangle.GetDispatches(dump_pitch,dump_row_length_used,dispatches);
      for(unsigned t:wanted_tiles){
        bool covered=false;
        for(unsigned d=0;d<count;++d)for(unsigned row=0;row<dispatches[d].height_tiles;++row){
          const uint32_t first=dump_base+dispatches[d].offset+row*dump_pitch;
          covered|=((t-first)&2047u)<dispatches[d].width_tiles;
        }
        if(!covered || owner_observations>=96)continue;
        const unsigned guest_samples=1u<<uint32_t(key.msaa_samples);
        const unsigned host_samples=guest_samples==2 && !msaa_2x_attachments_supported_ ? 4:guest_samples;
        REXGPU_INFO("external_host_owner index={} frame={} tile={} owner_key={:08X} base_tiles={} pitch_tiles={} guest_samples={} host_samples={} format={} vk_format={} width={} height={} layout={}",
          ++owner_observations,command_processor_.GetCurrentFrame(),t,key.key,uint32_t(key.base_tiles),key.GetPitchTiles(),guest_samples,host_samples,
          uint32_t(key.resource_format),uint32_t(GetColorVulkanFormat(key.GetColorFormat())),key.GetWidth()*draw_resolution_scale_x(),
          GetRenderTargetHeight(key.pitch_tiles_at_32bpp,key.msaa_samples)*draw_resolution_scale_y(),uint32_t(rt->current_layout()));
      }
    }
  }

'''
 s=s.replace(anchor,block+anchor);source.write_text(s)
scope={};compiler=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(compiler[:compiler.index('def build')],scope)
q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(source),'-o',str(root/'work/gpu-port/objects/vulkan_render_target_cache.cpp.o')],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace')[-4000:])
raise SystemExit(q.returncode)
