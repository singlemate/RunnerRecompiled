"""Pair encoded incoming depth and preserved depth after each selected draw."""
from pathlib import Path
import subprocess
root=Path.cwd();sdk=root/'work/references/rexglue-sdk'
h=root/'outputs/mirrors-edge-native/xbox/gpu-port/diagnostic_edram_probe.h';s=h.read_text()
if 'me_host_depth_probe' not in s:s=s.replace('inline MeHostProbeState me_host_probe;','inline MeHostProbeState me_host_probe;\ninline MeHostProbeState me_host_depth_probe;');h.write_text(s)
(sdk/'include/rex/graphics/vulkan/diagnostic_edram_probe.h').write_text(s)
h=sdk/'include/rex/graphics/vulkan/render_target_cache.h';s=h.read_text().replace('void DiagnosticHostAfterDraw(uint64_t vs, uint64_t ps);','void DiagnosticHostAfterDraw(uint64_t vs, uint64_t ps, bool depth_override=false);');h.write_text(s)
p=sdk/'src/graphics/vulkan/render_target_cache.cpp';s=p.read_text();a=s.index('void VulkanRenderTargetCache::DiagnosticHostAfterDraw');b=s.index('bool VulkanRenderTargetCache::DumpRenderTargets',a);block=s[a:b]
if 'bool depth_override' not in block:
 block=block.replace('uint64_t vs,uint64_t ps){','uint64_t vs,uint64_t ps,bool depth_override){').replace('auto& host_probe=me_host_probe;','auto& host_probe=depth_override?me_host_depth_probe:me_host_probe;').replace('const bool probe_depth=std::getenv("ME_DIAGNOSTIC_HOST_DEPTH")!=nullptr;','const bool probe_depth=depth_override || std::getenv("ME_DIAGNOSTIC_HOST_DEPTH")!=nullptr;')
 s=s[:a]+block+s[b:];p.write_text(s)
cp=sdk/'src/graphics/vulkan/command_processor.cpp';s=cp.read_text()
if 'ME_DIAGNOSTIC_PAIR_DEPTH' not in s:
 anchor='  // Draw.\n'
 block=r'''  // Clear only diagnostic color pixels; depth and other pixels are untouched.
  if(std::getenv("ME_DIAGNOSTIC_PAIR_DEPTH") && draw_ps_hash==0x89670FB6FAB9E7C6ull){
    VkClearAttachment ca{};ca.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;ca.colorAttachment=0;
    for(unsigned i=0;i<4;++i)ca.clearValue.color.float32[i]=511.0f;
    VkClearRect rects[2]{};
    rects[0].rect={{128,468},{1,1}};rects[1].rect={{640,180},{1,1}};
    rects[0].layerCount=rects[1].layerCount=1;
    deferred_command_buffer_.CmdVkClearAttachments(1,&ca,2,rects);
  }
'''
 assert s.count(anchor)==1;s=s.replace(anchor,block+anchor)
 a=s.index('  if(std::getenv("ME_DIAGNOSTIC_HOST_AFTER_DRAW")){');b=s.index('  // Invalidate textures in memexported memory',a);block=s[a:b]
 block=block.replace('  if(std::getenv("ME_DIAGNOSTIC_HOST_AFTER_DRAW")){','  if(std::getenv("ME_DIAGNOSTIC_HOST_AFTER_DRAW")){\n    const bool paired=std::getenv("ME_DIAGNOSTIC_PAIR_DEPTH")!=nullptr;\n    for(unsigned phase=0;phase<(paired?2u:1u);++phase){\n      if(paired && draw_ps_hash!=0x89670FB6FAB9E7C6ull)break;')
 block=block.replace('DiagnosticHostAfterDraw(draw_vs_hash,draw_ps_hash);','DiagnosticHostAfterDraw(draw_vs_hash,draw_ps_hash,phase==1);').replace('auto& host_probe=me_host_probe;','auto& host_probe=phase==1?me_host_depth_probe:me_host_probe;')
 needle='        float values[16];std::memcpy(values,mapped,64);'
 block=block.replace(needle,needle+r'''
        if(paired)REXGPU_INFO("external_paired_depth sample={} frame={} kind={} ib={:08X} count={} position={:08X},{:08X}",host_probe.count,frame_current_,phase==0?"incoming":"stored",regs[XE_GPU_REG_VGT_DMA_BASE],index_count,regs.GetVertexFetch(95).dword_0,regs.GetVertexFetch(95).dword_1);
''')
 block=block.rstrip()+'\n    }\n'
 s=s[:a]+block+s[b:];cp.write_text(s)
scope={};s=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(s[:s.index('def build')],scope)
for p,name in [(p,'vulkan_render_target_cache.cpp.o'),(cp,'vulkan_command_processor.cpp.o')]:
 q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(p),'-o',str(root/'work/gpu-port/objects'/name)],capture_output=True)
 if q.returncode:print(q.stderr.decode(errors='replace')[-4000:]);raise SystemExit(q.returncode)
