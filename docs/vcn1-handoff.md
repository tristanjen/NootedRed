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
| 最新提交 | `690a266` VCN1: Keep the VCN power-gate flag cleared after the TF power-up；docs（本轮） |

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
- 内核 VCN1 初始化诊断：返回 0 的桩可让 `InitHardwareCail`/`HardwareInitDec=1`，但它跳过了
  UVD 寄存器初始化，解码提交会挂死 UVDDec（§17）；真实 init 只卡在 0x83AF idle 等待（§13）。
- 用户态 VA：共享缓存补丁 Applied（`VCNUserApplied=16`、`VCNUserD15=1`）、`VCNDecoderPID` 出现、
  `NewCtxStatus=0`、`UvdCtxInit=1`、`ReqCap=1`（2026-11-07 21:53 实机）。

## 当前阻塞点

**内核初始化**（2026-11-07 18:00/21:06 dump）：`[adapter+0x12D8]` 确认为 `_greenland_uvd_init`
(0x4851476)，失败点是 `_Cail_MCILWaitFor` 等 UVD 寄存器 0x83AF bit1；桩生效时
`HardwareInitDec=1`（§13）。

**用户态已打通**（2026-11-07 21:53 dump）：`VCNUserApplied=16`、`VCNUserD15=1`（Applied）、
`VCNVPFPath=1`、`VPFPlanState=2`（AllPatched）、`VCNDecoderPID=1027`、`VCNGVASelectionFix=1`；
GVA 日志不再报 `AVD_api.Create failed err=5`。VA 驱动已经发起 `NewCtxStatus=0`、`UvdCtxInit=1`、
`ReqCap=1`（mode 6）——只差引擎能力。

**当前阻塞（2026-11-07 22:34–22:38）**：装上带「重复 init 短路」的构建（UUID `FBEA6D26`）后，
登录约 55 秒系统冻结。gpuRestart 报告显示 Restart Channel 12 **UVDDec**，VTDecoderXPCService
PID 550 提交了 2 个命令缓冲、CompletedTS=0，AMD Cail 报 FenceNotRetired=1、FullAsicReset=1。
原因：返回 0 的 ASIC 桩跳过了 `_greenland_uvd_init` 的 UVD 寄存器初始化；短路让 initHardwareCail
返回成功，引擎 start 后 VA 驱动提交真实解码命令，UVD 块从未初始化 → 看门狗复位。
**不要安装 `FBEA6D26` 这个构建**；已回滚到 `E14758AF`（`HardwareInitDec=0`，无提交，稳定）。

安全诊断构建（UUID `8F47944A`，§17）实机稳定；读数 `CailMmioReadResolved=1`、
`CailUvdStatusBefore=0`、`CailUvdStatusAfter=0`、`CailInitCalls=6`——UVD 块**完全没上电**，
不是 MCIL 等待路径的问题。

根因（§18）：`HardwareSkipFirmware=1`，原生 `initHardware` 跳过 `loadUVDFW`，只跑
`initHardwareCail(+0x25D&1 = 1)`；固件只在 probe 的**事后**单独加载一次，此时 CAIL init 已经失败。
Linux `vcn_v1_0_wait_for_idle` 等的是 `mmUVD_STATUS == 0x2`；读到 0 表示电源/时钟/复位没打开。

固件先行诊断（UUID `898C4F45`，§18）10:08 实机稳定，但 `CailFirmwareFirst=0` 只是最后一次写入；
`LoadUVDFW=1` 也可能来自 probe 的事后加载。若 boot 时 load 失败，原生 `initHardware` 会在 CAIL
之前返回 false，导致「固件先行 → CAIL」从未真正验证。

固件加载已排除（10:22 dump，UUID `1BDAD9EC`）：`CailFirmwareFirstEver=1`、
`ProbeFirmwareFirst=1`、`LoadUVDFWFirst=1`、`LoadUVDFWSecond=1`，但
`CailUvdStatusFirstBefore/After=0`、`CailInitResult=1`。固件在 boot 与 probe 两条路径都加载成功，
UVD 仍然没上电——问题在电源域，不在固件顺序。

关键线索：dump 从不出现 `PowerDec`/`PowerEnc`，accelerator 根本没调用引擎的 `powerOnOff`。
反汇编 `AMDUVDHWEngine::powerOnOff`（0x43D152A）只有几条指令：取 `[self+0x18]` 的 CAIL 客户端，
固定发送 **selector 3**（忽略 `on` 参数）。但强制调用（`B4419BB1`，11-08 dump）返回
`PowerForced=0`、`PowerDec=2`，UVD_STATUS 仍为 0：selector 3 只是下游使能，不是电源域开关。

