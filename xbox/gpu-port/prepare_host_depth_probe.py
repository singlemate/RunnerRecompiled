"""Read native MSAA depth using the bounded after-draw probe, opt-in."""
from pathlib import Path
import subprocess
root=Path.cwd();p=root/'work/references/rexglue-sdk/src/graphics/vulkan/render_target_cache.cpp';s=p.read_text()
a=s.index('void VulkanRenderTargetCache::DiagnosticHostAfterDraw');b=s.index('bool VulkanRenderTargetCache::DumpRenderTargets',a);block=s[a:b]
if 'ME_DIAGNOSTIC_HOST_DEPTH' not in block:
 block=block.replace('  auto* rt=static_cast<VulkanRenderTarget*>(diagnostic_last_update_used_render_targets()[1]);if(!rt)return;','  const bool probe_depth=std::getenv("ME_DIAGNOSTIC_HOST_DEPTH")!=nullptr;\n  auto* color_rt=static_cast<VulkanRenderTarget*>(diagnostic_last_update_used_render_targets()[1]);\n  if(!color_rt || color_rt->key().base_tiles!=1024 || color_rt->key().resource_format!=3)return;\n  auto* rt=probe_depth?static_cast<VulkanRenderTarget*>(diagnostic_last_update_used_render_targets()[0]):color_rt;if(!rt)return;')
 block=block.replace('if(key.is_depth || key.base_tiles!=1024 || key.GetPitchTiles()!=16 || key.resource_format!=3 || key.msaa_samples!=xenos::MsaaSamples::k2X)return;','if(key.GetPitchTiles()!=16 || key.msaa_samples!=xenos::MsaaSamples::k2X)return;\n  if(probe_depth && !key.is_depth)return;')
 block=block.replace('InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT)','InitializeSubresourceRange(probe_depth?(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT):VK_IMAGE_ASPECT_COLOR_BIT)')
 block=block.replace('VkDescriptorSetLayout layouts[]={descriptor_set_layout_sampled_image_,','VkDescriptorSetLayout layouts[]={probe_depth?descriptor_set_layout_sampled_image_x2_:descriptor_set_layout_sampled_image_,')
 block=block.replace('host_probe.after_draw=true;','if(probe_depth && host_probe.count<4)REXGPU_INFO("external_host_depth_source key={:08X} frame={} vs={:016X} ps={:016X}",key.key,command_processor_.GetCurrentFrame(),vs,ps);\n  host_probe.after_draw=true;')
 s=s[:a]+block+s[b:];p.write_text(s)
scope={};s=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text();exec(s[:s.index('def build')],scope)
q=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(p),'-o',str(root/'work/gpu-port/objects/vulkan_render_target_cache.cpp.o')],capture_output=True)
if q.returncode:print(q.stderr.decode(errors='replace')[-4000:])
raise SystemExit(q.returncode)
