"""Observe host MSAA colors after individual draws, opt-in bounded48 snapshots."""
from pathlib import Path
import subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd();sdk=root/'work/references/rexglue-sdk'
(sdk/'include/rex/graphics/vulkan/diagnostic_edram_probe.h').write_text((root/'outputs/mirrors-edge-native/xbox/gpu-port/diagnostic_edram_probe.h').read_text())
base=sdk/'include/rex/graphics/pipeline/render_target/cache.h';b=base.read_text()
if 'diagnostic_last_update_used_render_targets()' not in b:
 anchor='  RenderTarget* const* last_update_accumulated_render_targets() const {'
 assert b.count(anchor)==1
 b=b.replace(anchor,'  RenderTarget* const* diagnostic_last_update_used_render_targets() const {\n    return last_update_used_render_targets_;\n  }\n'+anchor);base.write_text(b)
h=sdk/'include/rex/graphics/vulkan/render_target_cache.h';s=h.read_text()
if 'DiagnosticHostAfterDraw' not in s:
 anchor='  bool Update(bool is_rasterization_done,'
 assert s.count(anchor)==1;s=s.replace(anchor,'  void DiagnosticHostAfterDraw(uint64_t vs, uint64_t ps);\n'+anchor);h.write_text(s)
rtc=sdk/'src/graphics/vulkan/render_target_cache.cpp';s=rtc.read_text()
if 'void VulkanRenderTargetCache::DiagnosticHostAfterDraw' not in s:
 start=s.index('      const auto* vd=command_processor_.GetVulkanDevice();',s.index('ME_DIAGNOSTIC_HOST_COLOR'))
 end=s.index('host_probe.frame=command_processor_.GetCurrentFrame();++host_probe.count;break;',start)+len('host_probe.frame=command_processor_.GetCurrentFrame();++host_probe.count;break;')
 body=s[start:end].replace('break;','return;')
 pre='''void VulkanRenderTargetCache::DiagnosticHostAfterDraw(uint64_t vs,uint64_t ps){
  auto& host_probe=me_host_probe;
  if(!std::getenv("ME_DIAGNOSTIC_HOST_AFTER_DRAW") || GetPath()!=Path::kHostRenderTargets || IsDrawResolutionScaled() || host_probe.count>=48 || host_probe.buffer!=VK_NULL_HANDLE)return;
  auto* rt=static_cast<VulkanRenderTarget*>(diagnostic_last_update_used_render_targets()[1]);if(!rt)return;
  const auto key=rt->key();
  if(key.is_depth || key.base_tiles!=1024 || key.GetPitchTiles()!=16 || key.resource_format!=3 || key.msaa_samples!=xenos::MsaaSamples::k2X)return;
  command_processor_.PushImageMemoryBarrier(rt->image(),ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT),rt->current_stage_mask(),VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,rt->current_access_mask(),VK_ACCESS_SHADER_READ_BIT,rt->current_layout(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  rt->SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  host_probe.after_draw=true;host_probe.vs=vs;host_probe.ps=ps;host_probe.depth=register_file().Get<reg::RB_DEPTHCONTROL>().value;
'''
 method=pre+body+'\n}\n\n'
 anchor='bool VulkanRenderTargetCache::DumpRenderTargets(uint32_t dump_base,'
 assert s.count(anchor)==1;s=s.replace(anchor,method+anchor);rtc.write_text(s)
cp=sdk/'src/graphics/vulkan/command_processor.cpp';s=cp.read_text()
if 'DiagnosticHostAfterDraw(draw_vs_hash' not in s:
 start=s.index('    auto& host_probe=me_host_probe;')
 end=s.index('    auto& edram_probe=me_edram_probe;',start)
 finish=s[start:end]
 needle='        float values[16];std::memcpy(values,mapped,64);'
 finish=finish.replace(needle,needle+'''
        REXGPU_INFO("external_host_draw_context sample={} frame={} vs={:016X} ps={:016X} depth={:08X}",host_probe.count,host_probe.frame,host_probe.vs,host_probe.ps,host_probe.depth);''')
 anchor='  // Invalidate textures in memexported memory and watch for changes.'
 assert s.count(anchor)==1
 s=s.replace(anchor,'  if(std::getenv("ME_DIAGNOSTIC_HOST_AFTER_DRAW")){\n    render_target_cache_->DiagnosticHostAfterDraw(draw_vs_hash,draw_ps_hash);\n'+finish+'  }\n'+anchor);cp.write_text(s)
scope={};compiler=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(compiler[:compiler.index('def build')],scope)
for source,name in [(rtc,'vulkan_render_target_cache.cpp.o'),(cp,'vulkan_command_processor.cpp.o')]:
 q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(source),'-o',str(root/'work/gpu-port/objects'/name)],capture_output=True)
 if q.returncode:print(q.stderr.decode(errors='replace')[-4000:]);raise SystemExit(q.returncode)
