"""Bounded HDR RGBA16F GPU samples, no guest writes or draw changes."""
from pathlib import Path
import subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd()
source=root/'work/references/rexglue-sdk/src/graphics/vulkan/command_processor.cpp'
s=source.read_text()
if 'ME_DIAGNOSTIC_HDR_READBACK' not in s:
 s=s.replace('#include <cstdio>','#include <cstdio>\n#include <rex/graphics/pipeline/texture/util.h>',1)
 anchor='  return true;\n}\n\nbool VulkanCommandProcessor::IssueDraw_MemexportReadbackFullPath'
 assert s.count(anchor)==1
 block='''  static unsigned hdr_samples=0;
  if(draw_ps_hash==0x8F5851C1A202047Bull && hdr_samples<3 && std::getenv("ME_DIAGNOSTIC_HDR_READBACK")){
    const auto fetch=regs.GetTextureFetch(1);
    const uint32_t w=fetch.size_2d.width+1,h=fetch.size_2d.height+1,pitch=fetch.pitch<<5;
    if(uint32_t(fetch.format)==32 && w>=32 && h>=32 && pitch>=w){
      const uint32_t xy[5][2]={{w/10,h*13/20},{w/2,h/4},{w/2,h*4/5},{w*9/10,h*13/20},{w/2,h/2}};
      VkBufferCopy regions[5]{};bool valid=true;
      for(unsigned i=0;i<5;++i){
        const int64_t offset=fetch.tiled ? texture_util::GetTiledOffset2D(xy[i][0],xy[i][1],pitch,3) : int64_t(xy[i][1])*pitch*8+xy[i][0]*8;
        const int64_t address=(int64_t(fetch.base_address)<<12)+offset;
        if(address<0 || address>0x1ffffff8)valid=false;
        regions[i]={VkDeviceSize(address),i*8,8};
      }
      const auto* vd=GetVulkanDevice();const auto& df=vd->functions();const auto dev=vd->device();
      VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory allocation=VK_NULL_HANDLE;uint32_t memory_type=UINT32_MAX;
      if(valid && ui::vulkan::util::CreateDedicatedAllocationBuffer(vd,40,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback,buffer,allocation,&memory_type)){
        shared_memory_->Use(VulkanSharedMemory::Usage::kRead);SubmitBarriers(true);
        deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),buffer,5,regions);
        PushBufferMemoryBarrier(buffer,0,40,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
        if(!AwaitAllQueueOperationsCompletion())return false;
        void* mapped=nullptr;
        if(df.vkMapMemory(dev,allocation,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS){
          if(!(vd->memory_types().host_coherent&(uint32_t(1)<<memory_type))){
            VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=allocation;range.size=VK_WHOLE_SIZE;
            df.vkInvalidateMappedMemoryRanges(dev,1,&range);
          }
          ++hdr_samples;const auto* bytes=static_cast<const uint8_t*>(mapped);
          for(unsigned i=0;i<5;++i){const auto* p=bytes+i*8;
            REXGPU_INFO("external_hdr_gpu sample={} frame={} width={} height={} pitch={} tiled={} endian={} x={} y={} address={:08X} bytes={:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
              hdr_samples,frame_current_,w,h,pitch,uint32_t(fetch.tiled),uint32_t(fetch.endianness),xy[i][0],xy[i][1],uint32_t(regions[i].srcOffset),p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
          }
          df.vkUnmapMemory(dev,allocation);
        }
        df.vkDestroyBuffer(dev,buffer,nullptr);df.vkFreeMemory(dev,allocation,nullptr);
      }
    }
  }
'''
 s=s.replace(anchor,block+anchor);source.write_text(s)
scope={};compiler=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(compiler[:compiler.index('def build')],scope)
result=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(source),'-o',str(root/'work/gpu-port/objects/vulkan_command_processor.cpp.o')],capture_output=True)
if result.returncode:print(result.stderr.decode(errors='replace')[-4000:])
raise SystemExit(result.returncode)
