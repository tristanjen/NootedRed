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
LC_SEGMENT_64 = 0x19
MH_MAGIC_64 = 0xFEEDFACF
MAX_COMMANDS = 4096


def load_commands(blob):
    """返回 (header, commands)；header 至少 32 字节且是 64 位 Mach-O。"""
    if len(blob) < 32 or struct.unpack_from("<I", blob)[0] != MH_MAGIC_64:
        return None, b""
    ncmds = struct.unpack_from("<I", blob, 16)[0]
    if ncmds == 0 or ncmds > MAX_COMMANDS:
        return None, b""
    return ncmds, blob[32:32 + struct.unpack_from("<I", blob, 20)[0]]


def read_segments(handle, fileoff):
    """读子项自己的段表；返回 [(name, vmaddr, vmsize, fileoffset, filesize)]。"""
    total = handle.seek(0, 2)
    if fileoff < 0 or fileoff >= total:
        return []
    handle.seek(fileoff)
    ncmds, commands = load_commands(handle.read(1 << 20))
    segments, cursor = [], 0
    for _ in range(ncmds or 0):
        if cursor + 8 > len(commands):
            break
        cmd, size = struct.unpack_from("<II", commands, cursor)
        if size < 8:
            break
        if cmd == LC_SEGMENT_64 and cursor + 56 <= len(commands):
            name, addr, vmsize, offset, filesize = struct.unpack_from("<16sQQQQ", commands, cursor + 8)
            segments.append((name.rstrip(b"\0").decode(errors="replace"), addr, vmsize, offset, filesize))
        cursor += size
    return segments


def list_entries(handle):
    """枚举内核集合里的全部 fileset；size 由下一个子项的 fileoff 推得。"""
    total = handle.seek(0, 2)
    handle.seek(0)
    ncmds, commands = load_commands(handle.read(1 << 20))
    if ncmds is None:
        raise ValueError("不是 64 位 Mach-O 内核集合")
    cursor, entries = 0, []
    for _ in range(ncmds):
        if cursor + 8 > len(commands):
            break
        cmd, size = struct.unpack_from("<II", commands, cursor)
        if size < 8:
            break
        # LC_FILESET_ENTRY: cmd, cmdsize, vmaddr(+8), fileoff(+16), entry_id(+24), reserved(+28)
        if cmd == FILESET_ENTRY and size >= 32:
            fileoff = struct.unpack_from("<Q", commands, cursor + 16)[0]
            name_id = struct.unpack_from("<I", commands, cursor + 24)[0]
            end = commands.find(b"\0", cursor + name_id)
            name = commands[cursor + name_id:end].decode(errors="replace") if end > 0 else ""
            if name.isprintable() and 0 < fileoff < total:
                entries.append((name, fileoff))
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
    ncmds, commands = load_commands(blob)
    cursor = 0
    for _ in range(ncmds or 0):
        if cursor + 8 > len(commands):
            break
        cmd, size = struct.unpack_from("<II", commands, cursor)
        if size < 8:
            break
        if cmd == LC_SEGMENT_64 and cursor + 72 <= len(commands):
            name, _, _, offset, filesize = struct.unpack_from("<16sQQQQ", commands, cursor + 8)
            if name.rstrip(b"\0") == b"__LINKEDIT" and offset + filesize > len(blob):
                target = WORK / ("linkedit-%X-%X.bin" % (offset, filesize))
                if not target.exists():
                    handle.seek(offset)
                    target.write_bytes(handle.read(filesize))
                return target
        cursor += size
    return None


def main():
    parser = argparse.ArgumentParser(description="导出内核集合里的 fileset 切片")
    parser.add_argument("--filter", default="", help="只导出名字含该子串的 fileset（默认不导出）")
    parser.add_argument("--contains", default="", help="反查该未滑动地址属于哪个 fileset，例如 0x94B7E0")
    parser.add_argument("--kc", default="", help="指定内核集合路径（默认自动查 Preboot 与 KernelCollections）")
    options = parser.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    paths = [Path(options.kc)] if options.kc else [
        path for root in KC_ROOTS if root.is_dir() for path in sorted(root.rglob("*.kc"))]
    if not paths:
        raise SystemExit("没找到内核集合，请用 --kc 指定")
    wanted = int(options.contains, 16) if options.contains else None
    report, exported, matched = [], [], []
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
            print("%s: %d fileset(s)" % (path, len(entries)))
            for name, fileoff, size in entries:
                try:
                    segments = read_segments(handle, fileoff)
                except (ValueError, struct.error) as error:
                    print("  skip %s: %s" % (name, error))
                    continue
                report.append({"kc": str(path), "name": name, "fileoff": fileoff, "size": size, "segments": segments})
                if wanted is not None and any(addr <= wanted < addr + span for _, addr, span, _, _ in segments):
                    matched.append(name)
                    print("0x%X -> %s" % (wanted, name))
                if not options.filter or options.filter.lower() in name.lower():
                    target = WORK / (name.replace("/", "_").replace(".", "_") + ".kc.bin")
                    handle.seek(fileoff)
                    blob = handle.read(size)
                    target.write_bytes(blob)
                    companion = dump_linkedit(handle, blob)
                    exported.append({"name": name, "path": str(target), "linkedit": companion and str(companion)})
                    print("  exported %-46s %#010x  %d bytes" % (name, fileoff, size))
    (WORK / "filesets.json").write_text(json.dumps({"exported": exported, "collections": report}, indent=1),
                                        encoding="utf-8")
    if wanted is not None:
        print("0x%X matched: %s" % (wanted, ", ".join(matched) or "none"))
    print("fileset map: %s" % (WORK / "filesets.json"))


if __name__ == "__main__":
    main()
