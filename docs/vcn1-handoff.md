# VCN1 / Picasso handoff

Ryzen 5 3500U（Picasso，iGPU `1002:15D8`，VCN 1.0）在 macOS 15.8.1 (24H32) 上补**硬件视频解码**。
当前状态：用户态补丁与视频上下文链已打通，硬件侧仍卡在 VCN 上电（见 `vcn1-analysis.md` §37）。
硬编尽力而为（HEVC 可期，AVC 无专用命令类）。

## 仓库与分支

| 项 | 值 |
|---|---|
| fork | https://github.com/tristanjen/NootedRed |
| 分支 | `vcn1-upstream`（基于 upstream/master `690e6c5` 重放 VCN1 增量） |
| 主检出 | `/Users/tristanjen/Desktop/NootedRed`（含切片与 capstone） |
| 工作树 | `/Users/tristanjen/Desktop/NootedRed/build/up-tree` ← 在这里改代码/构建 |
| 最新提交 | 代码与 docs 见 `git log -1`（build48/build49 PSM 分类探测与分类 4 兜底） |

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
没有把它带到 idle，CAIL 等待变成自旋。

build40（`14c078b`，CDHash `C29F2533`，已安装）：`probeHardwareInit` 默认关闭
（`-NRedVCNProbe` 才启用）；TF hook 只读（不写 `[0x1A0]`、不提前上电）；不再默认强制
engine `powerOnOff`（`-NRedVCNForcePower` 才启用）。新增 `PowerOnOffCalls`/`LastOn`/
`LastResult` 和 `CailFunc1..5Calls/Result`，用来观察 accelerator 是否调用 powerOnOff、
CAIL func 3 是否成功。系统应稳定。

build40 实测（2026-11-08 12:56 dump）：`PowerOnOffCalls` **不出现**——反汇编 UVD7 虚表
槽 +0x138（`__ZTV30AMDRadeonX5000_AMDUVD7HWEngine`）指向 0x43d2b24，只是一个
`mov al,1; ret` 的桩；UVD 基类的 `powerOnOff`（0x43d1538）在 UVD7 上被覆盖，所以钩子
永远不会被调用。`CailFunc1Calls=4` 全部 `Result=1`，`CailFunc2..5` 从未调用；
`CailLastSelector=1`、`CailLastResult=0xFFFFFFFFE00002C7`，`CailSubmit8=0`、
`CailSubmit9=0xFFFFFFFFE00002C7`。`InitPowerGate1A0Before/After=1`、
`PhmState190AtHardware=1`、`InitHardwareCail=0`、`HardwareInitDec=0`、`StartDec=0`。
即：CAIL init 仍因 UVD 块未上电而失败，且 accelerator 的 powerUpHWEngines 即使调
UVD7 的 powerOnOff 也只是空桩。

根因确认：`CAIL_PP_EnableVCNPG=0` 虽然以**扁平属性**出现在 IGPU（IOPCIDevice）上，
驱动读到的仍是默认值 1。`PECI_ReadRegistry(handle,"PP_EnableVCNPG",&out,1)` 的返回值
决定 `[state+0x1A0]`，而 build37 里它走了「未找到→写默认值」分支。

build41（`a5a2085`，CDHash `37DF0FD4`，UUID `90A2B51D`）：路由 `_PECI_ReadRegistry`，对
key=`PP_EnableVCNPG` 的读取强制写回 0，让 `PhwRaven_Initialize` 把 `[0x1A0]` 置 0；
`TF_PhwRaven_InitPowerGateState` 随之在引擎 init 前走 `PowerUpVCN`，且 `[0x1A0]=0`
会让之后的 `PowerDownVCN` 直接返回（VCN 在 PSM 管辖外持续上电）。新增探针：
`PeciVCNPGCalls/Raw/Result/Forced`、`RavenPowerUpCalls/Result/190Before/190After/1A0`、
`RavenPowerDownCalls/...`、`PhmPowerGateVCNCalls/LastOn/Result/190Before/190After`、
`InitPowerGateCalls`、`InitHardwareCailRaw`（门控前真实结果）。probe 仍默认关闭。

**build41 不安全（2026-11-08 实机）**：13:07 安装，13:14:53 冻屏后 panic，uptime 45.6s。
panic 为 `spinlock acquisition timeout`（CPU 4 的 fseventsd 线程在 VFS 路径上持锁阻塞），
栈里没有 NootedRed / AMD 帧；但 40–46 秒的时间点与 build35（VCN 持续上电后 TLB flush
超时 panic）同类，且 build41 是该次启动唯一增量。**不要再安装 build41。**
机器已回滚到 build40（UUID `FEB0F272`）运行。

