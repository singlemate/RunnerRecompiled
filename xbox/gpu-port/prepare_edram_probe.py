"""Record packed EDRAM before resolve conversion/clear; map after IssueCopy."""
from pathlib import Path
import subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd();sdk=root/'work/references/rexglue-sdk'
header=sdk/'include/rex/graphics/vulkan/diagnostic_edram_probe.h'
header.write_text((root/'outputs/mirrors-edge-native/xbox/gpu-port/diagnostic_edram_probe.h').read_text())
rtc=sdk/'src/graphics/vulkan/render_target_cache.cpp';s=rtc.read_text()
if 'ME_DIAGNOSTIC_EDRAM_SOURCE' not in s:
 s=s.replace('#include <algorithm>','#include <algorithm>\n#include <cstdlib>\n#include <rex/graphics/vulkan/diagnostic_edram_probe.h>',1)
 anchor='      uint32_t copy_dest_range_unscaled = resolve_info.copy_dest_extent_start -'
 assert s.count(anchor)==1
 block='''      auto& probe=me_edram_probe;
      const auto source=resolve_info.color_edram_info;
      if(std::getenv("ME_DIAGNOSTIC_EDRAM_SOURCE") && !direct_resolved && !draw_resolution_scaled &&
         !resolve_info.IsCopyingDepth() && source.format==3 && uint32_t(source.msaa_samples)<=2 &&
         !source.format_is_64bpp && resolve_info.copy_dest_base==0x128E0000u &&
         resolve_info.coordinate_info.width_div_8==160 && resolve_info.height_div_8>=64 &&
         !resolve_info.coordinate_info.edram_offset_x_div_8 && !resolve_info.coordinate_info.edram_offset_y_div_8 &&
         probe.buffer==VK_NULL_HANDLE && probe.count<12){
        const uint32_t xy[2][2]={{128,468},{640,180}};
        VkBufferCopy regions[8]{};
        probe.msaa=uint32_t(source.msaa_samples);
        const unsigned lanes=1u<<probe.msaa;probe.word_count=2*lanes;
        probe.select=uint32_t(resolve_info.copy_dest_coordinate_info.copy_sample_select);
        for(unsigned point=0;point<2;++point)for(unsigned lane=0;lane<lanes;++lane){
          const unsigned i=point*lanes+lane;
          const uint32_t sx=(xy[point][0]<<unsigned(probe.msaa>=2))+((lane>>1)&1u);
          const uint32_t sy=(xy[point][1]<<unsigned(probe.msaa>=1))+(lane&1u);
          const uint32_t tile=(uint32_t(source.base_tiles)+(sy/16)*uint32_t(source.pitch_tiles)+sx/80)&2047u;
          probe.offsets[i]=(tile*1280+(sy%16)*80+sx%80)*4;
          regions[i]={probe.offsets[i],i*4,4};
        }
        if(ui::vulkan::util::CreateDedicatedAllocationBuffer(vulkan_device,32,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kReadback,probe.buffer,probe.memory,&probe.memory_type)){
          probe.frame=command_processor_.GetCurrentFrame();probe.color=register_file().Get<reg::RB_COLOR_INFO>().value;++probe.count;
          UseEdramBuffer(EdramBufferUsage::kTransferRead);command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkCopyBuffer(edram_buffer_,probe.buffer,probe.word_count,regions);
          command_processor_.PushBufferMemoryBarrier(probe.buffer,0,probe.word_count*4,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
        }
      }

'''
 s=s.replace(anchor,block+anchor);s=s.replace('if (REXCVAR_GET(direct_host_resolve)) {','if (REXCVAR_GET(direct_host_resolve) && !std::getenv("ME_DIAGNOSTIC_EDRAM_SOURCE")) {');rtc.write_text(s)
cp=sdk/'src/graphics/vulkan/command_processor.cpp';s=cp.read_text()
if 'external_edram_lane' not in s:
 s=s.replace('#include <cstdio>','#include <cstdio>\n#include <rex/graphics/vulkan/diagnostic_edram_probe.h>',1)
 anchor='    static unsigned resolve_point_samples=0;'
 assert s.count(anchor)==1
 block='''    auto& edram_probe=me_edram_probe;
    if(edram_probe.buffer!=VK_NULL_HANDLE){
      const auto* vd=GetVulkanDevice();const auto& df=vd->functions();const auto dev=vd->device();
      if(!AwaitAllQueueOperationsCompletion())return false;
      void* mapped=nullptr;
      if(df.vkMapMemory(dev,edram_probe.memory,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS){
        if(!(vd->memory_types().host_coherent&(uint32_t(1)<<edram_probe.memory_type))){
          VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=edram_probe.memory;range.size=VK_WHOLE_SIZE;
          df.vkInvalidateMappedMemoryRanges(dev,1,&range);
        }
        uint32_t words[8];std::memcpy(words,mapped,edram_probe.word_count*4);
        const unsigned lanes=1u<<edram_probe.msaa;
        for(unsigned i=0;i<edram_probe.word_count;++i)
          REXGPU_INFO("external_edram_lane sample={} frame={} color={:08X} msaa={} select={} point={} lane={} offset={:08X} packed={:08X}",
            edram_probe.count,edram_probe.frame,edram_probe.color,lanes,edram_probe.select,i/lanes,i%lanes,edram_probe.offsets[i],words[i]);
        df.vkUnmapMemory(dev,edram_probe.memory);
      }
      df.vkDestroyBuffer(dev,edram_probe.buffer,nullptr);df.vkFreeMemory(dev,edram_probe.memory,nullptr);
      edram_probe.buffer=VK_NULL_HANDLE;edram_probe.memory=VK_NULL_HANDLE;
    }
'''
 s=s.replace(anchor,block+anchor);cp.write_text(s)
scope={};compiler=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(compiler[:compiler.index('def build')],scope)
for source,name in [(rtc,'vulkan_render_target_cache.cpp.o'),(cp,'vulkan_command_processor.cpp.o')]:
 q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(source),'-o',str(root/'work/gpu-port/objects'/name)],capture_output=True)
 if q.returncode:print(q.stderr.decode(errors='replace')[-4000:]);raise SystemExit(q.returncode)
