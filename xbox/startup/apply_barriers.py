"""Inserts real memory barriers for the PowerPC barrier instructions in the generated code.

XenonRecomp emits only a comment for `sync`, `lwsync` and `eieio`, so nothing orders the
guest's memory accesses. The guest threads run in parallel on ARM cores here, and the
game relies on these barriers to publish data between threads (a loader thread fills a
list, the main thread walks it). Without them a reader can see a half-written object:
the crash at lr 8249b888 (sub_8249B6E8 walking a list whose next pointer read as 0x1e)
on the tutorial -> "Edge" transition is the observed case.

  sync          -> __atomic_thread_fence(__ATOMIC_SEQ_CST)   (dmb ish)
  lwsync, eieio -> __atomic_thread_fence(__ATOMIC_ACQ_REL)   (dmb ish)

Idempotent: a barrier line already followed by the fence is left alone. Re-run after
regenerating the code (like apply_xenia_patches.py). Usage: apply_barriers.py <root> [--revert]
"""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path.cwd()
revert = '--revert' in sys.argv
generated = root / 'work/xbox-game-private/generated-fourth-final'
FENCES = {'sync': '__ATOMIC_SEQ_CST', 'lwsync': '__ATOMIC_ACQ_REL', 'eieio': '__ATOMIC_ACQ_REL'}
barrier = re.compile(r'^\t// (sync|lwsync|eieio) ?$')
marker = '/* me_barrier */'
changed_files = 0
inserted = 0
for path in sorted(generated.glob('ppc_recomp.*.cpp')):
    lines = path.read_text(encoding='utf-8').split('\n')
    out = []
    changed = False
    i = 0
    while i < len(lines):
        line = lines[i]
        if revert and marker in line:
            changed = True
            i += 1
            continue
        out.append(line)
        m = barrier.match(line)
        if m and not revert:
            following = lines[i + 1] if i + 1 < len(lines) else ''
            if marker not in following:
                out.append(f'\t__atomic_thread_fence({FENCES[m.group(1)]}); {marker}')
                inserted += 1
                changed = True
        i += 1
    if changed:
        path.write_text('\n'.join(out), encoding='utf-8')
        changed_files += 1
print(f"{'reverted' if revert else 'inserted'} barriers: {inserted if not revert else 'all'} in {changed_files} files")
