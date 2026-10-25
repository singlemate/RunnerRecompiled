"""Opt-in readback of both 2X depth samples of the prepass render target around the
4X EDRAM clear that moves its ownership away and back (see PREDICATED-TILING-DEPTH.md).

ME_DIAGNOSTIC_DEPTH_ROUNDTRIP=<frame>, in the first frame >= <frame> that has the
sequence, reads render target 00688000 (2X D24FS8, base 0) at two silhouette points:
  A - at the start of the 4X clear draw, before its render target update (transfers);
  B - after the render target update of the top tile sky draw, before it draws;
  C - after the first top-tile city draw (PS 89670FB6FAB9E7C6).
Observation only: the render target is returned to its previous usage after each read.
"""
from pathlib import Path
import os, subprocess, sys
root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
os.chdir(root)
sdk = root / 'work/references/rexglue-sdk'

def patch(path, marker, edits):
    text = path.read_text(encoding='utf-8')
    if marker in text:
        return
    for anchor, new, where in edits:
        assert text.count(anchor) == 1, (path.name, anchor[:60])
        text = text.replace(anchor, anchor + new if where == 'after' else new + anchor)
    path.write_text(text, encoding='utf-8')

patch(sdk / 'include/rex/graphics/pipeline/render_target/cache.h', 'DiagnosticFindRenderTarget', [(
    '''  RenderTarget* const* diagnostic_last_update_used_render_targets() const {
    return last_update_used_render_targets_;
  }
''', '''  RenderTarget* DiagnosticFindRenderTarget(uint32_t key_value) const {
    RenderTargetKey key;
    key.key = key_value;
    auto it = render_targets_.find(key);
    return it != render_targets_.end() ? it->second : nullptr;
  }
''', 'after')])

patch(sdk / 'include/rex/graphics/vulkan/diagnostic_edram_probe.h', 'me_roundtrip_probe', [(
    'inline MeHostProbeState me_host_depth_probe;\n',
    'inline MeHostProbeState me_roundtrip_probe[3];\n', 'after')])

patch(sdk / 'include/rex/graphics/vulkan/render_target_cache.h', 'DiagnosticDepthRoundtripProbe', [(
    '  void DiagnosticHostAfterDraw(uint64_t vs, uint64_t ps, bool depth_override=false);\n',
    '  bool DiagnosticDepthRoundtripProbe(uint32_t key_value, unsigned slot, const int32_t* points);\n',
    'after')])

