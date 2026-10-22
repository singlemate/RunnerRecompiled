"""Opt-in compatibility experiment for the observed original worker selection loop.

Only modifies private generated code. Original Xbox bytes and default execution
remain unchanged. Verify source opcode bytes before introducing the branch.
"""
from pathlib import Path
import subprocess

r = Path.cwd()
image = (r/'work/xbox-game-private/full-decoded-image.bin').read_bytes()
assert image[0x66a8fc:0x66a904].hex() == '357effff4080ffc8'
p = r/'work/xbox-game-private/generated-fourth-final/ppc_recomp.97.cpp'
s = p.read_text()
begin = s.index('PPC_FUNC_IMPL(__imp__sub_8266A880)')
end = s.index('PPC_FUNC_IMPL(__imp__sub_8266A990)',begin)
part = s[begin:end]
marker = '// opt-in worker scan: preserve default original branch'
if marker not in part:
    old = '\t// lwz r11,16(r31)'
    assert part.count(old) == 2
    part = part.replace(old,'loc_ME_POOL_SCAN_NEXT:\n'+old,1)
    old = '\tif (!ctx.cr0.lt) goto loc_8266A8C8;'
    assert part.count(old)==1
    new = '''\t// opt-in worker scan: preserve default original branch
\tif (me_pool_scan_enabled()) {
\t\tctx.r30.u64 = ctx.r11.u64;
\t\tif (!ctx.cr0.lt) goto loc_ME_POOL_SCAN_NEXT;
\t} else if (!ctx.cr0.lt) goto loc_8266A8C8;'''
    part=part.replace(old,new)
    p.write_text(s[:begin]+part+s[end:])
else:
    assert part.count(marker)==1 and part.count('loc_ME_POOL_SCAN_NEXT:')==1

cc=r/'work/toolchains/android-ndk-r30/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
obj=r/'work/xbox-game-private/arm64-objects-mmio/ppc_recomp.97.o'
subprocess.run([str(cc),'--target=aarch64-linux-android35','-std=c++17','-DSIMDE_ENABLE_NATIVE_ALIASES',
    '-I'+str(r/'work/references/XenonRecomp/thirdparty/simde'),'-O1','-fPIC','-Werror=asm-operand-widths',
    '-include',str(r/'outputs/mirrors-edge-native/xbox/startup/guest_memory_access.h'),'-c',str(p),'-o',str(obj)],check=True)
subprocess.run([str(cc.parent/'llvm-ar.exe'),'rcs',str(r/'work/xbox-game-private/libmirrorsedge_ppc_mmio.a'),str(obj)],check=True)
print('Prepared opt-in worker scan and rebuilt private TU97')