build42（`66c842a`，CDHash `955FE564`，UUID `00A6452E`，默认安全观测版）：把 build41 的 PECI 强制改为 `-NRedVCNForceVCNPG0` 门控
（默认关，放行原值），默认行为回到 build40；保留 PowerUp/PowerDown/PHM 门控的只读计数；
新增 `_PSM_UseAsicBlock`、`_PSM_ReArbit`、`_PHM_PowerGateUVD` 观测与
`UnsafePoweredAtInit` 安全标记（`[state+0x190]==0` 时置 1，提示 VCN 被留在上电状态）。
关键静态事实：`PSM_UseAsicBlock` 的分发目标 `_PhwRaven_SetAsicBlockGating`（0x47bab44）
只是 `mov eax,1; ret` 空桩，Raven 上真正调用 `PHM_PowerGateVCN` 的只有
`PSM_ReArbit`（[psm+0x5f0]/[psm+0x690] 控制）；所以两条都要看。
判据：`PsmUseAsicBlockBlockMask` 的 bit 2/3/6、`PsmReArbitGateDelta>0`、
`PhmPowerGateVCN/UVD` 的 `LastOn=0`，再决定在哪里安全地重跑引擎 init。

build42b 实测（2026-11-08 dump）：安全默认成立——`InitPowerGate1A0Before/After=1`、
`PhmState190AtHardware=1`、`UnsafePoweredAtInit=0`、`PeciVCNPGCalls=1`/`Raw=1`/`Result=1`
（证实驱动读到的是**默认值 1**，personality 里的扁平属性并没有被 `getProperty` 命中）、
`PeciVCNPGForced` 不出现。PSM 侧：`PsmReArbitCalls=7`、`PsmReArbitGateDelta=2`，
`PhmPowerGateVCNCalls=7`/`LastOn=1`、`PhmPowerGateUVDCalls=7`/`LastOn=1`，
`RavenPowerDownCalls=8` 全部空转（`190` 前后都是 1），`RavenPowerUpCalls` 不出现；
`PsmUseAsicBlock*` 不出现（从未调用）。即 PSM 每轮仲裁都在**关门**，
两个“应开”标志 `[psm+0x5F0]`/`[psm+0x690]` 始终为 0——视频栈没有向 PSM 申请媒体 block。

build43（`5777f9f`，CDHash `15BFC5D8`，UUID `028269CA`）：在原生 initHardware 失败后做一次
短促电源脉冲（只解码引擎、只一次）：临时清 `[0x1A0]` → `PhwRaven_PowerUpVCN`（完整路径：
msg 0xC + 0xC0/0xC1 寄存器写）→ 读 `UVD_STATUS(0x83AF)` 最多 3 次（间隔 1ms）→
恢复 `[0x1A0]` → `PhwRaven_PowerDownVCN` 关回去。窗口只有几毫秒，不跑 CAIL init。
同时新增 `_PSM_ActivateClient`、`_PEM_Task_VideoStartMultiUVDStates` 观测与 `PsmHandle`。
探针读数：`PowerProbeRun/UpResult/DPG/190AfterUp/Status/DownResult/190AfterDown`。

build43 实测（2026-11-08 dump）：脉冲机制成立且系统稳定——`PowerProbeRun=1`、
`PowerProbeUpResult=1`、`PowerProbe190AfterUp=0`、`PowerProbeDownResult=1`、
`PowerProbe190AfterDown=1`、`PowerProbeDPG=0`；但 `PowerProbeAdapter=0`，
状态读没有执行（adapter 取错：CAIL client 在 `[engine+0x18]` 虚表 +0x2B8 的 getter 后面）。
PSM：`PsmActivateClientCalls=6`、`Last=3`、`Arg2=1`、`Arg3=0`、`Result=1`，
`PemVideoStartCalls` 不出现——有客户端激活，但 PEM 的视频启动任务从未执行，
PSM 的 VCN 开窗标志仍是 0。