patch(sdk / 'src/graphics/vulkan/render_target_cache.cpp', 'DiagnosticDepthRoundtripProbe', [(
    'bool VulkanRenderTargetCache::DumpRenderTargets(uint32_t dump_base, uint32_t dump_row_length_used,',
    '''bool VulkanRenderTargetCache::DiagnosticDepthRoundtripProbe(uint32_t key_value, unsigned slot,
                                                            const int32_t* points) {
  auto* rt = static_cast<VulkanRenderTarget*>(DiagnosticFindRenderTarget(key_value));
  if (!rt || slot >= 3 || GetPath() != Path::kHostRenderTargets || IsDrawResolutionScaled()) {
    return false;
  }
  auto& probe = me_roundtrip_probe[slot];
  const auto key = rt->key();
  if (probe.buffer != VK_NULL_HANDLE || !key.is_depth ||
      key.msaa_samples != xenos::MsaaSamples::k2X) {
    return false;
  }
  const auto* vd = command_processor_.GetVulkanDevice();
  const auto& df = vd->functions();
  const auto dev = vd->device();
  static VkPipelineLayout layout = VK_NULL_HANDLE;
  static VkPipeline pipeline = VK_NULL_HANDLE;
  if (layout == VK_NULL_HANDLE) {
    VkDescriptorSetLayout layouts[] = {
        descriptor_set_layout_sampled_image_x2_,
        command_processor_.GetSingleTransientDescriptorLayout(
            VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute)};
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    VkPipelineLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 2;
    info.pSetLayouts = layouts;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range;
    if (df.vkCreatePipelineLayout(dev, &info, nullptr, &layout) != VK_SUCCESS) return false;
    pipeline = ui::vulkan::util::CreateComputePipeline(vd, layout, me_host_probe_spv,
                                                       sizeof(me_host_probe_spv));
  }
  if (pipeline == VK_NULL_HANDLE) return false;
  const auto dest = command_processor_.AllocateSingleTransientDescriptor(
      VulkanCommandProcessor::SingleTransientDescriptorLayout::kStorageBufferCompute);
  if (dest == VK_NULL_HANDLE) return false;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vd, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, ui::vulkan::util::MemoryPurpose::kReadback,
          probe.buffer, probe.memory, &probe.memory_type)) {
    return false;
  }
  VkDescriptorBufferInfo binfo{probe.buffer, 0, 64};
  VkWriteDescriptorSet write{};
  write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write.dstSet = dest;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &binfo;
  df.vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
  const VkPipelineStageFlags old_stage = rt->current_stage_mask();
  const VkAccessFlags old_access = rt->current_access_mask();
  const VkImageLayout old_layout = rt->current_layout();
  TransitionRenderTarget(*rt, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  probe.host_ids[0] = draw_util::GetD3D10SampleIndexForGuest2xMSAA(0, msaa_2x_attachments_supported_);
  probe.host_ids[1] = draw_util::GetD3D10SampleIndexForGuest2xMSAA(1, msaa_2x_attachments_supported_);
  const int32_t push[] = {points[0], points[1], points[2], points[3], int32_t(probe.host_ids[0]),
                          int32_t(probe.host_ids[1])};
  command_processor_.SubmitBarriers(true);
  command_processor_.BindExternalComputePipeline(pipeline);
  auto& commands = command_processor_.deferred_command_buffer();
  const VkDescriptorSet sets[] = {rt->GetDescriptorSetTransferSource(), dest};
  commands.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, sets, 0, nullptr);
  commands.CmdVkPushConstants(layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 24, push);
  commands.CmdVkDispatch(1, 1, 1);
  command_processor_.PushBufferMemoryBarrier(probe.buffer, 0, 64, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                             VK_ACCESS_HOST_READ_BIT);
  if (old_layout != VK_IMAGE_LAYOUT_UNDEFINED) {
    TransitionRenderTarget(*rt, old_stage, old_access, old_layout);
  }
  probe.frame = command_processor_.GetCurrentFrame();
  probe.depth = key.key;
  ++probe.count;
  return true;
}

''', 'before')])

