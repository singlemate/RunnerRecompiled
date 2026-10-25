"""Bounded per-frame trace of predicated-tiling state: every draw and resolve of one frame.

ME_DIAGNOSTIC_TILE_TRACE=<frame> logs window offset, scissors, EDRAM bases, depth state,
viewport registers and the first texture fetch of each draw, and the copy registers of
each resolve, for the first frame >= <frame>. Observation only: nothing is changed.
"""
from pathlib import Path
import os, subprocess, sys
root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
os.chdir(root)
source = root / 'work/references/rexglue-sdk/src/graphics/vulkan/command_processor.cpp'
text = source.read_text(encoding='utf-8')
if 'me_tile_trace_active' not in text:
    helper_anchor = 'bool VulkanCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,'
    assert text.count(helper_anchor) == 1
    helper = '''// ME_DIAGNOSTIC_TILE_TRACE: buffers each frame from <frame> on and prints the
// first whole frame that draws the city sky pass (PS 5EBE814012217F48).
// Bounded to 6000 records; printed once.
static uint64_t me_tile_trace_frame = UINT64_MAX;
static uint32_t me_tile_trace_records = 0;
static bool me_tile_trace_marker = false;
static bool me_tile_trace_done = false;
// Shared with render_target_cache.cpp, which records ownership transfers.
std::vector<std::string> me_tile_trace_lines;
bool me_tile_trace_open = false;
static bool me_tile_trace_active_impl(uint64_t frame);
static bool me_tile_trace_active(uint64_t frame) {
  me_tile_trace_open = me_tile_trace_active_impl(frame);
  return me_tile_trace_open;
}
static bool me_tile_trace_active_impl(uint64_t frame) {
  static const uint64_t wanted = [] {
    const char* value = std::getenv("ME_DIAGNOSTIC_TILE_TRACE");
    return value ? uint64_t(std::strtoull(value, nullptr, 10)) : UINT64_MAX;
  }();
  if (me_tile_trace_done || wanted == UINT64_MAX || frame < wanted) return false;
  if (frame != me_tile_trace_frame) {
    if (me_tile_trace_marker) {
      for (const auto& line : me_tile_trace_lines) REXGPU_INFO("{}", line);
      REXGPU_INFO("me_tile_trace_end frame={} records={}", me_tile_trace_frame,
                  me_tile_trace_lines.size());
      me_tile_trace_done = true;
      me_tile_trace_lines.clear();
      return false;
    }
    me_tile_trace_frame = frame;
    me_tile_trace_lines.clear();
    me_tile_trace_records = 0;
  }
  return me_tile_trace_records < 6000;
}
static float me_reg_float(const RegisterFile& regs, uint32_t index) {
  float value;
  std::memcpy(&value, &regs.values[index], sizeof(value));
  return value;
}

'''
    text = text.replace(helper_anchor, helper + helper_anchor)

    draw_anchor = '  texture_cache_->RequestTextures(used_texture_mask);'
    assert text.count(draw_anchor) == 1
    draw = '''
  if (me_tile_trace_active(frame_current_)) {
    const uint32_t record = me_tile_trace_records++;
    uint32_t fetch0_base = 0, fetch0_size = 0;
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (used_texture_mask & (1u << slot)) {
        const auto fetch = regs.GetTextureFetch(slot);
        fetch0_base = fetch.dword_1 & 0xFFFFF000u;
        fetch0_size = fetch.dword_2;
        break;
      }
    }
    if (pixel_shader && pixel_shader->ucode_data_hash() == 0x5EBE814012217F48ull) {
      me_tile_trace_marker = true;
    }
    me_tile_trace_lines.push_back(fmt::format(
        "me_tile_draw frame={} n={} vs={:016X} ps={:016X} prim={} indices={} win_off={:08X} "
        "win_tl={:08X} win_br={:08X} scr_tl={:08X} scr_br={:08X} mode_cntl={:08X} surface={:08X} "
        "color0={:08X} depth={:08X} depthctl={:08X} mask={:08X} modectl={:08X} vte={:08X} "
        "vport_x={},{} vport_y={},{} ps_writes_depth={} textures={:08X} fetch0={:08X}/{:08X}",
        frame_current_, record, vertex_shader->ucode_data_hash(),
        pixel_shader ? pixel_shader->ucode_data_hash() : 0, uint32_t(prim_type), index_count,
        regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET], regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
        regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR], regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL],
        regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR], regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
        regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_COLOR_INFO],
        regs[XE_GPU_REG_RB_DEPTH_INFO], regs[XE_GPU_REG_RB_DEPTHCONTROL],
        regs[XE_GPU_REG_RB_COLOR_MASK], regs[XE_GPU_REG_RB_MODECONTROL],
        regs[XE_GPU_REG_PA_CL_VTE_CNTL], me_reg_float(regs, XE_GPU_REG_PA_CL_VPORT_XSCALE),
        me_reg_float(regs, XE_GPU_REG_PA_CL_VPORT_XOFFSET),
        me_reg_float(regs, XE_GPU_REG_PA_CL_VPORT_YSCALE),
        me_reg_float(regs, XE_GPU_REG_PA_CL_VPORT_YOFFSET),
        pixel_shader && pixel_shader->writes_depth(), used_texture_mask, fetch0_base, fetch0_size));
  }
'''
    text = text.replace(draw_anchor, draw_anchor + draw)

    copy_anchor = '''bool VulkanCommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
'''
    assert text.count(copy_anchor) == 1
    copy = '''  if (me_tile_trace_active(frame_current_)) {
    const auto& regs = *register_file_;
    me_tile_trace_lines.push_back(fmt::format(
        "me_tile_copy frame={} n={} copy_control={:08X} dest_base={:08X} dest_info={:08X} "
        "dest_pitch={:08X} win_off={:08X} win_tl={:08X} win_br={:08X} scr_tl={:08X} "
        "scr_br={:08X} surface={:08X} color0={:08X} depth={:08X} modectl={:08X} "
        "depth_clear={:08X}",
        frame_current_, me_tile_trace_records++, regs[XE_GPU_REG_RB_COPY_CONTROL],
        regs[XE_GPU_REG_RB_COPY_DEST_BASE], regs[XE_GPU_REG_RB_COPY_DEST_INFO],
        regs[XE_GPU_REG_RB_COPY_DEST_PITCH], regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET],
        regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL], regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
        regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL], regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR],
        regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_COLOR_INFO],
        regs[XE_GPU_REG_RB_DEPTH_INFO], regs[XE_GPU_REG_RB_MODECONTROL],
        regs[XE_GPU_REG_RB_DEPTH_CLEAR]));
  }
'''
    text = text.replace(copy_anchor, copy_anchor + copy)
