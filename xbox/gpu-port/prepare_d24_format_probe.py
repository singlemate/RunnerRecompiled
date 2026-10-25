"""Opt-in A/B: ME_DIAGNOSTIC_D32_FOR_D24=1 stores guest D24S8 render targets as
VK_FORMAT_D32_SFLOAT_S8_UINT instead of VK_FORMAT_D24_UNORM_S8_UINT (diagnostic only)."""
from pathlib import Path
import os, subprocess, sys
root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
os.chdir(root)
path = root / 'work/references/rexglue-sdk/src/graphics/vulkan/render_target_cache.cpp'
text = path.read_text(encoding='utf-8')
if 'ME_DIAGNOSTIC_D32_FOR_D24' not in text:
    anchor = '''  depth_unorm24_vulkan_format_supported_ = (depth_unorm24_properties.optimalTilingFeatures &
                                            kUsedDepthFormatFeatures) == kUsedDepthFormatFeatures;
'''
    assert text.count(anchor) == 1
    text = text.replace(anchor, anchor + '''  if (std::getenv("ME_DIAGNOSTIC_D32_FOR_D24")) {
    depth_unorm24_vulkan_format_supported_ = false;
    REXGPU_WARN("me_diagnostic_d32_for_d24 not_a_fix=true");
  }
''')
    path.write_text(text, encoding='utf-8')
scope = {}
compiler = (root / 'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text()
exec(compiler[:compiler.index('def build')], scope)
result = subprocess.run([str(scope['cc']), *scope['common'], '-c', str(path), '-o',
                         str(root / 'work/gpu-port/objects/vulkan_render_target_cache.cpp.o')],
                        capture_output=True)
if result.returncode:
    print(result.stderr.decode(errors='replace')[-4000:])
raise SystemExit(result.returncode)