cp = sdk / 'src/graphics/vulkan/command_processor.cpp'
patch(cp, 'ME_DIAGNOSTIC_DEPTH_ROUNDTRIP', [
    ('''  const RegisterFile& regs = *register_file_;
  (void)index_buffer_info;
''', '''  // ME_DIAGNOSTIC_DEPTH_ROUNDTRIP (prepare_depth_roundtrip_probe.py), opt-in.
  static const uint64_t me_roundtrip_wanted = [] {
    const char* value = std::getenv("ME_DIAGNOSTIC_DEPTH_ROUNDTRIP");
    return value ? uint64_t(std::strtoull(value, nullptr, 10)) : UINT64_MAX;
  }();
  static unsigned me_roundtrip_phase = 0;
  static uint64_t me_roundtrip_frame = 0;
  static const int32_t me_roundtrip_points[4] = {170, 450, 1070, 490};
  auto me_roundtrip_finish = [&](bool report) {
    if (!AwaitAllQueueOperationsCompletion()) return false;
    const auto* vd = GetVulkanDevice();
    const auto& df = vd->functions();
    const auto dev = vd->device();
    static const char* const kLabels[3] = {"A_before_clear", "B_after_return", "C_after_city"};
    for (unsigned slot = 0; slot < 3; ++slot) {
      auto& probe = me_roundtrip_probe[slot];
      if (probe.buffer == VK_NULL_HANDLE) continue;
      void* mapped = nullptr;
      if (report && df.vkMapMemory(dev, probe.memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        if (!(vd->memory_types().host_coherent & (uint32_t(1) << probe.memory_type))) {
          VkMappedMemoryRange range{};
          range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
          range.memory = probe.memory;
          range.size = VK_WHOLE_SIZE;
          df.vkInvalidateMappedMemoryRanges(dev, 1, &range);
        }
        float values[16];
        std::memcpy(values, mapped, 64);
        for (unsigned i = 0; i < 4; ++i) {
          uint32_t bits;
          std::memcpy(&bits, &values[i * 4], 4);
          REXGPU_INFO("me_depth_roundtrip frame={} stage={} rt={:08X} point={},{} lane={} host_id={} "
                      "depth={:.9g} bits={:08X}",
                      probe.frame, kLabels[slot], probe.depth, me_roundtrip_points[(i / 2) * 2],
                      me_roundtrip_points[(i / 2) * 2 + 1], i % 2, probe.host_ids[i % 2],
                      values[i * 4], bits);
        }
        df.vkUnmapMemory(dev, probe.memory);
      }
      df.vkDestroyBuffer(dev, probe.buffer, nullptr);
      df.vkFreeMemory(dev, probe.memory, nullptr);
      probe.buffer = VK_NULL_HANDLE;
      probe.memory = VK_NULL_HANDLE;
    }
    return true;
  };
  if (me_roundtrip_wanted != UINT64_MAX && me_roundtrip_phase < 3) {
    if (me_roundtrip_phase && frame_current_ != me_roundtrip_frame) {
      // The sequence did not complete in one frame: drop it and start over.
      if (!me_roundtrip_finish(false)) return false;
      me_roundtrip_phase = 0;
    }
    if (!me_roundtrip_phase && frame_current_ >= me_roundtrip_wanted &&
        ((regs[XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3) == 2 &&
        (regs[XE_GPU_REG_RB_COLOR_INFO] & 0xFFF) == 1024 &&
        (regs[XE_GPU_REG_RB_DEPTH_INFO] & 0xFFF) == 1024 &&
        (regs[XE_GPU_REG_RB_MODECONTROL] & 7) == 5) {
      if (!BeginSubmission(true)) return false;
      if (render_target_cache_->DiagnosticDepthRoundtripProbe(0x00688000, 0, me_roundtrip_points)) {
        me_roundtrip_phase = 1;
        me_roundtrip_frame = frame_current_;
      }
    }
  }
''', 'after'),
    ('''    return draw_fail("render_target_update");
  }
''', '''  if (me_roundtrip_phase == 1 && frame_current_ == me_roundtrip_frame && pixel_shader &&
      pixel_shader->ucode_data_hash() == 0x5EBE814012217F48ull &&
      regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET] == 0 && (regs[XE_GPU_REG_RB_COLOR_INFO] & 0xFFF) == 1024 &&
      render_target_cache_->DiagnosticDepthRoundtripProbe(0x00688000, 1, me_roundtrip_points)) {
    me_roundtrip_phase = 2;
  }
''', 'after'),
    ('''  if(std::getenv("ME_DIAGNOSTIC_HOST_AFTER_DRAW")){
    const bool paired''', '''  if (me_roundtrip_phase == 2 && frame_current_ == me_roundtrip_frame && pixel_shader &&
      pixel_shader->ucode_data_hash() == 0x89670FB6FAB9E7C6ull &&
      regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET] == 0 &&
      render_target_cache_->DiagnosticDepthRoundtripProbe(0x00688000, 2, me_roundtrip_points)) {
    me_roundtrip_phase = 3;
    if (!me_roundtrip_finish(true)) return false;
  }
''', 'before'),
])

scope = {}
compiler = (root / 'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text()
exec(compiler[:compiler.index('def build')], scope)
for path, obj in ((cp, 'vulkan_command_processor.cpp.o'),
                  (sdk / 'src/graphics/vulkan/render_target_cache.cpp', 'vulkan_render_target_cache.cpp.o')):
    result = subprocess.run([str(scope['cc']), *scope['common'], '-c', str(path), '-o',
                             str(root / 'work/gpu-port/objects' / obj)], capture_output=True)
    if result.returncode:
        print(result.stderr.decode(errors='replace')[-4000:])
        raise SystemExit(result.returncode)
