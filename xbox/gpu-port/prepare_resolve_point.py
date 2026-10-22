"""Opt-in sequential GPU samples after resolves intersecting an explicit address."""
from pathlib import Path
import subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd()
source=root/'work/references/rexglue-sdk/src/graphics/vulkan/command_processor.cpp';s=source.read_text()
if 'ME_DIAGNOSTIC_RESOLVE_POINT' not in s:
 anchor='    if(resolved && written_length && std::getenv("ME_DIAGNOSTIC_RESOLVE_TRACE")){'
 assert s.count(anchor)==1
 block='''    static unsigned resolve_point_samples=0;
    static uint64_t resolve_point_sequence=0;
    ++resolve_point_sequence;
    const char* point_text=std::getenv("ME_DIAGNOSTIC_RESOLVE_POINT");
    if(resolved && written_length && resolve_point_samples<16 && point_text && std::strlen(point_text)==8){
      char* end=nullptr;const uint64_t point=std::strtoull(point_text,&end,16);
      if(end && !*end && point<=0x1ffffff8ull && point>=written_address && point+8<=uint64_t(written_address)+written_length){
        const auto* vd=GetVulkanDevice();const auto& df=vd->functions();const auto dev=vd->device();
        VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory allocation=VK_NULL_HANDLE;uint32_t memory_type=UINT32_MAX;
        const char* control_text=std::getenv("ME_DIAGNOSTIC_RESOLVE_CONTROL");
        char* control_end=nullptr;
        const uint64_t control=control_text && std::strlen(control_text)==8 ? std::strtoull(control_text,&control_end,16) : point;
        const bool control_valid=control_end && !*control_end && control<=0x1ffffff8ull && control>=written_address && control+8<=uint64_t(written_address)+written_length;
        if(ui::vulkan::util::CreateDedicatedAllocationBuffer(vd,16,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kReadback,buffer,allocation,&memory_type)){
          shared_memory_->Use(VulkanSharedMemory::Usage::kRead);SubmitBarriers(true);
          VkBufferCopy regions[2]{{point,0,8},{control_valid?control:point,8,8}};deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),buffer,2,regions);
          PushBufferMemoryBarrier(buffer,0,16,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
          if(!AwaitAllQueueOperationsCompletion())return false;
          void* mapped=nullptr;
          if(df.vkMapMemory(dev,allocation,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS){
            if(!(vd->memory_types().host_coherent&(uint32_t(1)<<memory_type))){
              VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=allocation;range.size=VK_WHOLE_SIZE;
              df.vkInvalidateMappedMemoryRanges(dev,1,&range);
            }
            const auto* p=static_cast<const uint8_t*>(mapped);const auto& regs=*register_file_;
            REXGPU_INFO("external_resolve_point sample={} sequence={} frame={} point={:08X} written_address={:08X} length={} color={:08X} copy_control={:08X} dest_info={:08X} bytes={:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
              ++resolve_point_samples,resolve_point_sequence,frame_current_,uint32_t(point),written_address,written_length,
              regs.Get<reg::RB_COLOR_INFO>().value,regs.Get<reg::RB_COPY_CONTROL>().value,regs.Get<reg::RB_COPY_DEST_INFO>().value,
              p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
            if(control_valid)REXGPU_INFO("external_resolve_control sample={} sequence={} frame={} point={:08X} bytes={:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
              resolve_point_samples,resolve_point_sequence,frame_current_,uint32_t(control),p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
            df.vkUnmapMemory(dev,allocation);
          }
          df.vkDestroyBuffer(dev,buffer,nullptr);df.vkFreeMemory(dev,allocation,nullptr);
        }
      }
    }
'''
 s=s.replace(anchor,block+anchor);source.write_text(s)
scope={};compiler=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(compiler[:compiler.index('def build')],scope)
q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(source),'-o',str(root/'work/gpu-port/objects/vulkan_command_processor.cpp.o')],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace')[-4000:])
raise SystemExit(q.returncode)
