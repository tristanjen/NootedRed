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
| 最新提交 | `dea7c38` VCN1: Identify the CAIL ASIC UVD init as Greenland and stub its MCIL wait；docs（本轮） |

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

## 当前阻塞点

`HardwareInitDec=0` / `ProbeInitResult=0`：引擎 `initHardware` 死在 `initHardwareCail`，CAIL 返回被归一化成 `0xE00002C7`。

已定位到：

1. `_CAILUvdControl`(0x47F05E7) 的能力闸门：Picasso 的 DDI 能力表 `_CAIL_DDI_CAPS_PICASSO_A0`(0x60F0440) word6=0x20000000、word10=0x02000120，bit22（0xD6）与 bit27（0x15B）都是 0 → 库层返回 1。
   现在在 `cailUvdControlHook` 里对位图 `adapter+0x358` 强制置位（`CailCapD6=1`、`CailCap15B=1`）。
2. 于是进入 `_CailUvdInit`(0x47F64BF)，门禁全过：版本 0x148、`[adapter+0xE48]`=0、mode=0、`[adapter+0x370]&4`=0、`[adapter+0x378]&0x48000`=0，
   然后 `call qword ptr [adapter+0x12D8]`（ASIC 专用 UVD init）**返回 1** —— 这就是现在的失败点。

`[adapter+0x12D8]` 已确定为 **`_greenland_uvd_init`(0x4851476)**：dump 的 `CailInitFunc=0x296476` 是相对
HWLibs `__TEXT` 基址 0x45BB000 的偏移，`0x45BB000 + 0x296476 = 0x4851476`；与 `_CailUvdInit` 的差值是
**0x5AFB7**。上一轮的五项差值表只列了大写 `*UvdInit`，漏掉了小写命名的 Greenland（Vega10 家族；Picasso 的
CAIL 能力位 0x148 已打开，`_Cail_Raven_InitFunctionPointer` 不覆盖这个槽）。

它返回 1 的路径：`[adapter+0x190]=0x8E`、能力 0x133/0x134=0 → `_uvd_init`(0x48410D8) → `_uvd_init_engine`(…,1)；
唯一失败点是 0x48416AB 的 `_Cail_MCILWaitFor(adapter, {reg=0x83AF, mask/value=2}, 1, 1, 1, 0xBB8, 4)`：等 UVD
寄存器 0x83AF 的 bit1。`_Cail_MCILWaitFor` 在 `[adapter+0x18]` 或它 +0x168 的回调为空时立即返回 1，否则
等待超时也返回 1。详见 `docs/vcn1-analysis.md` §13。

`CailInitDelta` 之前不写是因为 `cailUvdInitOriginal` 是 Lilu 的 trampoline（`routeFunctionInternal` 里
`*org = trampoline`），不是符号地址；本轮改用 `RouteRequest::from`。同时新增 `CailFamily`、`CailRevision`、
`CailMcilLow`、`CailMcilWaitLow`，并在真实 init 失败后把该槽换成返回 0 的桩重调 `_CailUvdInit`
（`CailInitFirstResult` 保留原始失败码，`CailStubResult`/`CailInitResult` 是桩后的结果）。

## 下一步

1. 安装 `build/ddr/Build/Products/Release/NootedRed.kext` 重启，读 `CailInitDelta`（应为 0x5AFB7）、
   `CailFamily`（应 0x8E）、`CailMcilLow`/`CailMcilWaitLow`、`CailInitFirstResult`、`CailStubResult`/`CailInitResult`。
2. 若 `CailStubResult=0`、`CailInitResult=0` 且 `CapUVD0`/`CapResult` 非零 → CAIL 链路已通，回到用户态验证硬解；
   若仍失败，按新的 `CapResult`/`NewCtxStatus` 继续。
3. 若 MCIL 回调为空，说明 CAIL 的等待本来就依赖未构造的宿主服务，桩是正确绕法；后续可把桩收窄到
   `_uvd_init_engine` 的那一次等待，保留其余寄存器初始化。

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
