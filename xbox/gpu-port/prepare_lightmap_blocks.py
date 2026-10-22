"""Read bounded BC1 source blocks from GPU shared memory, original rendering."""
from pathlib import Path
import subprocess
root=Path.cwd();p=root/'work/references/rexglue-sdk/src/graphics/vulkan/command_processor.cpp';s=p.read_text()
if 'ME_DIAGNOSTIC_LIGHTMAP_BLOCKS' not in s:
 anchor='  // Invalidate textures in memexported memory and watch for changes.'
 block=r'''  if(draw_ps_hash==0x89670FB6FAB9E7C6ull && std::getenv("ME_DIAGNOSTIC_LIGHTMAP_BLOCKS")){
    static unsigned block_snapshots=0;
    if(block_snapshots<1){
      ++block_snapshots;
      for(unsigned slot=0;slot<3;++slot){
        const auto fetch=regs.GetTextureFetch(slot);if(uint32_t(fetch.format)!=18)continue;
        const uint32_t w=fetch.size_2d.width+1,h=fetch.size_2d.height+1,pitch=fetch.pitch<<5;
        const uint32_t xy[4][2]={{0,0},{w/4,h/4},{w/2,h/2},{w*3/4,h*3/4}};
        VkBufferCopy regions[4]{};bool valid=true;
        for(unsigned i=0;i<4;++i){
          const int64_t offset=fetch.tiled?texture_util::GetTiledOffset2D(xy[i][0]/4,xy[i][1]/4,pitch/4,3):(int64_t(xy[i][1]/4)*(pitch/4)+xy[i][0]/4)*8;
          const int64_t address=(int64_t(fetch.base_address)<<12)+offset;
          if(address<0 || address>0x1ffffff8 || !shared_memory_->RequestRange(uint32_t(address),8)){valid=false;break;}
          regions[i]={VkDeviceSize(address),i*8,8};
        }
        const auto* vd=GetVulkanDevice();const auto& df=vd->functions();const auto dev=vd->device();
        VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory allocation=VK_NULL_HANDLE;uint32_t memory_type=UINT32_MAX;
        if(valid && ui::vulkan::util::CreateDedicatedAllocationBuffer(vd,32,VK_BUFFER_USAGE_TRANSFER_DST_BIT,ui::vulkan::util::MemoryPurpose::kReadback,buffer,allocation,&memory_type)){
          shared_memory_->Use(VulkanSharedMemory::Usage::kRead);SubmitBarriers(true);
          deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),buffer,4,regions);
          PushBufferMemoryBarrier(buffer,0,32,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
          if(!AwaitAllQueueOperationsCompletion())return false;
          void* mapped=nullptr;
          if(df.vkMapMemory(dev,allocation,0,VK_WHOLE_SIZE,0,&mapped)==VK_SUCCESS){
            if(!(vd->memory_types().host_coherent&(uint32_t(1)<<memory_type))){VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=allocation;range.size=VK_WHOLE_SIZE;df.vkInvalidateMappedMemoryRanges(dev,1,&range);}
            const auto* bytes=static_cast<const uint8_t*>(mapped);
            for(unsigned i=0;i<4;++i){const auto* b=bytes+i*8;REXGPU_INFO("external_lightmap_bc1 slot={} frame={} width={} height={} pitch={} tiled={} endian={} x={} y={} address={:08X} bytes={:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",slot,frame_current_,w,h,pitch,uint32_t(fetch.tiled),uint32_t(fetch.endianness),xy[i][0],xy[i][1],uint32_t(regions[i].srcOffset),b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7]);}
            df.vkUnmapMemory(dev,allocation);
          }
          df.vkDestroyBuffer(dev,buffer,nullptr);df.vkFreeMemory(dev,allocation,nullptr);
        }
      }
    }
  }
'''
 assert s.count(anchor)==1;s=s.replace(anchor,block+anchor);p.write_text(s)
s=p.read_text()
if 'lightmap_grid_v2' not in s:
 a=s.index('  if(draw_ps_hash==0x89670FB6FAB9E7C6ull && std::getenv("ME_DIAGNOSTIC_LIGHTMAP_BLOCKS"))');b=s.find('  if(draw_ps_hash==0x89670FB6FAB9E7C6ull && std::getenv("ME_DIAGNOSTIC_LIGHTMAP_IMAGE"))',a);b=s.index('  // Invalidate textures in memexported memory',a) if b<0 else b;block=s[a:b]
 block=block.replace('const uint32_t xy[4][2]={{0,0},{w/4,h/4},{w/2,h/2},{w*3/4,h*3/4}};','// lightmap_grid_v2: sample centers across an8x8 grid.\n        uint32_t xy[64][2]{};for(unsigned i=0;i<64;++i){xy[i][0]=((2*(i%8)+1)*w/16)&~3u;xy[i][1]=((2*(i/8)+1)*h/16)&~3u;}')
 block=block.replace('regions[4]','regions[64]').replace('i<4','i<64').replace('CreateDedicatedAllocationBuffer(vd,32,','CreateDedicatedAllocationBuffer(vd,512,').replace('buffer,4,regions','buffer,64,regions').replace('buffer,0,32,','buffer,0,512,')
 s=s[:a]+block+s[b:];p.write_text(s)
s=p.read_text()
if 'lightmap_source_submission_v3' not in s:
 a=s.index('  if(draw_ps_hash==0x89670FB6FAB9E7C6ull && std::getenv("ME_DIAGNOSTIC_LIGHTMAP_BLOCKS"))');b=s.find('  if(draw_ps_hash==0x89670FB6FAB9E7C6ull && std::getenv("ME_DIAGNOSTIC_LIGHTMAP_IMAGE"))',a);b=s.index('  // Invalidate textures in memexported memory',a) if b<0 else b;block=s[a:b]
 block=block.replace('for(unsigned slot=0;slot<3;++slot){','for(unsigned slot=0;slot<3;++slot){\n        // lightmap_source_submission_v3\n        if(!BeginSubmission(true))return false;')
 s=s[:a]+block+s[b:];p.write_text(s)
scope={};s=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(s[:s.index('def build')],scope)
q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(p),'-o',str(root/'work/gpu-port/objects/vulkan_command_processor.cpp.o')],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace')[-4000:])
raise SystemExit(q.returncode)
