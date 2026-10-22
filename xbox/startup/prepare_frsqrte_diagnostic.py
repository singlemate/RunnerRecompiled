from pathlib import Path
image=Path('work/xbox-game-private/full-decoded-image.bin').read_bytes()
for address in (0x82384344,0x82EBBF18,0x82EBC040):
    assert image[address-0x82000000:address-0x82000000+4].hex()=='fda00034', 'Unexpected frsqrte opcode'
p=Path('work/xbox-game-private/generated-fourth-final/ppc_recomp.36.cpp')
s=p.read_text()
old='__builtin_trap(); // unsupported guest instruction at 0x82384344'
new='ctx.f13.f64 = me_frsqrte_diagnostic(ctx.f0.f64, 0x82384344); // diagnostic seed, not bit-exact'
assert s.count(old)+s.count(new)==1
if old in s:p.write_text(s.replace(old,new))
p=Path('work/xbox-game-private/generated-fourth-final/ppc_recomp.298.cpp')
s=p.read_text()
for address in ('82EBBF18','82EBC040'):
    old='__builtin_trap(); // unsupported guest instruction at 0x'+address
    new='ctx.f13.f64 = me_frsqrte_diagnostic(ctx.f0.f64, 0x'+address+'); // diagnostic seed, not bit-exact'
    assert s.count(old)+s.count(new)==1, 'Unexpected generated site '+address
    s=s.replace(old,new)
old='__builtin_trap(); // unsupported guest instruction at 0x82EC0968'
new='me_dcbst_diagnostic(ctx.r11.u32); // guarded coherent CPU VM only'
assert s.count(old)+s.count(new)==1
s=s.replace(old,new)
p.write_text(s)
print('Prepared 3 guarded frsqrte sites and 1 coherent-VM dcbst site')