`AMDHardware::powerUpHWEngines` 遍历 `[self+0x3B8+i*8]` 的 11 个引擎并调用 `[engine_vt+0x138]`，
但我们的自定义引擎不在该数组；真正的电源域入口是 HWLibs 的 `PhwRaven_PowerUpVCN`（SMU
message 2 = PowerUpVcn）。`PHM_Initialize(pp,cfg)` 在 `pp+0x960` 分配 0x1AC0 字节的 PHM
扩展块，`+0x88`=SMC、`+0xA0`=ASIC 状态，该扩展块就是 `PhwRaven_*` 的 `phw` 参数。驱动自己的
`TF_PhwRaven_InitPowerGateState` 只有在 `[state+0x1A0]==0` 时才调 `PowerUpVCN`；Picasso 上很
可能被跳过（§21）。

**build35 不安全**（`0E74697C`，commit `428369b`）：它在 `PHM_Initialize` 返回后强制调用
`PhwRaven_PowerUpVCN`（SMU message 2 + SOC15 寄存器写），实机连续两次 panic：
11:11:53（启动后 43.1s）与 11:20:15（40.5s），均为 `TLB flush timeout`，CPU 7 卡在
`AMDRadeonX6000Framebuffer`。**不要再安装 build35。**

build36 只读观测版（`428430a`，CDHash `38DA3C47`）实机稳定，dump 确认：
`InitPowerGateSeen=1`、`InitPowerGate1A0Before/After=1`、`InitPowerGate190Before/After=1`、
`InitPowerGateResult=1`——驱动自己的 `TF_PhwRaven_InitPowerGateState` 运行了，但因为
`[state+0x1A0]==1` **主动跳过** `PowerUpVCN`；VCN 一直处于门控状态。
`CailUvdStatusBefore/After=0`、`CailInitResult=1`。

**修正**：实际读取的注册表键名是 **`CAIL_PP_EnableVCNPG`**（`MCIL_GetRegistrykey` 会拼
`CAIL_` 前缀），不是 `PP_EnableVCNPG`。它在 iGPU 的 `IOPCIDevice`（ioreg 的 `IGPU@0`）上，
和现有的 `CAIL_DisableGfxCGPowerGating` 在同一个字典。`getOSObjectData` 支持 OSNumber，
所以 `<integer>0</integer>` 或 4 字节 data 都可以。

build37 实测（`A73797F8`）：属性确实出现在 `IGPU@0` 的 ioreg 里
（`CAIL_PP_EnableVCNPG=0`，`cail_properties` 字典也有），但
`InitPowerGate1A0Before/After` 仍然是 1——驱动没有走 `PowerUpVCN`。说明注入成功，但
`PhwRaven_Initialize` 读取的时机/对象与这份属性不一致。

build38 实测（`6BAB212F`）：`InitPowerGate1A0Cleared=1`、`190After=0`，驱动自己的
`PowerUpVCN` 成功了；但 `PhmState190AtHardware=1`——TF 返回后把 `[0x1A0]` 恢复成 1，
PSM 的 `PowerGateVCN(phm,1)` 在 engine init 前又把 VCN 关掉。`CailUvdStatusBefore=0`。

build39（`690a266`，CDHash `EB3848FB`）：默认**不恢复** `[0x1A0]`，留在 0，
`PowerDownVCN` 开头的 `cmp [0x1A0],0` 直接跳过，VCN 保持上电；`-NRedVCNRestorePG`
恢复 build38 行为。预期 `PhmState190AtHardware=0`、`CailUvdStatusBefore=0x2`、
`CailInitResult=0`。

**build39 不安全**（UUID `9CFD1B1F`）：实机 panic，栈为 `_ulReadMmRegisterUlong`
→ `_uvd_init_engine`（等 0x83AF）→ `_CailUvdInit` → NootedRed `probeHardwareInit`
→ `accelVideoContextStart`；触发进程 `avconferenced`。VCN 被强行保持上电，但 PSM/时钟
没有把它带到 idle，CAIL 等待变成自旋。EFI 已回滚到 stock 构建（`30579C7E`），系统稳定。

build36 设计：只路由 `_TF_PhwRaven_InitPowerGateState`，**不**路由 `_PHM_Initialize`、
**不**调用 `PhwRaven_PowerUpVCN`、不写任何寄存器。保留 engine `powerOnOff(true)` 诊断和
no-start 闸。

`CapUVD0`/`CapResult=0` 是 init 失败的后果：查询已经到达内核并写入了属性，但引擎没 start，能力为 0。

## 下一步

1. **不要再装 build39**；EFI 现在是 stock 构建（UUID `30579C7E`），系统稳定。
2. 方向调整：不再从 PHM 强行上电。先做无写入的 PSM 观测：
   - `PSM_UseAsicBlock` / `PSM_UnuseAsicBlock`（UVD block 注册）
   - `PSM_ActivateClient` / `PSM_UpdateClockRequest`（视频客户端激活/时钟请求）
   - `PHM_PowerGateVCN` / `PhwRaven_PowerUpVCN`（PSM 何时调用）
   - `PhwRaven_SetDPGModeVCN` 与 VCN 时钟路径
3. probe 默认关闭（`-NRedVCNProbe` 才启用），避免视频上下文启动时提前跑 CAIL init。
4. 找到 PSM 激活 UVD block 的入口后，让自定义引擎走同一路径，而不是直接发 SMU。

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
