"""Enable an offscreen presenter before external GPU setup (no Window/UI ABI)."""
from pathlib import Path
import os,subprocess,sys
root=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else Path.cwd()
os.chdir(root)
sdk=root/'work/references/rexglue-sdk'
header=sdk/'include/rex/graphics/graphics_system.h'
source=sdk/'src/graphics/graphics_system.cpp'
text=header.read_text()
if 'X_STATUS SetupExternalPresenter();' not in text:
    anchor='  void PumpExternalVblank() { MarkVblank(); }'
    assert text.count(anchor)==1
    header.write_text(text.replace(anchor,'  X_STATUS SetupExternalPresenter();\n'+anchor))
text=source.read_text()
if 'X_STATUS GraphicsSystem::SetupExternalPresenter()' not in text:
    anchor='X_STATUS GraphicsSystem::SetupExternalGpu('
    assert text.count(anchor)==1
    method='''X_STATUS GraphicsSystem::SetupExternalPresenter() {
  if (command_processor_ || presenter_) return X_STATUS_UNSUCCESSFUL;
  if (!provider_) CreateProvider(true);
  if (!provider_) return X_STATUS_UNSUCCESSFUL;
  provider_supports_presentation_ = true;
  presenter_ = provider_->CreatePresenter([this](bool responsible, bool) {
    OnHostGpuLossFromAnyThread(responsible);
  });
  return presenter_ ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

'''
    source.write_text(text.replace(anchor,method+anchor))
print('External offscreen presenter setup prepared')
scope={}
compiler=(root/'outputs/mirrors-edge-native/xbox/gpu-port/compile_gpu_objects.py').read_text()
exec(compiler[:compiler.index('def build')],scope)
result=subprocess.run([str(scope['cc']),*scope['common'],'-c',str(source),'-o',str(root/'work/gpu-port/objects/graphics_system.cpp.o')],capture_output=True)
if result.returncode:
    print(result.stderr.decode(errors='replace')[-4000:])
    raise SystemExit(result.returncode)
