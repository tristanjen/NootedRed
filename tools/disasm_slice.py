"""用 capstone 离线反汇编内核切片里的函数；配合 kernel_slice.SliceImage 使用。

用法（在仓库根目录）：
    python3 -m tools.disasm_slice <Label> <mangled前缀>...   # 按符号名前缀匹配
"""
import sys
from pathlib import Path

import capstone

from kernel_slice import SliceImage

WORK_DEFAULT = Path(__file__).resolve().parents[1] / "build/vcn"


def function(image, name):
    address = image.symbols[name]
    following = [a for a in image.symbols.values() if a > address]
    end = min(following) if following else address + 0x200
    return address, end, image.read(address, end - address)


def disasm_function(image, name, limit=0x4000):
    address, end, code = function(image, name)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.skipdata = True
    md.detail = True
    print(f"\n==== {name}")
    print(f"addr {address:#x} size {end - address:#x}")
    lines = 0
    try:
        iterator = md.disasm(code, address)
        while True:
            try:
                insn = next(iterator)
            except StopIteration:
                break
            target = image.reverse.get(insn.address + insn.size + insn.disp) if insn.mnemonic == "lea" else None
            note = f"   ; {target}" if target else ""
            print(f"  {insn.address:#x}: {insn.bytes.hex():<20} {insn.mnemonic} {insn.op_str}{note}")
            lines += 1
            if lines > 600:
                print("  ... 截断")
                break
    except Exception as error:    # noqa: BLE001
        print(f"  [disasm error] {error}")


def main():
    args = sys.argv[1:]
    label = args[0]
    prefixes = args[1:]
    work = Path(WORK_DEFAULT)
    for candidate in [work, Path(r"C:\Users\Tristan\Desktop\build\vcn")]:
        path = candidate / f"{label}.kc.bin"
        if path.exists():
            work = candidate
            break
    else:
        raise SystemExit(f"找不到 {label}.kc.bin")
    image = SliceImage(work / f"{label}.kc.bin")
    for prefix in prefixes:
        matched = sorted(n for n in image.symbols if n.startswith(prefix) and "MetaClass" not in n
                         and not any(tag in n for tag in ("D0Ev", "D1Ev", "D2Ev", "dlEPvm", "nwEm")))
        for name in matched[:12]:
            try:
                disasm_function(image, name)
            except Exception as error:    # noqa: BLE001
                print(f"[error] {name}: {error}")


if __name__ == "__main__":
    main()
