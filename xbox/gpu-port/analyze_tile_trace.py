"""Decode me_tile_draw / me_tile_copy records (prepare_tile_trace.py) into a readable sequence."""
import re, sys
from collections import Counter

def s15(v):
    v &= 0x7FFF
    return v - 0x8000 if v & 0x4000 else v

def xy(v):
    return (v & 0x7FFF, (v >> 16) & 0x7FFF)

def fields(line):
    return dict(re.findall(r'(\w+)=([^ ]+)', line))

def h(f, k):
    return int(f[k], 16)

ZFUNC = ['never', 'less', 'equal', 'lequal', 'greater', 'notequal', 'gequal', 'always']
rows = []
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = re.search(r'me_tile_(draw|copy) (.*)', line)
    if not m:
        continue
    kind, f = m[1], fields(m[2])
    off = h(f, 'win_off')
    win = (s15(off), s15(off >> 16))
    surface = h(f, 'surface')
    color0, depth = h(f, 'color0'), h(f, 'depth')
    common = dict(n=int(f['n']), kind=kind, win=win,
                  scissor=(xy(h(f, 'win_tl')), xy(h(f, 'win_br'))),
                  pitch=surface & 0x3FFF, msaa=(surface >> 16) & 3,
                  cbase=color0 & 0xFFF, cfmt=(color0 >> 16) & 0xF,
                  dbase=depth & 0xFFF, dfmt=(depth >> 16) & 1,
                  edram_mode=h(f, 'modectl') & 7)
    if kind == 'draw':
        dc = h(f, 'depthctl')
        common.update(ps=f['ps'][:8], vs=f['vs'][:8], idx=int(f['indices']),
                      z=('test ' + ZFUNC[(dc >> 4) & 7] if dc & 2 else 'off') + (' write' if dc & 4 else ''),
                      stencil=bool(dc & 1), mask=h(f, 'mask'), wdepth=f['ps_writes_depth'],
                      winoff_en=bool(h(f, 'mode_cntl') & (1 << 16)), fetch0=f['fetch0'],
                      vport=(f['vport_x'], f['vport_y']))
    else:
        cc = h(f, 'copy_control')
        common.update(src=cc & 7, sample=(cc >> 4) & 7, cclear=bool(cc & 0x100),
                      dclear=bool(cc & 0x200), cmd=(cc >> 20) & 3, dest=f['dest_base'],
                      dest_info=f['dest_info'], dest_pitch=f['dest_pitch'], dclear_value=f['depth_clear'])
    rows.append(common)

prev = None
for r in rows:
    key = (r['kind'], r['win'], r['scissor'], r['cbase'], r['dbase'], r['msaa'], r.get('ps'), r.get('z'), r.get('mask'))
    if r['kind'] == 'draw':
        text = (f"{r['n']:4} DRAW win={r['win']} sc={r['scissor']} msaa={r['msaa']} pitch={r['pitch']} "
                f"c0={r['cbase']}/f{r['cfmt']} d={r['dbase']}/f{r['dfmt']} mode={r['edram_mode']} "
                f"z={r['z']}{' st' if r['stencil'] else ''} mask={r['mask']:X} ps={r['ps']} vs={r['vs']} "
                f"idx={r['idx']} wdepth={r['wdepth']} woe={int(r['winoff_en'])} tex={r['fetch0']} vp={r['vport']}")
    else:
        text = (f"{r['n']:4} COPY win={r['win']} sc={r['scissor']} msaa={r['msaa']} pitch={r['pitch']} "
                f"c0={r['cbase']}/f{r['cfmt']} d={r['dbase']}/f{r['dfmt']} src={r['src']} sample={r['sample']} "
                f"cmd={r['cmd']} cclear={int(r['cclear'])} dclear={int(r['dclear'])}/{r['dclear_value']} "
                f"dest={r['dest']} info={r['dest_info']} dpitch={r['dest_pitch']}")
    print(text)

print('\nwindow offsets:', Counter(r['win'] for r in rows))
print('draw counts by (win, msaa, dbase, cbase):')
for k, v in Counter((r['win'], r['msaa'], r['dbase'], r['cbase']) for r in rows if r['kind'] == 'draw').most_common():
    print('  ', k, v)
