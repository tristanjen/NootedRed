# VCN1 / Picasso handoff

Ryzen 5 3500U（Picasso，iGPU `1002:15D8`，VCN 1.0）在 macOS 15.8.1 (24H32) 上打通**硬件视频解码**。
硬编尽力而为（HEVC 可期，AVC 无专用命令类）。

## 仓库与分支

| 项 | 值 |
|---|---|
| fork | https://github.com/tristanjen/NootedRed |
| 分支 | `vcn1-upstream`（基于 upstream/master `690e6c5` 重放 VCN1 增量） |
| 主检出 | `/Users/tristanjen/Desktop/NootedRed`（含切片与 capstone） |
| 工作树 | `/Users/tristanjen/Desktop/NootedRed/build/up-tree` ← 在这里改代码/构建 |
| 最新提交 | `09e1657` VPF: Regenerate the manifest with the missing 15D8 decode entry；docs（本轮） |

**规则**：每次改完文件都要提交并 push：

```bash
git -c http.version=HTTP/1.1 push "https://x-access-token:$(/usr/local/bin/gh auth token)@github.com/tristanjen/NootedRed.git" vcn1-upstream
```

（github.com:443 偶发 `Failed to connect ... after 75013 ms` / `HTTP2 framing layer` / `SSL_ERROR_SYSCALL`，重试即可；`failed to store: 100001` 是 keychain 警告，无害。）

## 构建

```bash
cd /Users/tristanjen/Desktop/NootedRed/build/up-tree
nohup xcodebuild -project NootedRed.xcodeproj -scheme NootedRed -configuration Release -derivedDataPath build/ddr build > build/vcn/buildNN.log 2>&1 &
```

产物：`build/ddr/Build/Products/Release/NootedRed.kext`（及 `NootedRed-0.9.0-RELEASE.zip`）。
单次命令别跑太久（工具上限 120s），xcodebuild 用 `nohup` 后台跑再轮询日志。

## 实机验证回路

装 kext 到 EFI/OC/Kexts → 重启 → Safari/QuickTime 播 1080p（不要 IINA 软解）→

```bash
ioreg -l | grep -o '"NRed[A-Za-z0-9]*" = [0-9a-fA-Fx]*' > ~/Desktop/vcn1-boot.txt
```

注意：属性是「最后一次写入」，探针不要覆盖自己要读的基准值。

## 已打通的部分

- 内核侧 VCN1 路径：`AMDUVD7HWEngine` 单引擎挂进 X5000 的 UVD0 槽，`EngineInitResult=1`、`EngineInitType=5`、`EngineBase=0x7E00`、`RingsDec=1`、`ChannelsDec=1`。
- SML/TTL：`[hardware+0x338]` 是 `AmdTtlServices`（X5000HWLibs），槽 `+0x58` = `queryHwBlockRegisterBase`，已按实例 vptr 挂钩。
- CAIL：`[hardware+0x340]` 是 `AmdCailServices`；`earlyInitialize`/`initialize` 都成功（`CailEarly=0`、`CailInit=0` 是成功码），VBIOS 签名 0x55/0xAA 通过。
- 用户态 VA 驱动补丁表含 0x15D8（family 4），播放时 `NRedVCNUserApplied=16`、`DecoderPID` 出现。
- 视频上下文：`accel+0x1F10` bit3 本来就置位，`NewCtxResult=1`、`VideoCtxStart=1`、`VcQueueUVD0=1`。
- 内核 VCN1 初始化：Greenland ASIC init 的 MCIL 等待用返回 0 的桩绕过后，`InitHardwareCail=1`、
  `HardwareInitDec=1`、`ProbeInitResult=1`（2026-11-07 实机）。

## 当前阻塞点

**内核侧已通**（2026-11-07 18:00 dump）：`CailInitDelta=0x5AFB7`、`CailFamily=0x8E`、`CailRevision=0x42`、
`CailInitFirstResult=1`、`CailStubResult=0`、`CailInitResult=0`、`CailMcilLow`/`CailMcilWaitLow` 非零，
`InitHardwareCail=1`、`CailDec=1`、`HardwareInitDec=1`、`ProbeInitResult=1`。`[adapter+0x12D8]` 确认为
`_greenland_uvd_init`(0x4851476)，失败点是 `_Cail_MCILWaitFor` 等 UVD 寄存器 0x83AF bit1（详见
`docs/vcn1-analysis.md` §13）。桩已生效，CAIL 初始化链路走通。

