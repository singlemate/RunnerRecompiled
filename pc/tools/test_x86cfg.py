"""Self-test of branch decoding on hand-written byte patterns (no game data).

python -I outputs/mirrors-edge-native/pc/tools/test_x86cfg.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import x86cfg as X  # noqa: E402

VA = 0x401000
CASES = [
    ("e8fb0f0000", X.K_CALL, 0x402000),          # call rel32
    ("e9fb0f0000", X.K_JMP, 0x402000),           # jmp rel32
    ("ebfe", X.K_JMP, VA),                        # jmp $
    ("7410", X.K_JCC, VA + 0x12),                # je rel8
    ("0f8510000000", X.K_JCC, VA + 0x16),        # jne rel32
    ("e3fe", X.K_JCC, VA),                        # jecxz $
    ("c3", X.K_RET, None),
    ("c20800", X.K_RET, None),
    ("cc", X.K_STOP, None),
    ("0f0b", X.K_STOP, None),                     # ud2
    ("ff15f0b7a801", X.K_CALL_IAT, 0x1A8B7F0),    # call [disp32]
    ("ff25d0dfa801", X.K_JMP_IAT, 0x1A8DFD0),     # jmp [disp32]
    ("ff248580100f01", X.K_JMP_TABLE, 0x10F1080),  # jmp [eax*4+disp32]
    ("ffe0", X.K_JMP_IND, None),                  # jmp eax
    ("ff5004", X.K_CALL_IND, None),               # call [eax+4]
    ("3effe0", X.K_JMP_IND, None),                # ds-prefixed jmp eax
    ("8bc1", X.K_NONE, None),                     # mov eax, ecx
]


def main():
    fails = 0
    for hexbytes, kind, target in CASES:
        code = bytes.fromhex(hexbytes)
        got = X.branch(code, 0, VA, len(code))
        if got != (kind, target):
            fails += 1
            print(f"FAIL {hexbytes}: got {got}, want {(kind, target)}")
    print(f"{len(CASES) - fails}/{len(CASES)} passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
