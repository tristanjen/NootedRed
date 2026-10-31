#!/usr/bin/env python3
"""从内核集合（.kc）导出 fileset 切片，并反查某个未滑动地址归属哪个 fileset。

内核集合里每个 kext 都是一个 fileset 子项（LC_FILESET_ENTRY），共享同一个 __LINKEDIT；
要静态分析某个符号，需要把它的切片单独存成 Mach-O 再配共享 LINKEDIT 伴生文件。

用法（在 macOS 上）：
  python3 tools/extract_kext_slices.py                      # 列表：每个 fileset 的名字与段范围
  python3 tools/extract_kext_slices.py --filter AMD         # 导出名称含 AMD 的切片到 build/vcn/
  python3 tools/extract_kext_slices.py --contains 0x94B7E0  # 反查该未滑动地址落在哪个 fileset
  python3 tools/extract_kext_slices.py --contains 0x94B7E0 --filter AMD   # 反查并顺便导出
"""
import argparse
import json
import struct
from pathlib import Path

KC_ROOTS = (Path("/System/Volumes/Preboot"), Path("/System/Library/KernelCollections"))
WORK = Path("build/vcn")
FILESET_ENTRY = 0x80000035    # LC_FILESET_ENTRY (0x35 | LC_REQ_DYLD)


def read_segments(handle, fileoff):
    """读子项自己的段表；返回 [(name, vmaddr, vmsize, fileoffset, filesize)]。"""
    handle.seek(fileoff)
    header = handle.read(32)
    if len(header) < 32 or struct.unpack_from("<I", header)[0] != 0xFEEDFACF:
        return []
    ncmds = struct.unpack_from("<I", header, 16)[0]
    commands = handle.read(struct.unpack_from("<I", header, 20)[0])
    cursor, segments = 0, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", commands, cursor)
        if cmd == 0x19:    # LC_SEGMENT_64
            name, addr, vmsize, offset, filesize = struct.unpack_from("<16sQQQQ", commands, cursor + 8)
            segments.append((name.rstrip(b"\0").decode(errors="replace"), addr, vmsize, offset, filesize))
        cursor += size
    return segments


def list_entries(handle):
    """枚举内核集合里的全部 fileset；size 由下一个子项的 fileoff 推得。"""
    handle.seek(0)
    header = handle.read(32)
    if len(header) < 32 or struct.unpack_from("<I", header)[0] != 0xFEEDFACF:
        raise ValueError("不是 64 位 Mach-O 内核集合")
    ncmds = struct.unpack_from("<I", header, 16)[0]
    commands = handle.read(struct.unpack_from("<I", header, 20)[0])
    cursor, entries = 0, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", commands, cursor)
        if cmd == FILESET_ENTRY:
            fileoff = struct.unpack_from("<Q", commands, cursor + 8)[0]
            name_id = struct.unpack_from("<I", commands, cursor + 24)[0]
            end = commands.find(b"\0", cursor + name_id)
            entries.append((commands[cursor + name_id:end].decode(errors="replace"), fileoff))
        cursor += size
    offsets = sorted(fileoff for _, fileoff in entries)
    result = []
    for name, fileoff in entries:
        following = [offset for offset in offsets if offset > fileoff]
        size = min(following[0] - fileoff if following else 1 << 24, 128 << 20)
        result.append((name, fileoff, size))
    return result


def dump_linkedit(handle, blob):
    """子项的符号在共享 __LINKEDIT（kc 文件偏移）里；按需导出伴生文件。"""
    for name, _, _, offset, filesize in read_segments_from_blob(blob):
        if name != "__LINKEDIT" or offset + filesize <= len(blob):
            continue
        target = WORK / ("linkedit-%X-%X.bin" % (offset, filesize))
        if target.exists():
            return target
        handle.seek(offset)
        target.write_bytes(handle.read(filesize))
        return target
    return None


def read_segments_from_blob(blob):
    if struct.unpack_from("<I", blob)[0] != 0xFEEDFACF:
        return []
    ncmds = struct.unpack_from("<I", blob, 16)[0]
    cursor, segments = 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", blob, cursor)
        if cmd == 0x19:
            name, addr, vmsize, offset, filesize = struct.unpack_from("<16sQQQQ", blob, cursor + 8)
            segments.append((name.rstrip(b"\0").decode(errors="replace"), addr, vmsize, offset, filesize))
        cursor += size
    return segments


def main():
    parser = argparse.ArgumentParser(description="导出内核集合里的 fileset 切片")
    parser.add_argument("--filter", default="", help="只导出名字含该子串的 fileset（默认不导出）")
    parser.add_argument("--contains", default="", help="反查该未滑动地址属于哪个 fileset，例如 0x94B7E0")
    parser.add_argument("--kc", default="", help="指定内核集合路径（默认自动查 Preboot 与 KernelCollections）")
    options = parser.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    paths = [Path(options.kc)] if options.kc else [
        path for root in KC_ROOTS if root.is_dir() for path in sorted(root.rglob("*.kc"))]
    wanted = int(options.contains, 16) if options.contains else None
    report, exported = {"collections": []}, []
    for path in paths:
        try:
            handle = path.open("rb")
        except OSError as error:
            print("skip %s: %s" % (path, error))
            continue
        with handle:
            try:
                entries = list_entries(handle)
            except (ValueError, struct.error) as error:
                print("skip %s: %s" % (path, error))
                continue
            for name, fileoff, size in entries:
                segments = read_segments(handle, fileoff)
                report["collections"].append({"kc": str(path), "name": name, "fileoff": fileoff, "size": size,
                                              "segments": segments})
                if wanted is not None and any(a <= wanted < a + s for _, a, s, _, _ in segments):
                    print("0x%X -> %s  (%s)" % (wanted, name, "  ".join(
                        "%s %#x-%#x" % (n, a, a + s) for n, a, s, _, _ in segments)))
                if not options.filter or options.filter.lower() in name.lower():
                    target = WORK / (name.replace("/", "_").replace(".", "_") + ".kc.bin")
                    handle.seek(fileoff)
                    blob = handle.read(size)
                    target.write_bytes(blob)
                    companion = dump_linkedit(handle, blob)
                    exported.append({"name": name, "path": str(target), "linkedit": companion and str(companion)})
                    print("exported %-48s %#010x  %d bytes" % (name, fileoff, size))
    (WORK / "filesets.json").write_text(json.dumps({"exported": exported, "collections": report["collections"]},
                                                   indent=1), encoding="utf-8")
    print("fileset map: %s" % (WORK / "filesets.json"))


if __name__ == "__main__":
    main()