build44（`0e47a32`，CDHash `A9936CCB`，UUID `D3B887A1`）：修正探针的 adapter 解析
（`cailClient`，或 `[engine+0x18]` 虚表 +0x2B8 的 getter）后在同一个电源窗口里条件初始化：
只有当 `UVD_STATUS` bit1（0x2，idle）置位时才调用原生 `initHardware`（`0x25D=1`
跳过固件，只走 CAIL init），随后立刻关回。新增 `PowerProbeAdapter/WindowInit/StatusAfterInit`，
并记录 `PSM_ActivateClient` 的调用者地址（client 3 单独记录）与第一次 `PSM_ReArbit` 调用者。

build44 实测（2026-11-08 14:30 dump）：`PowerProbeAdapter=1`（adapter 修好了）、
`PowerProbeUpResult=1`、`PowerProbe190AfterUp=0`、`PowerProbeDownResult=1`、
`PowerProbe190AfterDown=1`，但 **`PowerProbeStatus=0`**，`WindowInit` 不出现。
结论：`PowerUpVCN` 能把电源域打开，但 UVD 块不会 idle——固件必须在**上电状态下**装载，
只在 gated 状态下装载（首次 initHardware 的老路径）时不会真正启动。PSM 侧不变
（`ActivateClientCalls=6`/`Last=3`、`PemVideoStartCalls` 不出现）。

build45（`16c6574`，CDHash `217B04E3`，UUID `44B5526C`）：初始化电源窗口。
`initPowerGateHook` 在第一次 TF 调用前把 `[state+0x1A0]` 临时清 0（TF 于是走完整的
`PowerUpVCN`：SMU msg 2 + msg 0xC + 0xC0/0xC1 寄存器写），并**保持**到解码引擎第一次
`initHardware`（`loadUVDFW` + `initHardwareCail`）结束时，才恢复 `[0x1A0]` 并调用
`PhwRaven_PowerDownVCN` 立即关回。若 TF 到达时引擎 init 已经跑过（`uvd7InitHardwareCount!=0`）
则不开窗（`InitWindowSkipped=1`）。旧的电源脉冲探针改为只在 `-NRedVCNPowerProbe` 时运行。
新增读数：`InitWindowOpened/Closed/Skipped/Saved1A0/DownResult/190AfterDown/1A0AfterDown`。

**build45 实机冻屏/panic（14:45，`Kernel-2026-11-08-144526.panic`，uptime 155.6s）**：
`NMIPI for spinlock acquisition timeout`，锁 `0xffffff80171b1d88`（内核数据）；CPU 6 的栈是
定时器中断（`lapic_interrupt → rtclock_intr → timer_intr → timer_queue_expire_with_options
→ thread_timer_expire → thread_setrun`），被中断上下文是 **VoodooI2C**（boot args 含
`-vi2c-force-polling`）；CPU 4（imagent）等同一把锁。**整个 panic 文件没有 NootedRed/AMD 帧**。
即：不能把这次 panic 直接归因于 VCN，但 build45 的 TF 窗口存在“TF 已上电、引擎 init 未执行”
时 `[0x1A0]` 停在 0、VCN 长期上电的泄漏路径（build35/build41 的不稳定状态），机制必须移除。
机器已回滚到 build44 运行。

build46（`240f2c1`，CDHash `D8326E76`，UUID `85F706FB`，默认安全版）：
`initPowerGateHook` 恢复只读（不写 `[0x1A0]`/`[0x190]`，不再打开 TF 窗口）；初始化电源窗口
改为**自包含**地放在 `uvd7InitHardware` 内部，只有显式传 `-NRedVCNInitWindow` 才启用：
保存 `[0x1A0]` → 清 0 → `PhwRaven_PowerUpVCN`（完整路径）→ `loadUVDFW` +
`initHardwareCail` → 无论结果如何立即恢复 `[0x1A0]` 并 `PhwRaven_PowerDownVCN` 关回。
窗口只存在于这一次函数调用内，不存在 TF 打开后泄漏的问题。另在 `accelVideoContextStart`
加兜底：若窗口仍打开则强制关闭并发布 `InitWindowForcedClosed=1`。默认（无 boot-arg）行为
与 build44 完全一致。