**用户态阻塞**：`CapUVD0`/`CapResult` 未出现，`getCapabilities` 从未被调用；QuickTime 日志
`(AppleGVA) GVA error: AVD_api.Create failed err=5`，`UserApplied`/`DecoderPID` 都没有。dump 的
共享缓存阶段是 D00/01/03/04/06/09，`UserLastError=4`（KERN_INVALID_ARGUMENT），`DynamicMatched=1`
但没有 Applied。

已定位：`patchSharedRegion` 在 `selectFairPlayDecode` 成功后调用
`VPF::ManifestData::find(Vcn2Decode, OsDarwin24, 0x15D8)`，而提交的 `PatchManifestData.hpp` 是陈旧的
（1276959 加了 `Patches15D8` 却没重跑生成器），Vcn2Decode 只有 15E7/1636/1638/164C → 返回 null →
`CodeMismatch` + `KERN_INVALID_ARGUMENT`，与 dump 完全吻合。

修复：`python3 tools/vpf/generate_manifest.py --write`，新增 `Sites_Vcn2Decode_Darwin24_15D8` 与
15D8 的 `FeatureManifest`（33 个功能、354 个站点）；`--check`、`validate_manifest.py`、
`test_vpf_manifest.py`、`test_display_manifest.py` 全通过。详见 `docs/vcn1-analysis.md` §14。

## 下一步

1. 安装新 `build/ddr/Build/Products/Release/NootedRed.kext` 重启，播 30 秒 QuickTime 视频，读
   `NRedVCNUserApplied`、`NRedVCNDecoderPID`、`NRedVCNUserD15`（Applied）以及
   `NRedVCN1CapUVD0`/`NRedVCN1CapResult`。
2. 若 `CapResult` 非零 → 用户态已开始查询硬件能力，确认 `UsingHardwareAcceleratedVideoDecoder`，
   与软解做像素比对。
3. 若仍无 Applied，按新的 `NRedVCNUserLastError`/阶段位和 `DRMVideoLastStage` 继续查
   （下一个可能点：`adaptPatches`/planner 或 VM 事务）。

## 反汇编环境

- capstone 5.0.9：`PYTHONPATH=/Users/tristanjen/Desktop/NootedRed/build/vcn/pylib`
- 切片（`/Users/tristanjen/Desktop/NootedRed/build/vcn/`）：`com_apple_kext_AMDRadeonX5000.kc.bin`、`com_apple_kext_AMDRadeonX5000HWLibs.kc.bin`、`com_apple_kext_AMDRadeonX5000HWServices.kc.bin`、`com_apple_kext_AMDRadeonX6000.kc.bin`
- `/Users/tristanjen/Desktop/NootedRed/tools/kernel_slice.py`：`SliceImage(path)`，`segments` 是 5 元组 `(name, vmaddr, vsize, fileoff, filesize)`，`symbols` 是 mangled → 地址
- 虚表条目是 chained fixup：真实地址 = 值 & 0xFFFFFFF，条目位置 = `__ZTV + 0x10 + X`；对象 vptr 指向 `__ZTV + 0x10`，所以实测差值 = 静态差值 + 16

## 代码地图

`NootedRed/VCN.cpp`（VCN1 主体：`prepareVCN1`、`hookCailLibrary`、`cailUvdControlHook`、`cailUvdInitHook`、`probeHardwareInit`）、`VCN.hpp`、`X5000.cpp`/`.hpp`（`allocateHWEngines` VCN1 分支、能力位）、`HWLibs.cpp`（钩 HWLibs 前调用 `VCN::hookCailLibrary`）、`UserVideoDecode.cpp`、`UserSequoiaVideoData.hpp`（生成物）、`VCNCapabilities.hpp`、`docs/vcn.md`、`docs/vcn1-analysis.md`（§1–§12 全过程记录）。

## 完整对话导出

`build/vcn/vcn1-transcript.md`（约 1.2MB，2607 段，按 seq 排序，可用 grep 检索细节；未提交到 git）。
原始事件流：`build/vcn/session.jsonl`（28MB）。