if 'ME_DIAGNOSTIC_SKIP_DEPTH_PREPASS' not in text:
    # Diagnostic only (not a fix): omit the full-screen 2X depth prepass, so the
    # top predicated tile tests against cleared depth like the bottom tile does.
    anchor = '  if (me_tile_trace_active(frame_current_)) {\n    const uint32_t record'
    assert text.count(anchor) == 1
    skip = '''  static const bool me_skip_depth_prepass = std::getenv("ME_DIAGNOSTIC_SKIP_DEPTH_PREPASS") != nullptr;
  if (me_skip_depth_prepass && !pixel_shader && regs[XE_GPU_REG_RB_COLOR_MASK] == 0 &&
      (regs[XE_GPU_REG_RB_DEPTHCONTROL] & 6) == 6 && (regs[XE_GPU_REG_RB_MODECONTROL] & 7) == 5 &&
      ((regs[XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3) == 1 && regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET] == 0 &&
      ((regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR] >> 16) & 0x7FFF) == 720) {
    static uint64_t skipped = 0;
    if (++skipped <= 4) {
      REXGPU_WARN("me_diagnostic_depth_prepass_omitted indices={} not_a_fix=true", index_count);
    }
    return true;
  }
'''
    text = text.replace(anchor, skip + anchor)