**build46 窗口实验（2026-11-08 15:03，`-NRedVCNInitWindow`）不安全**：
启动后约 47.3 秒、登录进桌面几秒后冻屏并 panic（panic 文件
`Kernel-2026-11-08-150645.panic`，写在下次启动时）。`NMIPI for unresponsive processor:
TLB flush timeout`；RIP `0xffffff7fa14d1abf` = `AMDRadeonX6000Framebuffer` +
`0x1cabf` = `AMDRadeonX6000_AmdRegisterAccess::readValidateReg32(unsigned int)+0x8b`。
这与 build35 的 11:11:53 panic（RIP `0xffffff7f9d2d1abf`，同样 +`0x1cabf`，43.1 秒）
是**同一条指令、同一类失败**；panic 前一条指令是 `mov edx, [rcx+rax]`（MMIO 寄存器读）。
实验没有拿到 ioreg：窗口很可能卡在寄存器读里，`initHardware` 没返回、
`closeInitWindow` 没执行。**不要重试这个窗口，也不要默认打开。**
panic 后 EFI 已恢复为 stock NootedRed（`30579C7E`），`config.plist` 已从
`config.plist.bak-20261108-150146` 恢复（`-NRedVCNInitWindow` 已移除）。

build47（在 stock 期间准备，CDHash `DBBC2952`，UUID `E5F48284`）：
默认路径只解析 PSM 状态机入口（`_PSM_GetStateByClassification`、`_PSM_SetUvdState`、
`_PSM_AdjustPowerState`、`_PSM_UnuseAsicBlock`、`_PSM_GetAsicBlockStatus`），并在
`PSM_ReArbit`/`PSM_ActivateClient` 钩子里捕获 `psm` 句柄；不改变任何电源行为。
可选唤醒 `-NRedVCNPsmWake`（默认关）在 `Vega10VideoContext::contextStart` 返回成功且
UVD0 队列非空后执行一次 PSM 序列：`GetStateByClassification(0x12)` → `SetUvdState` →
`AdjustPowerState` → `UseAsicBlock(3)`；失败立即 `UnuseAsicBlock(3)`，客户端 stop 时释放。
不写 `[0x1A0]`/`[0x190]`，不调用 `PhwRaven_PowerUpVCN/PowerDownVCN`。完整设计与判读见
`docs/vcn1-psm-plan.md`。

**build47 首轮唤醒实测（2026-11-08 15:49，`-NRedVCNPsmWake`）**：
`PsmWakeRun=1`、`PsmWakeSymbols=31`、`PsmWake190Before=1`、`PsmWakeFlag5F0/690Before=0`，
但 **`PsmWakeGetStateResult=15`**（0xF = 状态不存在）、`PsmWakeStateIndex=0`、
`PsmWakeFailed=1`；后续 SetUvd/Adjust/UseBlock 未执行，PSM 未上电，系统稳定。
0xF 来自 `PSM_GetStateByClassification_Legacy`：`PSM_SearchForState` 找不到分类 0x12，
回退枚举全部状态后仍找不到。结论：**当前 PSM 状态表没有分类 0x12 的 UVD 状态**，
不是调用顺序问题。

build48（CDHash `A5E04CEB`，UUID `A0F67623`）：在唤醒前先调用
`PSM_ListPresentClassifications` 枚举状态表实际存在的分类位（`PsmWakeClassesMask`），
并依次探测 0x12/0x13/0x14/0x11 四个 UVD 分类（`PsmWakeClass12/13/14/11Result`），
取第一个存在的分类和对应 block（0x12→3、0x13→4、0x14→5、0x11→2），再执行
SetUvdX/AdjustPowerState/UseAsicBlock。仍不写 `[0x1A0]`/`[0x190]`、不直接调
`PhwRaven_PowerUpVCN`。

**build48 分类探测实测（2026-11-08 21:33，`-NRedVCNPsmWake`）**：
`PsmWakeClassesMask=49`（0x31，只有分类 **0/4/5** 存在）；0x12/0x13/0x14/0x11 四个
UVD 分类全部返回 **15**，`PsmWakeNoState=1`、`PsmWakeFailed=1`，未上电，系统稳定。
即 **当前 PPT/PSM 状态表没有任何 UVD 分类**，不是换分类就能解决的。

build49（CDHash `4370048A`，UUID `9BA1FB25`）：四个 UVD 分类都不存在时，按
`PEM_CWDDEPM_PredictUVDStateSettings` 的默认基状态改用分类 4（性能状态）：
`GetStateByClassification(4)` → `SetUvdState(idx4)` → `AdjustPowerState` →
`UseAsicBlock(3)`。发布 `PsmWakeClass4Result/Index`、`PsmWakeFallbackClass4`。
仍不写 `[0x1A0]`/`[0x190]`、不直接调 `PhwRaven_PowerUpVCN`。

