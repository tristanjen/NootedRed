"""只读审计 VCN1（Raven/Picasso）接入所需的内核与用户态驱动形态。

在目标 macOS 主机上运行：
    python3 tools/audit_vcn1_driver.py [--cache-dir DIR] [--output FILE]

产出 build/vcn/vcn1-audit.json 与终端摘要；不修改任何系统文件。
各分区独立容错：单个驱动或映像解析失败只记录错误，不影响其余部分。
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

from audit_video_driver import disk_image, symbols
from metal_cache import macho

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build/vcn"

# Sequoia 起系统 kext 位于 cryptex；两个位置都尝试。
KERNELS = {
    "X5000": ["/System/Volumes/Preboot/Cryptexes/OS/System/Library/Extensions/AMDRadeonX5000.kext/Contents/"
              "MacOS/AMDRadeonX5000",
              "/System/Library/Extensions/AMDRadeonX5000.kext/Contents/MacOS/AMDRadeonX5000"],
    "X5000HWLibs": ["/System/Volumes/Preboot/Cryptexes/OS/System/Library/Extensions/"
                    "AMDRadeonX5000HWServices.kext/Contents/PlugIns/AMDRadeonX5000HWLibs.kext/Contents/MacOS/"
                    "AMDRadeonX5000HWLibs",
                    "/System/Library/Extensions/AMDRadeonX5000HWServices.kext/Contents/PlugIns/"
                    "AMDRadeonX5000HWLibs.kext/Contents/MacOS/AMDRadeonX5000HWLibs"],
    "X6000": ["/System/Volumes/Preboot/Cryptexes/OS/System/Library/Extensions/AMDRadeonX6000.kext/Contents/MacOS/"
              "AMDRadeonX6000",
              "/System/Library/Extensions/AMDRadeonX6000.kext/Contents/MacOS/AMDRadeonX6000"],
}
# 家族分派与 VCN1/UVD/VCE 路径的关键函数；identify 已确认 family 4 = Vega 20 (VCN1)。
FOLLOWUP_FUNCTIONS = [
    "__ZN9VAFactory13createDecoderEiP9VAContext",
    "__ZN9VAFactory25createDecodeDataProcessorE23DecodeDataProcessorTypeP9VADecoder",
    "__ZN9VAFactory13createEncoderEiP9VAContext",
    "__ZN9VAFactory21createEncodeProcessorEiP9VAEncoder",
    "__ZN9VAFactory16createUVDCommandEP9VAContext",
    "__ZN12AvdVAContext13CreateDecoderEjjjjj",
    "__ZN12AvdVAContext13CreateEncoderEjjjjj",
    "__ZN12AvdVAContext13EncodePictureEP11AVD_CmdInfoP14_CommandRefCon",
    "__ZN17VAAcceleratorInfo14getExtraIdInfoEjjP11_VAHWConfig",
    "__ZN17VAAcceleratorInfo11getVendorIDEPj",
    "__ZN12VAUvdDecoder4InitEj",
    "__ZN12VAUvdDecoder15setupPowerStateEjP21_CWDDEPM_SCHEDULER_IDPjS2_S2_",
    "__ZN12VAVcnDecoder4InitEj",
    "__ZN12VAUveEncoder4initEj",
    "__ZN12VAUveEncoder9configureEjPv",
    "__ZN12VAUveEncoder15getFeedbackSizeEv",
    "__ZN12VAUveEncoder22getEncodePackageParamsEjPv",
    "__ZN12VAUveEncoder22setEncodePackageParamsEjPv",
    "__ZN12VAAvcEncoder4initEj",
    "__ZN12VAHevcEncoder4initEj",
    "__ZN13UvdEncCommand18buildCreateSessionEP25_HevcEncCreateSessionInfoP9VAEncoder",
    "__ZN13UvdEncCommand19buildGeneralCommandEP15_HevcEncCmdInfo",
    "__ZN9UvdEngine11startEngineE21eAMDAccelVideoChannel",
    "__ZN9UvdEngine10stopEngineEv",
    "__ZN9UvdEngine16createUvdContextEP23sVideoContextCreateInfoP23sVideoContextReturnInfo",
    "__ZN9VceEngine11startEngineEj",
    "__ZN9VceEngine16createVceContextEP23sVideoContextCreateInfoP23sVideoContextReturnInfo",
]
VA_IMAGE = "/System/Library/Extensions/AMDRadeonVADriver2.bundle/Contents/MacOS/AMDRadeonVADriver2"
GVA_MATCH = "AppleGVA.framework"
# 与 tools/fixtures/sequoia-compat.json 的 video 键一致；用于比对用户构建与作者 15.8.1 档案。
PROFILE_VIDEO = [
    "__ZN11VACPFactory8createCPEP11_VAHWConfigP11VAIOConnectPP4VACp",
    "__ZN17VAAcceleratorInfo8identifyEjj",
    "__ZN18VAAddrLibInterface4initEP11VAIOConnectP13sHardwareInfo",
    "__ZN4Addr2V27Gfx9Lib20HwlConvertChipFamilyEjj",
    "__ZN4Addr3Lib6CreateEPK18_ADDR_CREATE_INPUTP19_ADDR_CREATE_OUTPUT",
    "__ZN9VAFactory20createGraphicsEngineEP9VAContext",
    "__ZN9VAFactory14createImageBltEP9VAContextj",
    "__ZN9VAFactory17createDeblockerVPEi",
    "__ZN9VAFactory17createDitheringVPEi",
    "__ZN9VAFactory20createDeinterlacerVPEi",
    "__ZN9VAFactory29createColourFormatConverterVPEi",
    "__ZN9VAFactory31createDynamicContrastEnhancerVPEi",
    "__ZN14Vcn2EncCommand15getFeedbackSizeEv",
    "__ZN26AVDVcn2AvcEncDataProcessor19ProcessEncodeStatusEP19_VAEncodeStatusInfoPvP15VAVendorTexture",
    "__ZN27AVDVcn2HevcEncDataProcessor19ProcessEncodeStatusEP19_VAEncodeStatusInfoPvP15VAVendorTexture",
    "__ZN11Vcn2Encoder15setEncodeParamsEPvP15VAVendorTexture",
]
KERNEL_BUCKETS = {
    "vcnUvdVce": re.compile(r"VCN|UVD|VCE", re.I),
    "videoContext": re.compile(r"VideoContext"),
    "shared": re.compile(r"AccelShared|SharedUserClient|SMLInterface"),
    "engine": re.compile(r"HWEngine|Engine", re.I),
}
KERNEL_VTABLE_CLASSES = re.compile(
    r"VCN|UVD|VCE|VideoContext|GraphicsAccelerator|AccelShared|SharedUserClient|SMLInterface|"
    r"Vega10Hardware|Vega20Hardware|GFX9Hardware")
VA_BUCKETS = {
    "vcn1": re.compile(r"Vcn1|VCN1"),
    "vcn2": re.compile(r"Vcn2|VCN2"),
    "uvd": re.compile(r"Uvd|UVD"),
    "vce": re.compile(r"Vce|VCE"),
    "encode": re.compile(r"Enc(oder|Command|DataProcessor|odeStatus|odeParams)|Encode"),
    "decode": re.compile(r"Dec(oder|odeStatus|DataProcessor|Command)"),
    "accelInfo": re.compile(r"AcceleratorInfo|identify|HWInfo"),
}
MAX_BUCKET = 3000
MAX_CODE = 8192
MAX_DISASM = 64


def run(command, timeout=60):
    return subprocess.run(command, check=True, capture_output=True, text=True, timeout=timeout)


def class_name(symbol):
    # __ZN<len><name>10gMetaClassE -> name（含 AMDRadeonXxxxx_ 前缀）
    m = re.match(r"^__ZN(\d+)(.+?)\d+gMetaClassE$", symbol)
    if not m or int(m[1]) != len(m[2]):
        return None
    return m[2]


def kernel_symbols(path):
    data, read = disk_image(path)
    result = run(["nm", "-n", str(path)])
    names = {}
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) == 3:
            try:
                names[fields[2]] = int(fields[0], 16)
            except ValueError:
                pass
    return data, read, names


def enrich_vtable(read, names, metaclass):
    # 由 metaclass 符号精确构造 __ZTV<len><Class>，不做后缀猜测。
    m = re.match(r"^__ZN(\d+)(.+?)\d+gMetaClassE$", metaclass)
    if not m or int(m[1]) != len(m[2]):
        raise ValueError(f"无法解析 metaclass：{metaclass}")
    vtable_symbol = f"__ZTV{m[1]}{m[2]}"
    address = names.get(vtable_symbol)
    if address is None:
        raise ValueError(f"虚表符号缺失：{vtable_symbol}")
    end = min(a for a in names.values() if a > address)
    entries = struct.unpack("<" + "Q" * ((end - address - 16) // 8), read(address + 16, end - address - 16))
    table = {"symbol": vtable_symbol, "symbolAddress": address, "addressPoint": address + 16,
             "slots": [{"offset": i * 8, "target": a} for i, a in enumerate(entries)]}
    reverse = {a: name for name, a in names.items()}
    for slot in table["slots"]:
        slot["symbol"] = reverse.get(slot["target"])
        following = min((a for a in names.values() if a > slot["target"]), default=slot["target"])
        if slot["target"] and 0 < following - slot["target"] <= 32:
            slot["code"] = read(slot["target"], following - slot["target"]).hex()
    return table


def audit_kernel(report):
    for label, candidates in KERNELS.items():
        entry = {}
        report["kernel"][label] = entry
        path = next((Path(p) for p in candidates if Path(p).exists()), None)
        if path is None:
            entry["error"] = "未找到驱动二进制：" + " | ".join(candidates)
            continue
        entry["path"] = str(path)
        try:
            data, read, names = kernel_symbols(path)
        except Exception as error:    # noqa: BLE001 - 审计需要完整记录失败原因
            entry["error"] = f"{type(error).__name__}: {error}"
            continue
        entry["sha256"] = hashlib.sha256(data).hexdigest()
        entry["symbolCount"] = len(names)
        for bucket, pattern in KERNEL_BUCKETS.items():
            entry[bucket] = sorted(n for n in names if pattern.search(n))[:MAX_BUCKET]
        metaclasses = sorted(n for n in names if n.endswith("gMetaClassE"))
        entry["metaclasses"] = [class_name(n) or n for n in metaclasses]
        vtables = {}
        for metaclass in metaclasses:
            classname = class_name(metaclass)
            if not classname or not KERNEL_VTABLE_CLASSES.search(classname):
                continue
            if len(vtables) >= 24:
                break
            try:
                vtables[classname] = enrich_vtable(read, names, metaclass)
            except Exception as error:    # noqa: BLE001
                vtables[classname] = {"error": f"{type(error).__name__}: {error}"}
        entry["vtables"] = vtables


class SharedCache:
    """与 metal_cache.CacheReader 相同的只读解析，但不校验缓存 UUID。"""

    def __init__(self, directory):
        self.directory = Path(directory)
        self.files, self.mappings = [], []
        self.uuid = self.magic = None
        self.base = 0
        for path in sorted(self.directory.glob("dyld_shared_cache_x86_64h*")):
            if path.suffix in (".map", ".atlas"):
                continue
            handle = path.open("rb")
            self.files.append(handle)
            header = handle.read(104)
            if not header.startswith(b"dyld_v1"):
                continue
            offset, count = struct.unpack_from("<II", header, 16)
            handle.seek(offset)
            data = handle.read(count * 32)
            for i in range(count):
                address, size, file_offset, _, _ = struct.unpack_from("<QQQII", data, i * 32)
                self.mappings.append((address, address + size, file_offset, handle))
            if path.name == "dyld_shared_cache_x86_64h":
                self.magic, self.uuid = header[:16], header[88:104]
                self.base = struct.unpack_from("<Q", data)[0]
        if self.uuid is None:
            raise ValueError("主缓存缺失：dyld_shared_cache_x86_64h")

    def image_base(self, matcher):
        current, base = None, None
        for line in (self.directory / "dyld_shared_cache_x86_64h.map").read_text().splitlines():
            if line.startswith("/"):
                current, base = line, None
                continue
            if current and matcher(current) and base is None:
                m = re.match(r"\s+(__\w+)\s+(0x[0-9a-fA-F]+)", line)
                if m:
                    base = int(m[2], 16)
                    return base, current
        return None, None

    def read(self, address, length):
        for start, end, offset, handle in self.mappings:
            if start <= address and address + length <= end:
                handle.seek(offset + address - start)
                data = handle.read(length)
                if len(data) != length:
                    raise ValueError("缓存读取不完整")
                return data
        raise ValueError(f"缓存地址未映射：{address:#x}")

    def close(self):
        for handle in self.files:
            handle.close()


def disasm(code, label):
    # 与 build_video_decode_patch.compile_payload 相同的字节回灌手法；失败不影响审计。
    asm, obj = WORK / f"audit-{label}.s", WORK / f"audit-{label}.o"
    asm.write_text(".text\n.globl _dump\n_dump:\n.byte " + ",".join(str(v) for v in code) + "\n")
    run(["xcrun", "clang", "-c", str(asm), "-o", str(obj)])
    return run(["xcrun", "llvm-objdump", "-d", str(obj)]).stdout


def audit_function(cache, names, name, *, dump_disasm):
    address = names[name]
    end = min(a for a in names.values() if a > address)
    code = cache.read(address, end - address)
    entry = {"address": address, "size": len(code), "sha256": hashlib.sha256(code).hexdigest(),
             "code": code[:MAX_CODE].hex()}
    if dump_disasm:
        try:
            entry["disasm"] = disasm(code[:MAX_CODE], name[-48:].replace("/", "_"))
        except Exception as error:    # noqa: BLE001
            entry["disasmError"] = f"{type(error).__name__}: {error}"
    return entry


def audit_userspace(report, cache):
    userspace = report["userspace"]
    base, image_path = cache.image_base(lambda p: p == VA_IMAGE)
    if base is None:
        userspace["error"] = f"映像不在缓存 map 中：{VA_IMAGE}"
        return
    names, sections = symbols(cache.read, base)
    userspace["image"] = VA_IMAGE
    userspace["base"] = base
    userspace["symbolCount"] = len(names)
    for bucket, pattern in VA_BUCKETS.items():
        userspace[bucket] = sorted(n for n in names if pattern.search(n))[:MAX_BUCKET]

    functions = {}
    for name in PROFILE_VIDEO + [n for n in names if "VAAcceleratorInfo8m_hwInfoE" in n] + FOLLOWUP_FUNCTIONS:
        if name in functions:
            continue
        if name not in names:
            functions[name] = {"missing": True}
            continue
        functions[name] = audit_function(cache, names, name, dump_disasm=True)
    extra = sorted({n for bucket in ("vcn1", "uvd", "vce") for n in userspace.get(bucket, [])}
                   | {n for n in userspace.get("encode", [])})
    for name in extra:
        if len(functions) >= 120 or name in functions:
            continue
        want_disasm = len([v for v in functions.values() if "disasm" in v]) < MAX_DISASM
        try:
            functions[name] = audit_function(cache, names, name, dump_disasm=want_disasm)
        except Exception as error:    # noqa: BLE001
            functions[name] = {"error": f"{type(error).__name__}: {error}"}
    userspace["functions"] = functions

    profile_path = ROOT / "tools/fixtures/sequoia-compat.json"
    if profile_path.exists():
        try:
            profile = json.loads(profile_path.read_text())
            comparison = {"profileBuild": profile.get("build"), "profileUUID": profile.get("uuid"),
                          "liveUUID": cache.uuid.hex(), "video": {}}
            for name, meta in profile.get("video", {}).items():
                live = functions.get(name, {}).get("sha256")
                comparison["video"][name] = ("missing" if name not in names else
                                             "match" if live == meta.get("sha256") else "differ")
            gva = profile.get("gva", {})
            if "address" in gva and "sha256" in gva:
                try:
                    size = gva.get("size", 0)
                    code = cache.read(gva["address"], size)
                    comparison["gvaAtProfileAddress"] = ("match" if hashlib.sha256(code).hexdigest() == gva["sha256"]
                                                        else "differ")
                except Exception as error:    # noqa: BLE001
                    comparison["gvaAtProfileAddress"] = f"{type(error).__name__}: {error}"
            userspace["profileComparison"] = comparison
        except Exception as error:    # noqa: BLE001
            userspace["profileError"] = f"{type(error).__name__}: {error}"

    gva_base, gva_path = cache.image_base(lambda p: GVA_MATCH in p)
    gva = {"image": gva_path}
    userspace["appleGVA"] = gva
    if gva_base is None:
        gva["error"] = "AppleGVA 映像不在缓存 map 中"
        return
    gva["base"] = gva_base
    try:
        gva_names, _ = symbols(cache.read, gva_base)
        gva["symbolCount"] = len(gva_names)
        gva["exports"] = sorted(gva_names)[:800]
    except Exception as error:    # noqa: BLE001
        gva["error"] = f"{type(error).__name__}: {error}"


def system_info(report):
    info = report["system"]
    try:
        info["sw_vers"] = run(["sw_vers"]).stdout.strip()
    except Exception as error:    # noqa: BLE001
        info["sw_vers"] = f"{type(error).__name__}: {error}"
    try:
        info["uname"] = run(["uname", "-r"]).stdout.strip()
    except Exception as error:    # noqa: BLE001
        info["uname"] = f"{type(error).__name__}: {error}"
    try:
        ioreg = run(["ioreg", "-l", "-w0", "-r", "-n", "IGPU"], timeout=120).stdout.splitlines()
        info["ioregNRed"] = [line.strip() for line in ioreg
                             if "NRed" in line or "device-id" in line or "revision-id" in line][:400]
    except Exception as error:    # noqa: BLE001
        info["ioregError"] = f"{type(error).__name__}: {error}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path,
                        default=Path("/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld"))
    parser.add_argument("--output", type=Path, default=WORK / "vcn1-audit.json")
    args = parser.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    report = {"kernel": {}, "userspace": {}, "system": {}}

    system_info(report)
    audit_kernel(report)
    cache = None
    try:
        cache = SharedCache(args.cache_dir)
        report["cache"] = {"directory": str(args.cache_dir), "uuid": cache.uuid.hex(),
                           "magic": cache.magic.hex(), "base": cache.base}
        audit_userspace(report, cache)
    except Exception as error:    # noqa: BLE001 - 顶层也要留下完整失败原因
        report["userspace"]["error"] = f"{type(error).__name__}: {error}"
    finally:
        if cache:
            cache.close()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=1, ensure_ascii=False) + "\n")
    print("== VCN1 audit ==")
    print("system:", report["system"].get("sw_vers"), "|", report["system"].get("uname"))
    for label, entry in report["kernel"].items():
        if "error" in entry:
            print(f"kernel {label}: ERROR {entry['error']}")
        else:
            print(f"kernel {label}: symbols={entry['symbolCount']} vcnUvdVce={len(entry['vcnUvdVce'])} "
                  f"videoContext={len(entry['videoContext'])} vtables={list(entry['vtables'])}")
    userspace = report["userspace"]
    if "error" in userspace:
        print("userspace: ERROR", userspace["error"])
    else:
        print("cache uuid:", userspace.get("profileComparison", {}).get("liveUUID") or report["cache"]["uuid"])
        for bucket in VA_BUCKETS:
            print(f"va {bucket}: {len(userspace.get(bucket, []))}")
        comparison = userspace.get("profileComparison", {})
        video = comparison.get("video", {})
        if video:
            print("profile 15.8.1 comparison:",
                  {state: list(video.values()).count(state) for state in ("match", "differ", "missing")})
    print("saved:", args.output)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:    # noqa: BLE001 - 直接给用户可读的错误
        print(f"audit failed: {type(error).__name__}: {error}", file=sys.stderr)
        sys.exit(1)
