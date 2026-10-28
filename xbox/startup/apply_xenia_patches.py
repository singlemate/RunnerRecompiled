"""Applies selected Xenia Canary game patches for Mirror's Edge (45410850, default.xex
hash 25AA0820DF95AA47; xenia-canary/game-patches) to the static recompilation, i.e. to
the generated C++ of the affected guest instructions. Idempotent; keeps <file>.orig.
Each patch lists (function, original C++ line, replacement). Instruction words at the
addresses were checked against the decoded XEX (work/startup/ppc_at.py) before use.
Usage: python apply_xenia_patches.py <project root> [--revert]"""
import shutil, sys
from pathlib import Path

root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
gen = root / 'work/xbox-game-private/generated-fourth-final'
MARK = ' // XENIA PATCH:'
PATCHES = [
    # Unlock FPS (illusion, boma): 0x8254f5fc lfs f1,1152(r3) -> nop; 0x8281492b li r11,2 -> li r11,1
    # Switchable at run time: me_unlock_fps() is false with ME_UNLOCK_FPS=0 (original code).
    ('ppc_recomp.73.cpp', 'sub_8254F5E0', '\ttemp.u32 = PPC_LOAD_U32(ctx.r3.u32 + 1152);\n\tctx.f1.f64 = double(temp.f32);\n',
     '\tif (!me_unlock_fps()) { temp.u32 = PPC_LOAD_U32(ctx.r3.u32 + 1152); ctx.f1.f64 = double(temp.f32); }' + MARK + ' Unlock FPS, 0x8254f5fc lfs f1,1152(r3) -> nop\n'),
    ('ppc_recomp.132.cpp', 'sub_82814838', 'loc_82814928:\n\t// li r11,2\n\tctx.r11.s64 = 2;\n',
     'loc_82814928:\n\t// li r11,2\n\tctx.r11.s64 = me_unlock_fps() ? 1 : 2;' + MARK + ' Unlock FPS, 0x8281492b li r11,2 -> li r11,1\n'),
    # Dynamic Lighting & Shadows Shading Fix (boma): invisible hands/feet from MSAA tiling.
    # Switchable at run time (shadow flicker A/B): me_shading_fix() is false with
    # ME_SHADING_FIX=0, and the original store then runs.
    ('ppc_recomp.39.cpp', 'sub_823AA2B0', '\tPPC_STORE_U32(ctx.r7.u32 + 19380, ctx.r9.u32);\n',
     '\tif (me_shading_fix()) ctx.r9.s64 = 0; else PPC_STORE_U32(ctx.r7.u32 + 19380, ctx.r9.u32);' + MARK + ' Shading Fix, 0x823aa2e0 stw r9,19380(r7) -> li r9,0\n'),
    ('ppc_recomp.39.cpp', 'sub_823AA310', '\tPPC_STORE_U32(ctx.r8.u32 + 19380, ctx.r9.u32);\n',
     '\tif (me_shading_fix()) ctx.r9.s64 = 0; else PPC_STORE_U32(ctx.r8.u32 + 19380, ctx.r9.u32);' + MARK + ' Shading Fix, 0x823aa350 stw r9,19380(r8) -> li r9,0\n'),
]
# Declaration the switchable patches need, added once after the shared include.
DECL = 'extern "C" bool me_shading_fix();' + MARK + ' switch (entry_probe.cpp)\n'
DECL_UNLOCK = 'extern "C" bool me_unlock_fps();' + MARK + ' switch (entry_probe.cpp)\n'
revert = '--revert' in sys.argv
for name, func, old, new in PATCHES:
    path = gen / name
    text = path.read_text(encoding='utf-8')
    head = f'PPC_FUNC_IMPL(__imp__{func})'
    start = text.index(head); end = text.index('\n}\n', start)
    body = text[start:end]
    src, dst = (new, old) if revert else (old, new)
    if body.count(dst) == 1 and body.count(src) == 0:
        print(f'{name} {func}: already {"reverted" if revert else "applied"}'); continue
    assert body.count(src) == 1, f'{name} {func}: expected exactly one match ({body.count(src)})'
    orig = path.with_suffix(path.suffix + '.orig')
    if not orig.exists(): shutil.copy2(path, orig)
    text = text[:start] + body.replace(src, dst) + text[end:]
    include = '#include "ppc_recomp_shared.h"\n'
    for switch, decl in (('me_shading_fix()', DECL), ('me_unlock_fps()', DECL_UNLOCK)):
        if switch not in new: continue
        if revert: text = text.replace(include + decl, include)
        elif decl not in text: text = text.replace(include, include + decl, 1)
    path.write_text(text, encoding='utf-8')
    print(f'{name} {func}: {"reverted" if revert else "applied"}')

# Xenia patch 0x829fb3e7 (li r3,1 -> li r3,0): allow a modified Coalesced.ini
# only while the explicit A/B overlay is active. The original code remains the
# default branch, so ordinary game copies retain the original hash check.
path = gen / 'ppc_recomp.205.cpp'
text = path.read_text(encoding='utf-8')
head = 'PPC_FUNC_IMPL(__imp__sub_829FB3C0)'
start = text.index(head)
end = text.index('\n}\n', start)
body = text[start:end]
old = '\tctx.r3.s64 = 1;\nloc_829FB3E8:'
new = '\tctx.r3.s64 = me_diagnostic_coalesced_enabled() ? 0 : 1;' + MARK + ' Coalesced hash check, 0x829fb3e7\nloc_829FB3E8:'
decl = 'extern "C" bool me_diagnostic_coalesced_enabled();' + MARK + ' switch (entry_probe.cpp)\n'
src, dst = (new, old) if revert else (old, new)
if body.count(src) == 1 and body.count(dst) == 0:
    text = text[:start] + body.replace(src, dst) + text[end:]
    include = '#include "ppc_recomp_shared.h"\n'
    if revert:
        text = text.replace(include + decl, include)
    elif decl not in text:
        text = text.replace(include, include + decl, 1)
    path.write_text(text, encoding='utf-8')
    print(f'{path.name} sub_829FB3C0: {"reverted" if revert else "applied"}')
elif body.count(dst) == 1 and body.count(src) == 0:
    print(f'{path.name} sub_829FB3C0: already {"reverted" if revert else "applied"}')
else:
    raise AssertionError('Coalesced hash-check patch location changed')
