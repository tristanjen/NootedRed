"""从运行时能力表读取视频设备，生成器不另建 PCI 白名单。"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def parse_entries():
    source = (ROOT / 'NootedRed/VCNCapabilities.hpp').read_text()
    return [(int(device, 16), generation, int(family)) for device, generation, family in re.findall(
        r'\{(0x[0-9A-Fa-f]+),\s*Generation::(VCN1|VCN22),\s*(\d+)\}', source)]


def devices():
    entries = parse_entries()
    nred = (ROOT / 'NootedRed/NRed.cpp').read_text()
    start = nred.index('switch (this->deviceID)')
    end = nred.index('default: PANIC', start)
    accepted = {int(device, 16) for device in re.findall(r'case (0x[0-9A-Fa-f]+):', nred[start:end])}
    if not entries or len(entries) != len({entry[0] for entry in entries}) or {entry[0] for entry in entries} != accepted:
        raise ValueError('VCN 能力表与 NRed 设备分类不一致')
    if any((generation == 'VCN1' and family not in (0, 4)) or (generation == 'VCN22' and family != 5)
           for _, generation, family in entries):
        raise ValueError('未核实的 VA 解码能力分类')
    return [(device, family) for device, generation, family in entries
            if (generation == 'VCN22' and family == 5) or (generation == 'VCN1' and family == 4)]


def device_generations():
    """设备 -> 视频代际（'VCN1'/'VCN22'），供生成器区分补丁集。"""
    return {device: generation for device, generation, _ in parse_entries()}