build36 设计：只路由 `_TF_PhwRaven_InitPowerGateState`，**不**路由 `_PHM_Initialize`、
**不**调用 `PhwRaven_PowerUpVCN`、不写任何寄存器。保留 engine `powerOnOff(true)` 诊断和
no-start 闸。

`CapUVD0`/`CapResult=0` 是 init 失败的后果：查询已经到达内核并写入了属性，但引擎没 start，能力为 0。

## 下一步

1. **build49 分类 4 兜底实测失败**（2026-11-08 21:48）：`PsmWakeClass4Result/Index=1/1`、
   `PsmWakeFallbackClass4=1`，SetUvd/Adjust/UseBlock 全部返回 1，但
   `PsmWakeFlag5F0/690After=0`、`PsmWakeVcnGateDelta=0`、`PsmWake190After=1`，
   随后 `PsmWakeUnuseResult=1`、`PsmWakeFailed=1`。**分类 4 的 state 没有 VCN 时钟属性，
   PSM 分类路线到此为止**，不要再试 0x11–0x14 或分类 4。
2. **先恢复 config.plist**：本机启动仍带 `-NRedVCNPsmWake`（`PsmWakeRun=1` 可证）。
   挂载 ESP 后用 `~/Desktop/config.plist.bak-20261108-214330` 覆盖
   `EFI/OC/config.plist`，确认 boot-args 不再含该开关。
3. **补固件路径诊断**：`LoadUVDFW=1`、`FirmwareAddress=0x7B52F000` 非零，但 `FwMsg*`
   全缺。下一版在 `prepareVCN1` 发布 `FwMsgRouteInstalled`，并在 `loadUVDFW` 入口读
   `+0x3BA` 缓存标志、发布提前返回位，先区分“没下发”和“没钩上”。
4. **转向 CWDDEPM 建状态**：查 `PEM_CWDDEPM_PredictUVDStateSettings` 的 UVD 状态创建
   /`PSM_AddState`，给状态表补一个带 VCN 时钟/门控属性的 UVD state；或直接驱动
   `PEM_Task_VideoStartMultiUVDStates`（`PemVideoHooked=1`，但本机从未被调用）。
   不要退回裸调 `PowerUpVCN`。
5. **禁止** build35/39/41/45、`-NRedVCNInitWindow`、`-NRedVCNForceVCNPG0`。
6. 任何新实验仍必须：boot-arg 默认关、备份 `config.plist`、装前备份 EFI kext、失败后
   先恢复 EFI/配置再重启。

## 反汇编环境

- capstone 5.0.9：`PYTHONPATH=/Users/tristanjen/Desktop/NootedRed/build/vcn/pylib`
- 切片（`/Users/tristanjen/Desktop/NootedRed/build/vcn/`）：`com_apple_kext_AMDRadeonX5000.kc.bin`、`com_apple_kext_AMDRadeonX5000HWLibs.kc.bin`、`com_apple_kext_AMDRadeonX5000HWServices.kc.bin`、`com_apple_kext_AMDRadeonX6000.kc.bin`
- `/Users/tristanjen/Desktop/NootedRed/tools/kernel_slice.py`：`SliceImage(path)`，`segments` 是 5 元组 `(name, vmaddr, vsize, fileoff, filesize)`，`symbols` 是 mangled → 地址
- 虚表条目是 chained fixup：真实地址 = 值 & 0xFFFFFFF，条目位置 = `__ZTV + 0x10 + X`；对象 vptr 指向 `__ZTV + 0x10`，所以实测差值 = 静态差值 + 16

## 代码地图

`NootedRed/VCN.cpp`（VCN1 主体：`prepareVCN1`、`hookCailLibrary`、`cailUvdControlHook`、`cailUvdInitHook`、`probeHardwareInit`）、`VCN.hpp`、`X5000.cpp`/`.hpp`（`allocateHWEngines` VCN1 分支、能力位）、`HWLibs.cpp`（钩 HWLibs 前调用 `VCN::hookCailLibrary`）、`UserVideoDecode.cpp`、`UserSequoiaVideoData.hpp`（生成物）、`VCNCapabilities.hpp`、`docs/vcn.md`、`docs/vcn1-analysis.md`（§1–§37 全过程记录）。

## 完整对话导出

`build/vcn/vcn1-transcript.md`（约 1.2MB，2607 段，按 seq 排序，可用 grep 检索细节；未提交到 git）。
原始事件流：`build/vcn/session.jsonl`（28MB）。
