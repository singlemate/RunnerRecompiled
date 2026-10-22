from pathlib import Path
import subprocess,struct
root=Path.cwd();sdk=root/'work/references/rexglue-sdk';out=root/'work/gpu-port/lightmap_image_probe.spv'
subprocess.run([str(root/'work/toolchains/android-ndk-r30/shader-tools/windows-x86_64/glslc.exe'),'--target-env=vulkan1.0','-fshader-stage=compute',str(root/'outputs/mirrors-edge-native/xbox/gpu-port/lightmap_image_probe.comp'),'-o',str(out)],check=True)
words=struct.unpack('<'+'I'*(out.stat().st_size//4),out.read_bytes());(sdk/'include/rex/graphics/vulkan/diagnostic_lightmap_image.h').write_text('#pragma once\n#include <cstdint>\nstatic const uint32_t me_lightmap_image_spv[]={'+','.join(hex(x) for x in words)+'};\n')
p=sdk/'src/graphics/vulkan/command_processor.cpp';s=p.read_text()
if 'ME_DIAGNOSTIC_LIGHTMAP_IMAGE' not in s:
 s='#include <rex/graphics/vulkan/diagnostic_lightmap_image.h>\n'+s
 anchor='  // Invalidate textures in memexported memory and watch for changes.'
 block=r'''  if(draw_ps_hash==0x89670FB6FAB9E7C6ull && std::getenv("ME_DIAGNOSTIC_LIGHTMAP_IMAGE")){
    static bool image_probe_done=false;
    if(!image_probe_done){
      image_probe_done=true;
      const auto* vd=GetVulkanDevice();const auto& df=vd->functions();const auto dev=vd->device();
      VkDescriptorSetLayout layout=VK_NULL_HANDLE;VkPipelineLayout pipeline_layout=VK_NULL_HANDLE;VkDescriptorPool pool=VK_NULL_HANDLE;VkPipeline pipeline=VK_NULL_HANDLE;
      VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory allocation=VK_NULL_HANDLE;uint32_t memory_type=UINT32_MAX;
      auto probe=[&](){
        VkDescriptorSetLayoutBinding bindings[2]{};bindings[0]={0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};bindings[1]={1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;li.bindingCount=2;li.pBindings=bindings;if(df.vkCreateDescriptorSetLayout(dev,&li,nullptr,&layout)!=VK_SUCCESS)return;
        VkPipelineLayoutCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pi.setLayoutCount=1;pi.pSetLayouts=&layout;if(df.vkCreatePipelineLayout(dev,&pi,nullptr,&pipeline_layout)!=VK_SUCCESS)return;
        pipeline=ui::vulkan::util::CreateComputePipeline(vd,pipeline_layout,me_lightmap_image_spv,sizeof(me_lightmap_image_spv));if(pipeline==VK_NULL_HANDLE)return;
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
        VkDescriptorPoolCreateInfo dpi{};dpi.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dpi.maxSets=1;dpi.poolSizeCount=2;dpi.pPoolSizes=sizes;if(df.vkCreateDescriptorPool(dev,&dpi,nullptr,&pool)!=VK_SUCCESS)return;
        VkDescriptorSetAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&layout;VkDescriptorSet set;if(df.vkAllocateDescriptorSets(dev,&ai,&set)!=VK_SUCCESS)return;
        if(!ui::vulkan::util::CreateDedicatedAllocationBuffer(vd,1024,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,ui::vulkan::util::MemoryPurpose::kReadback,buffer,allocation,&memory_type))return;
        for(unsigned slot=0;slot<3;++slot){
          const auto view=texture_cache_->GetActiveBindingOrNullImageView(slot,xenos::FetchOpDimension::k2D,false);
          VkDescriptorImageInfo image{};image.imageView=view;image.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;VkDescriptorBufferInfo bi{buffer,0,1024};
          VkWriteDescriptorSet writes[2]{};for(auto& w:writes){w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=set;w.descriptorCount=1;}
          writes[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;writes[0].pImageInfo=&image;writes[1].dstBinding=1;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[1].pBufferInfo=&bi;df.vkUpdateDescriptorSets(dev,2,writes,0,nullptr);
          SubmitBarriers(true);BindExternalComputePipeline(pipeline);deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);deferred_command_buffer_.CmdVkDispatch(1,1,1);
          PushBufferMemoryBarrier(buffer,0,1024,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
          if(!AwaitAllQueueOperationsCompletion())return;
          void* mapped=nullptr;if(df.vkMapMemory(dev,allocation,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS){
            if(!(vd->memory_types().host_coherent&(uint32_t(1)<<memory_type))){VkMappedMemoryRange r{};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=allocation;r.size=VK_WHOLE_SIZE;df.vkInvalidateMappedMemoryRanges(dev,1,&r);}
            const auto* v=static_cast<const float*>(mapped);for(unsigned i=0;i<64;++i)REXGPU_INFO("external_lightmap_image slot={} frame={} index={} view_null={} rgba={},{},{},{}",slot,frame_current_,i,view==VK_NULL_HANDLE,v[i*4],v[i*4+1],v[i*4+2],v[i*4+3]);df.vkUnmapMemory(dev,allocation);
          }
        }
      };probe();
      if(buffer)df.vkDestroyBuffer(dev,buffer,nullptr);if(allocation)df.vkFreeMemory(dev,allocation,nullptr);if(pipeline)df.vkDestroyPipeline(dev,pipeline,nullptr);if(pool)df.vkDestroyDescriptorPool(dev,pool,nullptr);if(pipeline_layout)df.vkDestroyPipelineLayout(dev,pipeline_layout,nullptr);if(layout)df.vkDestroyDescriptorSetLayout(dev,layout,nullptr);
    }
  }
'''
 assert s.count(anchor)==1;s=s.replace(anchor,block+anchor);p.write_text(s)
s=p.read_text()
if 'lightmap_image_v2' not in s:
 a=s.index('  if(draw_ps_hash==0x89670FB6FAB9E7C6ull && std::getenv("ME_DIAGNOSTIC_LIGHTMAP_IMAGE"))');b=s.index('  // Invalidate textures in memexported memory',a);block=s[a:b]
 block=block.replace('CreateDedicatedAllocationBuffer(vd,1024,','CreateDedicatedAllocationBuffer(vd,1040,').replace('bi{buffer,0,1024}','bi{buffer,0,1040}').replace('buffer,0,1024,','buffer,0,1040,')
 block=block.replace('        for(unsigned slot=0;slot<3;++slot){','        // lightmap_image_v2: ensure bindings after diagnostic submission fences.\n        texture_cache_->RequestTextures(7);\n        VkImageView probe_views[3];for(unsigned slot=0;slot<3;++slot)probe_views[slot]=texture_cache_->GetActiveBindingOrNullImageView(slot,xenos::FetchOpDimension::k2D,false);\n        for(unsigned slot=0;slot<3;++slot){').replace('const auto view=texture_cache_->GetActiveBindingOrNullImageView(slot,xenos::FetchOpDimension::k2D,false);','const auto view=probe_views[slot];')
 block=block.replace('const auto* v=static_cast<const float*>(mapped);','const auto* v=static_cast<const float*>(mapped);REXGPU_INFO("external_lightmap_image_size slot={} width={} height={} layers={}",slot,v[256],v[257],v[258]);')
 block=block.replace('\\n','\n')
 s=s[:a]+block+s[b:];p.write_text(s)
s=p.read_text()
if 'lightmap_submission_v3' not in s:
 a=s.index('  if(draw_ps_hash==0x89670FB6FAB9E7C6ull && std::getenv("ME_DIAGNOSTIC_LIGHTMAP_IMAGE"))');b=s.index('  // Invalidate textures in memexported memory',a);block=s[a:b]
 old='''        texture_cache_->RequestTextures(7);
        VkImageView probe_views[3];for(unsigned slot=0;slot<3;++slot)probe_views[slot]=texture_cache_->GetActiveBindingOrNullImageView(slot,xenos::FetchOpDimension::k2D,false);
        for(unsigned slot=0;slot<3;++slot){
          const auto view=probe_views[slot];'''
 new='''        for(unsigned slot=0;slot<3;++slot){
          // lightmap_submission_v3
          if(!BeginSubmission(true))return;
          texture_cache_->RequestTextures(7);
          const auto view=texture_cache_->GetActiveBindingOrNullImageView(slot,xenos::FetchOpDimension::k2D,false);'''
 assert old in block;block=block.replace(old,new);s=s[:a]+block+s[b:];p.write_text(s)
scope={};s=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(s[:s.index('def build')],scope)
q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(p),'-o',str(root/'work/gpu-port/objects/vulkan_command_processor.cpp.o')],capture_output=True)
if q.returncode:print('\n'.join(x for x in q.stderr.decode(errors='replace').splitlines() if 'error:' in x))
raise SystemExit(q.returncode)
