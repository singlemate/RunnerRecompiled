"""Sample actual host MSAA color before DumpRenderTargets, opt-in only."""
from pathlib import Path
import struct,subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd();sdk=root/'work/references/rexglue-sdk'
inc=sdk/'include/rex/graphics/vulkan'
(inc/'diagnostic_edram_probe.h').write_text((root/'outputs/mirrors-edge-native/xbox/gpu-port/diagnostic_edram_probe.h').read_text())
data=(root/'work/gpu-port/host_msaa_probe.spv').read_bytes();words=struct.unpack('<'+'I'*(len(data)//4),data)
(inc/'diagnostic_host_shader.h').write_text('#pragma once\ninline constexpr uint32_t me_host_probe_spv[]={'+','.join(hex(x) for x in words)+'};\n')
rtc=sdk/'src/graphics/vulkan/render_target_cache.cpp';s=rtc.read_text()
if 'ME_DIAGNOSTIC_HOST_COLOR' not in s:
 s=s.replace('#include <rex/graphics/vulkan/diagnostic_edram_probe.h>','#include <rex/graphics/vulkan/diagnostic_edram_probe.h>\n#include <rex/graphics/vulkan/diagnostic_host_shader.h>',1)
 anchor='  // Sort the invocations to reduce context and binding switches.'
 assert s.count(anchor)==1
 block='''  auto& host_probe=me_host_probe;
  if(std::getenv("ME_DIAGNOSTIC_HOST_COLOR") && !IsDrawResolutionScaled() && host_probe.count<8 && host_probe.buffer==VK_NULL_HANDLE){
    for(const auto& rectangle:dump_rectangles_){
      auto* rt=static_cast<VulkanRenderTarget*>(rectangle.render_target);if(!rt)continue;
      const auto key=rt->key();
      if(key.is_depth || key.base_tiles!=1024 || key.GetPitchTiles()!=16 || key.resource_format!=3 || key.msaa_samples!=xenos::MsaaSamples::k2X)continue;
      ResolveCopyDumpRectangle::Dispatch dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
      const auto n=rectangle.GetDispatches(dump_pitch,dump_row_length_used,dispatches);
      auto covers=[&](uint32_t t){for(unsigned d=0;d<n;++d)for(unsigned y=0;y<dispatches[d].height_tiles;++y)
        if(((t-(dump_base+dispatches[d].offset+y*dump_pitch))&2047u)<dispatches[d].width_tiles)return true;return false;};
      if(!covers(1953) || !covers(1384))continue;
      const auto* vd=command_processor_.GetVulkanDevice();const auto& df=vd->functions();const auto dev=vd->device();
      if(host_probe.layout==VK_NULL_HANDLE){
        VkDescriptorSetLayout layouts[]={descriptor_set_layout_sampled_image_,command_processor_.GetSingleTransientDescriptorLayout(VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute)};
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,24};
        VkPipelineLayoutCreateInfo info{};info.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;info.setLayoutCount=2;info.pSetLayouts=layouts;info.pushConstantRangeCount=1;info.pPushConstantRanges=&range;
        if(df.vkCreatePipelineLayout(dev,&info,nullptr,&host_probe.layout)!=VK_SUCCESS)break;
        host_probe.pipeline=ui::vulkan::util::CreateComputePipeline(vd,host_probe.layout,me_host_probe_spv,sizeof(me_host_probe_spv));
      }
      if(host_probe.pipeline==VK_NULL_HANDLE)break;
      const auto dest=command_processor_.AllocateSingleTransientDescriptor(VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute);
      if(dest==VK_NULL_HANDLE)break;
      if(!ui::vulkan::util::CreateDedicatedAllocationBuffer(vd,64,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,ui::vulkan::util::MemoryPurpose::kReadback,host_probe.buffer,host_probe.memory,&host_probe.memory_type))break;
      VkDescriptorBufferInfo binfo{host_probe.buffer,0,64};VkWriteDescriptorSet write{};write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=dest;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&binfo;df.vkUpdateDescriptorSets(dev,1,&write,0,nullptr);
      host_probe.host_ids[0]=draw_util::GetD3D10SampleIndexForGuest2xMSAA(0,msaa_2x_attachments_supported_);
      host_probe.host_ids[1]=draw_util::GetD3D10SampleIndexForGuest2xMSAA(1,msaa_2x_attachments_supported_);
      const int32_t push[]={128,468,640,180,int32_t(host_probe.host_ids[0]),int32_t(host_probe.host_ids[1])};
      command_processor_.SubmitBarriers(true);command_processor_.BindExternalComputePipeline(host_probe.pipeline);
      auto& commands=command_processor_.deferred_command_buffer();const VkDescriptorSet sets[]={rt->GetDescriptorSetTransferSource(),dest};
      commands.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,host_probe.layout,0,2,sets,0,nullptr);
      commands.CmdVkPushConstants(host_probe.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,24,push);commands.CmdVkDispatch(1,1,1);
      command_processor_.PushBufferMemoryBarrier(host_probe.buffer,0,64,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
      host_probe.frame=command_processor_.GetCurrentFrame();++host_probe.count;break;
    }
  }

'''
 s=s.replace(anchor,block+anchor);rtc.write_text(s)
cp=sdk/'src/graphics/vulkan/command_processor.cpp';s=cp.read_text()
if 'external_host_color' not in s:
 anchor='    auto& edram_probe=me_edram_probe;'
 assert s.count(anchor)==1
 block='''    auto& host_probe=me_host_probe;
    if(host_probe.buffer!=VK_NULL_HANDLE){
      const auto* vd=GetVulkanDevice();const auto& df=vd->functions();const auto dev=vd->device();
      if(!AwaitAllQueueOperationsCompletion())return false;
      void* mapped=nullptr;
      if(df.vkMapMemory(dev,host_probe.memory,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS){
        if(!(vd->memory_types().host_coherent&(uint32_t(1)<<host_probe.memory_type))){VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=host_probe.memory;range.size=VK_WHOLE_SIZE;df.vkInvalidateMappedMemoryRanges(dev,1,&range);}
        float values[16];std::memcpy(values,mapped,64);
        for(unsigned i=0;i<4;++i)REXGPU_INFO("external_host_color sample={} frame={} point={} lane={} host_id={} rgba={},{},{},{}",host_probe.count,host_probe.frame,i/2,i%2,host_probe.host_ids[i%2],values[i*4],values[i*4+1],values[i*4+2],values[i*4+3]);
        df.vkUnmapMemory(dev,host_probe.memory);
      }
      df.vkDestroyBuffer(dev,host_probe.buffer,nullptr);df.vkFreeMemory(dev,host_probe.memory,nullptr);host_probe.buffer=VK_NULL_HANDLE;host_probe.memory=VK_NULL_HANDLE;
    }
'''
 s=s.replace(anchor,block+anchor);cp.write_text(s)
scope={};compiler=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(compiler[:compiler.index('def build')],scope)
for source,name in [(rtc,'vulkan_render_target_cache.cpp.o'),(cp,'vulkan_command_processor.cpp.o')]:
 q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(source),'-o',str(root/'work/gpu-port/objects'/name)],capture_output=True)
 if q.returncode:print(q.stderr.decode(errors='replace')[-4000:]);raise SystemExit(q.returncode)
