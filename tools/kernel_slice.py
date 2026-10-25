"""内核集合切片 + 共享 LINKEDIT 的只读解析，供审计与离线分析共用。

内核集合（.kc）的 fileset 子项共享一个 __LINKEDIT；抽取器把每个子项窗口存为
<Label>.kc.bin，共享段存为 linkedit-<off>.bin。本模块按 (切片, linkedit) 对解析
段、符号表与虚表，不依赖 nm/llvm 工具。
"""
import re
import struct
from pathlib import Path

VCN_VTABLE_CLASSES = re.compile(
    r"VCN|UVD|VCE|VideoContext|GraphicsAccelerator|AccelShared|SharedUserClient|SMLInterface|"
    r"Vega10Hardware|Vega12Hardware|Vega20Hardware|GFX9Hardware|Hardware")


def decode_pointer(value):
    """内核集合虚表指针为链式重定位编码；低位 28 位才是 vm 目标，高位是链式字段。"""
    return value & 0xFFFFFFF if value >= 0x10000000 else value


class SliceImage:
    """fileset 子项切片；符号经由共享 LINKEDIT 伴生文件解析。"""

    def __init__(self, path, companion_dirs=()):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        if struct.unpack_from("<I", self.data)[0] != 0xFEEDFACF:
            raise ValueError(f"不是 64 位 Mach-O：{path}")
        self.segments, self.sections, self.symtab = [], [], None
        ncmds = struct.unpack_from("<I", self.data, 16)[0]
        commands = self.data[32:32 + struct.unpack_from("<I", self.data, 20)[0]]
        cursor = 0
        for _ in range(ncmds):
            cmd, size = struct.unpack_from("<II", commands, cursor)
            if cmd == 0x19:    # LC_SEGMENT_64
                name, address, seg_size, offset, file_size, _, _, count, _ = struct.unpack_from(
                    "<16sQQQQIIII", commands, cursor + 8)
                self.segments.append((name.rstrip(b"\0").decode(), address, seg_size, offset, file_size))
                for i in range(count):
                    sec_name, _, address, sec_size, offset = struct.unpack_from(
                        "<16s16sQQI", commands, cursor + 72 + i * 80)[:5]
                    self.sections.append((sec_name.rstrip(b"\0").decode(), address, sec_size, offset))
            elif cmd == 0x2:    # LC_SYMTAB
                self.symtab = struct.unpack_from("<IIII", commands, cursor + 8)
            cursor += size
        self.companion = self._find_companion(companion_dirs)
        self.symbols = self._symbols()
        self.reverse = {address: name for name, address in self.symbols.items()}

    def _find_companion(self, companion_dirs):
        """__LINKEDIT 不在切片内时，按段范围找 linkedit-<off>-<size>.bin 伴生文件。"""
        for _, start, size, offset, file_size in self.segments:
            if _ != "__LINKEDIT":
                continue
            if offset < len(self.data) and offset + min(size, file_size) <= len(self.data):
                return None    # LINKEDIT 已在切片内
            self.companion_expected = f"linkedit-{offset:X}-{size:X}.bin"
            for directory in list(companion_dirs) + [self.path.parent]:
                candidate = Path(directory) / self.companion_expected
                if candidate.exists():
                    return candidate
            return None
        return None

    def _read_file(self, position, length):
        """按 kc 文件偏移读取：切片内先换算为切片相对偏移，其余落入共享 LINKEDIT。"""
        slice_position = position - self._file_base
        if 0 <= slice_position and slice_position + length <= len(self.data):
            return self.data[slice_position:slice_position + length]
        if self.companion is None:
            raise ValueError(f"{self.path.name}: 文件偏移越界且无伴生 LINKEDIT：{position:#x}")
        companion = self.companion.read_bytes()
        begin = position - self._companion_base
        if begin < 0 or begin + length > len(companion):
            raise ValueError(f"{self.path.name}: 伴生 LINKEDIT 范围外：{position:#x}")
        return companion[begin:begin + length]

    @property
    def _file_base(self):
        """切片窗口在 kc 文件中的起始偏移（首个段即 __TEXT，其 off 即入口位置）。"""
        return self.segments[0][3] if self.segments else 0

    @property
    def _companion_base(self):
        if self.companion is None:
            return 0
        m = re.search(r"linkedit-([0-9A-F]+)-", self.companion.name)
        return int(m[1], 16)

    def read(self, address, length):
        for _, start, size, offset, file_size in self.segments:
            if start <= address and address + length <= start + min(size, file_size):
                return self._read_file(offset + address - start, length)
        raise ValueError(f"{self.path.name}: 读取越界：{address:#x}+{length:#x}")

    def _symbols(self):
        if self.symtab is None:
            raise ValueError(f"{self.path.name}: 无符号表")
        offset, count, strings_offset, strings_size = self.symtab
        if offset + count * 16 > len(self.data) and self.companion is None:
            raise ValueError(f"{self.path.name}: 缺少共享 LINKEDIT 伴生文件 {getattr(self, 'companion_expected', '?')}")
        entries = self._read_file(offset, count * 16)
        strings = self._read_file(strings_offset, strings_size)
        result = {}
        for i in range(count):
            index, kind, _, _, address = struct.unpack_from("<IBBHQ", entries, i * 16)
            if kind & 0xE != 0xE or not address:
                continue
            end = strings.find(b"\0", index)
            if end < index:
                continue
            result[strings[index:end].decode(errors="replace")] = address
        return result

    @staticmethod
    def class_name(metaclass):
        m = re.match(r"^__ZN(\d+)(.+?)\d+gMetaClassE$", metaclass)
        if not m or int(m[1]) != len(m[2]):
            return None
        return m[2]

    def vtable(self, metaclass):
        m = re.match(r"^__ZN(\d+)(.+?)\d+gMetaClassE$", metaclass)
        vtable_symbol = f"__ZTV{m[1]}{m[2]}"
        address = self.symbols.get(vtable_symbol)
        if address is None:
            raise ValueError(f"虚表符号缺失：{vtable_symbol}")
        end = min(a for a in self.symbols.values() if a > address)
        entries = struct.unpack("<" + "Q" * ((end - address - 16) // 8), self.read(address + 16, end - address - 16))
        table = {"symbol": vtable_symbol, "address": address, "slots": []}
        for i, encoded in enumerate(entries):
            target = decode_pointer(encoded)
            slot = {"offset": i * 8, "target": target, "symbol": self.reverse.get(target)}
            following = min((a for a in self.symbols.values() if a > target), default=target)
            if target and 0 < following - target <= 32:
                slot["code"] = self.read(target, following - target).hex()
            table["slots"].append(slot)
        return table


def analyze_image(image, vtable_limit=40):
    """返回可 JSON 序列化的内核驱动画像：符号桶、metaclass、虚表。"""
    report = {
        "path": str(image.path),
        "segments": [(name, address, size) for name, address, size, _, _ in image.segments],
        "symbolCount": len(image.symbols),
    }
    buckets = {
        "vcnUvdVce": re.compile(r"VCN|UVD|VCE", re.I),
        "videoContext": re.compile(r"VideoContext"),
        "shared": re.compile(r"AccelShared|SharedUserClient|SMLInterface"),
        "hwEngine": re.compile(r"HWEngine"),
    }
    for label, pattern in buckets.items():
        matched = sorted(((a, n) for n, a in image.symbols.items() if pattern.search(n)))
        report[label] = [f"{name}@{value:X}" for value, name in matched[:3000]]
    metaclasses = sorted(n for n in image.symbols if n.endswith("gMetaClassE"))
    report["metaclasses"] = [SliceImage.class_name(n) or n for n in metaclasses]
    vtables = {}
    for metaclass in metaclasses:
        classname = SliceImage.class_name(metaclass)
        if not classname or not VCN_VTABLE_CLASSES.search(classname):
            continue
        if len(vtables) >= vtable_limit:
            break
        try:
            vtables[classname] = image.vtable(metaclass)
        except Exception as error:    # noqa: BLE001 - 记录失败原因后继续
            vtables[classname] = {"error": f"{type(error).__name__}: {error}"}
    report["vtables"] = vtables
    return report
