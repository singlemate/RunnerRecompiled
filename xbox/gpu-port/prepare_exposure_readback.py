"""Bounded GPU shared-buffer readback for exposure diagnosis; no guest writes."""
from pathlib import Path
import subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd()
source=root/'work/references/rexglue-sdk/src/graphics/vulkan/command_processor.cpp'
s=source.read_text()
if 'ME_DIAGNOSTIC_EXPOSURE_READBACK' not in s:
 anchor='  return true;\n}\n\nbool VulkanCommandProcessor::IssueDraw_MemexportReadbackFullPath'
 assert s.count(anchor)==1
 block='''  static unsigned exposure_samples=0;
  if(draw_ps_hash==0x80C8053F5C552351ull && exposure_samples<8 && std::getenv("ME_DIAGNOSTIC_EXPOSURE_READBACK")){
    const auto fetch=regs.GetTextureFetch(1);
    const uint32_t source_address=fetch.base_address<<12;
    if(fetch.size_2d.width==0 && fetch.size_2d.height==0 && source_address<=0x1ffffff0u){
      const auto* vd=GetVulkanDevice();const auto& df=vd->functions();const auto dev=vd->device();
      VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory allocation=VK_NULL_HANDLE;
      uint32_t memory_type=UINT32_MAX;VkDeviceSize allocation_size=0;
      if(ui::vulkan::util::CreateDedicatedAllocationBuffer(vd,16,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback,buffer,allocation,&memory_type,&allocation_size)){
        shared_memory_->Use(VulkanSharedMemory::Usage::kRead);SubmitBarriers(true);
        VkBufferCopy region{source_address,0,16};
        deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),buffer,1,&region);
        PushBufferMemoryBarrier(buffer,0,16,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
        if(!AwaitAllQueueOperationsCompletion())return false;
        void* mapped=nullptr;
        if(df.vkMapMemory(dev,allocation,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS){
          if(!(vd->memory_types().host_coherent&(uint32_t(1)<<memory_type))){
            VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=allocation;range.size=VK_WHOLE_SIZE;
            df.vkInvalidateMappedMemoryRanges(dev,1,&range);
          }
          const auto* bytes=static_cast<const uint8_t*>(mapped);
          REXGPU_INFO("external_exposure_gpu sample={} frame={} address={:08X} format={} endian={} tiled={} bytes={:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
            ++exposure_samples,frame_current_,source_address,uint32_t(fetch.format),uint32_t(fetch.endianness),uint32_t(fetch.tiled),bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7]);
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