if 'ME_DIAGNOSTIC_SKIP_TILE_CLEAR' not in text:
    # Diagnostic only (not a fix): omit the 4X EDRAM clear quad that precedes the top
    # tile (color+depth base 1024), which makes the render target cache move the prepass
    # depth through a 4X D24S8 render target and back.
    anchor = '  if (me_tile_trace_active(frame_current_)) {\n    const uint32_t record'
    assert text.count(anchor) == 1
    skip = '''  static const bool me_skip_tile_clear = std::getenv("ME_DIAGNOSTIC_SKIP_TILE_CLEAR") != nullptr;
  if (me_skip_tile_clear && !pixel_shader && ((regs[XE_GPU_REG_RB_SURFACE_INFO] >> 16) & 3) == 2 &&
      (regs[XE_GPU_REG_RB_COLOR_INFO] & 0xFFF) == 1024 && (regs[XE_GPU_REG_RB_DEPTH_INFO] & 0xFFF) == 1024 &&
      (regs[XE_GPU_REG_RB_MODECONTROL] & 7) == 5) {
    static uint64_t skipped = 0;
    if (++skipped <= 4) REXGPU_WARN("me_diagnostic_tile_clear_omitted not_a_fix=true");
    return true;
  }
'''
    text = text.replace(anchor, skip + anchor)
source.write_text(text, encoding='utf-8')

rt_source = root / 'work/references/rexglue-sdk/src/graphics/vulkan/render_target_cache.cpp'
rt_text = rt_source.read_text(encoding='utf-8')
if 'me_tile_trace_lines' not in rt_text:
    anchor = '''    const Transfer::Rectangle* resolve_clear_rectangle) {
  assert_true(GetPath() == Path::kHostRenderTargets);
'''
    assert rt_text.count(anchor) == 1
    block = '''  if (me_tile_trace_open) {
    auto describe = [](const RenderTarget* rt) {
      if (!rt) return std::string("none");
      const RenderTargetKey k = rt->key();
      return fmt::format("{:08X}(base={},pitch={},msaa={},depth={},fmt={})", k.key,
                         uint32_t(k.base_tiles), uint32_t(k.pitch_tiles_at_32bpp),
                         uint32_t(k.msaa_samples), uint32_t(k.is_depth),
                         uint32_t(k.resource_format));
    };
    for (uint32_t i = 0; i < render_target_count; ++i) {
      if (!render_targets[i]) continue;
      for (const Transfer& transfer : render_target_transfers[i]) {
        me_tile_trace_lines.push_back(fmt::format(
            "me_tile_transfer dest={} tiles={}..{} source={} host_depth_source={}",
            describe(render_targets[i]), transfer.start_tiles, transfer.end_tiles,
            describe(transfer.source), describe(transfer.host_depth_source)));
      }
    }
  }
'''
    rt_text = rt_text.replace(anchor, anchor + block)
    decl_anchor = 'namespace rex::graphics::vulkan {'
    assert rt_text.count(decl_anchor) == 1
    rt_text = rt_text.replace(decl_anchor, decl_anchor + '''
// Defined in command_processor.cpp by prepare_tile_trace.py.
extern std::vector<std::string> me_tile_trace_lines;
extern bool me_tile_trace_open;
''', 1)
    rt_source.write_text(rt_text, encoding='utf-8')

scope = {}
compiler = (root / 'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text()
exec(compiler[:compiler.index('def build')], scope)
for path, obj in ((source, 'vulkan_command_processor.cpp.o'), (rt_source, 'vulkan_render_target_cache.cpp.o')):
    result = subprocess.run([str(scope['cc']), *scope['common'], '-c', str(path), '-o',
                             str(root / 'work/gpu-port/objects' / obj)], capture_output=True)
    if result.returncode:
        print(result.stderr.decode(errors='replace')[-4000:])
        raise SystemExit(result.returncode)
