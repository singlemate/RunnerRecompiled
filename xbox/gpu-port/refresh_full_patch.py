"""Export the local RexGlue worktree against the pinned v0.10.0 base."""

from pathlib import Path
import subprocess


root = Path.cwd()
sdk = root / "work/references/rexglue-sdk"
target = root / "outputs/mirrors-edge-native/xbox/gpu-port/rexglue-v0.10.0-full.patch"
tracked = subprocess.run(
    ["git", "diff", "--binary", "c94f5eb"], cwd=sdk,
    stdout=subprocess.PIPE, check=True,
).stdout
new_file = subprocess.run(
    ["git", "diff", "--no-index", "--binary", "--", "/dev/null",
     "src/ui/vulkan/vulkan_presenter_window.cpp"],
    cwd=sdk, stdout=subprocess.PIPE,
)
if new_file.returncode != 1:
    raise RuntimeError(f"Unexpected new-file diff status: {new_file.returncode}")
target.write_bytes(tracked + new_file.stdout)
print(f"Wrote {target} ({target.stat().st_size} bytes)")
