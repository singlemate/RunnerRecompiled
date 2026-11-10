import argparse,hashlib,json,random,subprocess,zipfile
from pathlib import Path
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_LR,UC_ARM64_REG_PC
ap=argparse.ArgumentParser();ap.add_argument('--ndk',required=True);args=ap.parse_args()
root=Path(__file__).resolve().parents[1];build=root/'build'/'android';tools=Path(args.ndk)/'toolchains/llvm/prebuilt/windows-x86_64/bin';apk=root/'RunnerRecompiled.apk'
with zipfile.ZipFile(apk) as z:
 assert z.testzip() is None
 lib=z.getinfo('lib/arm64-v8a/libmeprobe.so');assert lib.compress_type==zipfile.ZIP_STORED
 assert z.read(lib)==(build/'native/lib/arm64-v8a/libmeprobe.so').read_bytes()
 assert z.getinfo('classes.dex').file_size>0
sym=subprocess.check_output([str(tools/'llvm-readelf.exe'),'-Ws',str(build/'native/lib/arm64-v8a/libmeprobe.so')],text=True)
assert 'Java_org_mirrorsedge_research_MainActivity_nativeProbe' in sym
reloc=subprocess.check_output([str(tools/'llvm-readelf.exe'),'-r',str(build/'probe.o')],text=True)
assert '.rela.text' not in reloc and '.rel.text' not in reloc  # unwind metadata is not executed
subprocess.run([str(tools/'llvm-objcopy.exe'),'--only-section=.text','-O','binary',str(build/'probe.o'),str(build/'probe.bin')],check=True)
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.mem_map(0x100000,0x10000);u.mem_write(0x100000,(build/'probe.bin').read_bytes())
edges=[0,1,23,0x7fffffff,0x80000000,0xffffffff];rng=random.Random(2008)
cases=[(a,b) for a in edges for b in edges]+[(rng.getrandbits(32),rng.getrandbits(32)) for _ in range(128)]
for a,b in cases:
 for reg,value in [(UC_ARM64_REG_X0,0),(UC_ARM64_REG_X1,0),(UC_ARM64_REG_X2,a),(UC_ARM64_REG_X3,b),(UC_ARM64_REG_LR,0x108000)]:u.reg_write(reg,value)
 u.emu_start(0x100000,0x108000,count=32);assert u.reg_read(UC_ARM64_REG_PC)==0x108000
 assert u.reg_read(UC_ARM64_REG_X0)&0xffffffff==(a+23*b)&0xffffffff
report={'apk_sha256':hashlib.sha256(apk.read_bytes()).hexdigest(),'apk_bytes':apk.stat().st_size,'zip_integrity':'PASS','native_library_uncompressed':'PASS','jni_symbol_present':'PASS','arm64_arithmetic_cases':len(cases),'arm64_arithmetic':'PASS','device_install_and_launch':'NOT RUN','ui_gamepad_touch_save_operations':'NOT RUN ON DEVICE'}
(root/'android-verification.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))

