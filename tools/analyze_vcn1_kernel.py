"""离线分析从内核集合提取的 AMD 驱动切片（配合共享 LINKEDIT 伴生文件）。

用法：
    python3 tools/analyze_vcn1_kernel.py [X5000 X5000HWLibs X6000]
文件位于 build/vcn/：<Label>.kc.bin 与 linkedit-<off>-<size>.bin。
"""
import json
import sys
from pathlib import Path

from kernel_slice import SliceImage, analyze_image

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build/vcn"


def main():
    targets = sys.argv[1:] or ["X5000", "X5000HWLibs", "X6000"]
    for label in targets:
        path = WORK / f"{label}.kc.bin"
        if not path.exists():
            print(f"[skip] {path} 不存在")
            continue
        try:
            report = analyze_image(SliceImage(path))
        except Exception as error:    # noqa: BLE001
            print(f"[error] {label}: {type(error).__name__}: {error}")
            continue
        print(f"\n{'='*24} {label} {'='*24}")
        print("segments:", [(n, hex(a), hex(s)) for n, a, s in report["segments"]])
        print("symbols:", report["symbolCount"])
        for bucket in ("vcnUvdVce", "videoContext", "shared", "hwEngine"):
            items = report.get(bucket, [])
            print(f"-- {bucket}: {len(items)}")
            for item in items[:50]:
                print("  ", item[:120])
            if len(items) > 50:
                print(f"    ... 共 {len(items)}")
        print("-- vtables:", list(report.get("vtables", {})))
        output = WORK / f"{label}-kernel-report.json"
        output.write_text(json.dumps(report, indent=1, ensure_ascii=False) + "\n")
        print("saved:", output)


if __name__ == "__main__":
    main()
