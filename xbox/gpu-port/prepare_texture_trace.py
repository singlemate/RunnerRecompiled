"""Bounded texture upload diagnostics; no texture data or rendering changes."""
from pathlib import Path
import os,subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd()
os.chdir(root)
source=root/'work/references/rexglue-sdk/src/graphics/vulkan/texture_cache.cpp'
text=source.read_text()
anchor='  LoadShaderIndex load_shader = host_format.load_shader;'
if 'external_texture_upload' not in text:
    assert text.count(anchor)==1
    trace='''
  static uint64_t external_texture_uploads = 0;
  const auto upload = ++external_texture_uploads;
  if (upload <= 256) {
    REXGPU_INFO("external_texture_upload call={} guest_format={} host_format={} signed={} base={:08X} mips={:08X} width={} height={} base_load={} mip_load={} mip_max={} tiled={}",
        upload, uint32_t(texture_key.format), uint32_t(host_format.format), host_format_is_signed,
        texture_key.base_page << 12, texture_key.mip_page << 12, texture_key.GetWidth(), texture_key.GetHeight(),
        load_base, load_mips, uint32_t(texture_key.mip_max_level), bool(texture_key.tiled));
  }
'''
    text=text.replace(anchor,anchor+trace)
    old='''  if (load_shader == kLoadShaderIndexUnknown) {
    return false;
  }'''
    assert text.count(old)==1
    text=text.replace(old,old.replace('    return false;','    REXGPU_ERROR("external_texture_load_failed unknown shader format={}", uint32_t(texture_key.format));\n    return false;'))
if 'external_texture_keys' not in text:
    text=text.replace('#include <array>','#include <array>\n#include <set>',1)
    old='  if (upload <= 256) {'
    assert text.count(old)==1
    text=text.replace(old,'''  static std::set<std::array<uint32_t, 8>> external_texture_keys;
  const std::array<uint32_t, 8> external_texture_key = {
      uint32_t(texture_key.format), uint32_t(host_format.format), texture_key.base_page,
      texture_key.mip_page, texture_key.GetWidth(), texture_key.GetHeight(),
      uint32_t(texture_key.mip_max_level), uint32_t(texture_key.tiled)};
  if (external_texture_keys.size() < 256 && external_texture_keys.insert(external_texture_key).second) {''')
if 'ME_TEXTURE_RGBA_DIAGNOSTIC' not in text:
    text=text.replace('#include <cstring>','#include <cstring>\n#include <cstdlib>',1)
    start=text.index('  // S3TC.')
    end=text.index('  // Keep signed format support aligned',start)
    section=text[start:end]
    condition='if ((format_properties.optimalTilingFeatures & kLinearFilterFeatures) != kLinearFilterFeatures) {'
    assert section.count(condition)==5
    section=section.replace(condition,'if (diagnostic_bc_decode || (format_properties.optimalTilingFeatures & kLinearFilterFeatures) != kLinearFilterFeatures) {')
    section='''  const char* bc_decode_environment = std::getenv("ME_TEXTURE_RGBA_DIAGNOSTIC");
  const bool diagnostic_bc_decode = bc_decode_environment && !std::strcmp(bc_decode_environment, "1");
  REXGPU_INFO("external_texture_bc_decode_diagnostic enabled={}", diagnostic_bc_decode);
'''+section
    text=text[:start]+section+text[end:]
source.write_text(text)
scope={};compiler=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text()
exec(compiler[:compiler.index('def build')],scope)
result=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(source),'-o',str(root/'work/gpu-port/objects/vulkan_texture_cache.cpp.o')],capture_output=True)
if result.returncode:print(result.stderr.decode(errors='replace')[-4000:])
raise SystemExit(result.returncode)
