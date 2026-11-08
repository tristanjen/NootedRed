# VCN1（Raven/Picasso）接入分析 — 逆向结论

目标：Ryzen 5 3500U（Picasso `1002:15D8`，VCN 1.0，gfx902）在 macOS 15.8.1 (24H32)
上启用硬件视频解码（H.264/HEVC）与尽力而为的硬件编码。全部结论来自对实机
`AMDRadeonX5000`/`AMDRadeonX6000`/`AMDRadeonX5000HWLibs` 内核集合切片与
`AMDRadeonVADriver2` 共享缓存映像的离线反汇编（工具：`tools/audit_vcn1_driver.py`、
`tools/kernel_slice.py`、`tools/disasm_slice.py`）。

## 1. 用户态（AMDRadeonVADriver2，共享缓存 UUID 42ec2890…）

### identify() 的 PCI→VA 家族表（448 字节函数，0x5A 处读全局 16 位设备号）

| family | 设备 | GPU | 视频引擎 |
| --- | --- | --- | --- |
| 0 | 0x67C0-0x67DF 掩码 | VI dGPU | UVD6 |
| 1 | 0x67E0-0x67FF 掩码 | VI dGPU | UVD6 |
| 2 | 0x6860-0x687F 掩码 | Vega 10 | UVD7 + VCE4 |
| 3 | 0x69A0-0x69AF 掩码 | Vega 12 | UVD7 + VCE4 |
| **4** | **0x66A0-0x66AF 掩码** | **Vega 20** | **VCN 1.0** |
| 5 | 0x7310-0x7347 掩码 | Navi 10 | VCN2 |
| 6 | 0x7360 | Navi 10 变体 | VCN2 |
| 7 | 0x7340-0x7347 | Navi 14 | VCN2 |
| 8/9/10 | 0x73A0+/0x73C0+/0x73E0+ | Navi 2x | VCN3 |
| 11 | 其他 | — | 未知 |

**15D8 应映射到 family 4（Vega 20）**，与 fork 把 Renoir/Cezanne 映射到 5（Navi）同构。
生成器 `identify_payload()`（tools/build_video_decode_patch.py）在 0x5A 处插入
`cmpw $device; jne 原链; push $family`，直接适用（15.8.1 与 24H32 档案 16/16 函数
hash 匹配，作者静态数据可复用）。

### hwConfig（getExtraIdInfo 按 family 0..10 填充 `_VAHWConfig`）

- `+0x08` = deviceId；`+0x0C`(dword) = **视频引擎代际码**；`+0x94` = 解码能力位；
  `+0x96`(byte) bit0 = 走 VCN(UVE) 编码而非 VCE。
- 引擎代际码（createUVDCommand 跳表 0..4 + createEncoder/createEncodeProcessor 佐证）：
  `0=VI(UVD6/ViUvdCommand) 1=UVD7(Uvd7Command) 2=UVD7v2(=VCN1, Uvd7v2Command) 3=VCN2(Vcn2DecCommand/Vcn2Encoder) 4=VCN3(Vcn3DecCommand/Vcn3Encoder)`。
- 已核实的某个 case 写入 `+0xC=2` 且 `+0x94=0xAD76|cond<<7`、`+0x96` 高字节 0xAD
  （bit0=1，UVE 可用）——与 VCN1 家族的组合一致（跳表精确映射待 Mac 侧 ripData 复核）。
- 工厂分派：
  - `createDecoder(type)` → VAUvdDecoder(0x140)/VAVcnDecoder，解码 caps 位 `+0x94 bit0x10` 决定子类型；
  - `createUVDCommand(ctx)` 按 `+0xC` 跳表 → 5 个命令类（见上）；
  - `createEncoder(type 0xB=AVC/0xE=HEVC)`：`+0x96 bit0` 置位 → VAUveEncoder(0x538)
    + 内嵌 Vcn2/Vcn3Encoder（`+0xC==3` 选 2 否则 3）；清零 → VAAvcEncoder(0xDC0)/
    VAHevcEncoder(0xDD0)（VCE 系）；`createEncodeProcessor` 同构。
  - **VCN1 编码路径**：`UvdEncCommand`（`_HevcEnc*` 结构，UVD 式编码包）+
    `AvdUveAvc/HevcHeaderEncode`、`AVDUve*DataProcessor` 存在；AVC 的 VCN1 专用命令类
    不存在 → HEVC 硬编可期，AVC 硬编大概率仅 VCE（APU 无 VCE）→ 不可用。
- `UvdEngine::startEngine/createUvdContext` 是 IOConnect 包装：
  selector **0x104**（创建视频上下文，输入 0x50 字节）、**0x106**（启动引擎），
  经 `context+0x4B0` 的 connect（accelerator shared user client）。

## 2. 内核（X5000 内核集合切片，Darwin 24.6.0/15.8.1）

### 引擎数组（AMDHardware 内，基址 +0x3B8，槽间距 8）

`AMDHWEngineType`：`0=PM4 1=SDMA0 2=SDMA1 3=SDMA2 4=SDMA3 5=UVD0 6=UVD1 7=VCE 8=VCN0 9=VCN1 10=SAMU`。

`AMDVega20Hardware::allocateHWEngines`（0x43C7EA0，0xE2 字节）分配：
`+0x3B8=GFX9PM4Engine(0x340)`，`+0x3C0/+0x3C8=GFX9SDMAEngine(0x250)×2`，
**`+0x3E0=AMDUVD7v2HWEngine(0x438)`（UVD0 槽=解码），
`+0x3E8=AMDUVD7v2HWEngine(0x438)`（UVD1 槽=编码）**，
`+0x3F0=VCE4v1HWEngine(0x3A0)`。

### AMDUVD7v2HWEngine（VCN1 引擎，0x438 字节）

- `init(interface, engineType)`（0x43D4FB8）：
  - 校验 engineType∈{5,6}；`ebx = (engineType==6)`（编码标志）；
  - `hw = iface->vt[+0x2C0](iface)`；`hw->vt[+0x58](hw, 0xC, isEncode, 1, &base)`
    —— **SML 查询选择子 0x0C 返回 VCN 寄存器基址**（写入引擎 `+0x250`）；
  - 调 `UVDHWEngine::init` 基类（`__ZTV…UVDHWEngine+0x128`）；随后
    `+0x210=2`、`+0x404=0xA`、`+0x3B8(u16)=0x100`、`+0x25C(u16)=0x101`、
    `+0x250=base`、`+0x254=isEncode`、`+0x434=0`。
- `allocateHWRings`：3 个 0xE8 ring（Dec/Enc/EncLLQ，存 +0x50/+0x58/+0x60，计数 +0x48）。
- `allocateHWChannels`：0x278 通道，按引擎 `+0xC`（engineType）分支 Dec(5)/Enc(6) 通道。
- `initHardware`（0x43D527A）：`+0x10` 非 0 直接成功；否则 `+0x25D==0` 时调 `vt[+0x338]()`（loadUVDFW），再由 `vt[+0x390](+0x25D&1)` 处理 CAIL；
  成功后就 iface `vt[+0x128](base+0x500, 1)` 与 `(2*base+0x500, 2)` 两次
  （中断/事件路由，base=引擎+0x250）。
- 同伴类：`UVD7v2Dec/EncRing`、`UVD7v2Dec/EncChannel`、`UVD7v2EncLLQRing/Channel`；
  `AMDAccelUVDContext`（IOConnect 0x104/0x106 的内核侧：requestCapability、
  saveUVDSession/restoreUVDSession、setClocks、PM client）；
  `AMDVega10VideoContext`、`AMDAccelVideoContext`、`AMDGFX9GraphicsAccelerator::newVideoContext`。

### 与 fork 现状的关系

- fork 的 VCN2 方案：可选加载 X6000，把 `AMDVCN2HWEngine` 嫁接进 X5000 的
  `kAMDHWEngineTypeVCN0`(8) 槽 + 跨驱动类桥接（safeCast/ClientStart/SurfaceCopy/SML）。
- **VCN1 方案无需跨驱动**：全部类原生在 X5000。要点：
  1. `X5000::allocateHWEngines` 为 VCN1 设备加配 **一个** `AMDUVD7HWEngine`
     （槽 5，即 `pm4EngineField+5*8`，见 §6），并 `hasUVD0Field=true`——**不是** hasVCN0；
  2. X5000 原生 `newVideoContext`/AccelShared/AccelUVDContext 路径保持原生；
  3. HWLibs/SML 查询 0x0C 需对 APU 返回 Raven VCN 基址（fork 已 hook
     `IAMDSMLInterface::createSMLInterface`，注入点现成；具体 SML 方法 0x58 的
     实现在 X5000HWLibs 切片待解）；
  4. 固件已就绪：`ativvaxy_rv.dat`（AMD Adrenaline 26.5.2，VCN1，ipVersion 0x0100）
     已打包并经 `wrapGetIpFw` 按名分发；Linux raven/picasso/raven2 VCN 固件三同源。

## 3. 已核实/待办

- 已核实：15.8.1 与作者档案逐函数匹配；用户态 15D8 补丁实机事务通过；VCN1 原生引擎类是
  Vega10 世代的 `AMDUVD7HWEngine`（见 §6），实机已入槽。
- 待办：`loadUVDFW`/`initHardwareCail` 失败子步骤定位（诊断已上线）；
  IOConnect 0x104/0x106 视频上下文与解码会话的端到端验收；
  Chrome/长时稳定性与 FairPlay。

## 4. 实机首轮验收发现（15.8.1，2026-10-30）

- 用户态全链路通过：`DynamicMatched/DynamicApplied=1`、`UserApplied=16`、`DecoderPID` 与
  `GVASelectionFix=1`——15D8→family 4 补丁在真实进程事务中工作。
- `NRedVCN1Prepared/EnginesAllocated=1`；`NRedVCN1LastHardwareInit=0`（initHardware 被调但失败）。
- **`notifyGfxAccess` 符号前缀**：Sequoia 24H32 上是 `__ZN30…`（长度 30），fork 原代码的
  `__ZN29…` resolve 失败并毒化整个 `ok`，导致 X5000 五条路由（acceleratorStart/newShared/
  newClient/newVideo/createSML）全部未装（`NRedVCNX5000Prepared=0`）。已改为 30→29 双尝试。
- **引擎初始化流程**：`AMDHardware::initializeHWEngines` 按槽 0..10 调
  `engine->init(engine, hardware, slotIndex)`；UVD7v2::init 内
  `hw = hardware->vt[+0x2C0]()`（返回 hardware+0x338 的 SML 成员）后
  `hw->vt[+0x58](hw, 0xC, isEncode, 1, &base)`。
- **SML 实例类**：原生 `IAMDSMLInterface::createSMLInterface(config)`——config 掩码
  `0xFFF8FFFF` 为 0 时创建 `AMDSMLGFX9Interface`，否则创建基类 `AMDSMLInterface`。
  两张虚表的 +0x58 都指向 AMDSupport 里的同一原生查询；只挂 GFX9 表会被基类实例绕过
  （实机 `SMLBaseServed` 缺失即此因）。现在两张表都接管：先观察原生返回再固定应答 0x7800。
- `initHardware` 的两个子步骤已分别挂钩诊断：`loadUVDFW`（SML/PSP 固件下载）与
  `initHardwareCail`（CAIL 寄存器初始化）；引擎 init 成功后的 `+0x250` 基址与
  `+0x254` isEncode 也会发布（`NRedVCN1Engine*`）。下一轮 ioreg 将区分失败子步骤。

## 5. 第二轮实机与运行期接管（15.8.1，2026-10-31）

- 第二轮 ioreg：`X5000Prepared=1`、`NativeLoadReady=1`、`AcceleratorStarted=1`、`SMLGFX9Hooked/BaseHooked=1`、`EngineInitResult=1`、`EngineInitType=6`、`EngineIsEncode=1`；但 `EngineBase=0`、`InitHardwareCail=0`、`LastHardwareInit=0`，且 `SMLBaseServed/SMLNativeStatus/NativeBase` 全缺失 → 两张静态 SML 虚表补丁在两轮实机中都没被调用。
- `AMDUVD7v2HWEngine::init`（0x43D4FB8）逐字节确认：`+0x25C(u16)=0x101` 使 `+0x25D=1`；`initHardware`（0x43D527A）在 `+0x25D!=0` 时**跳过** `vt[0x338]`（loadUVDFW），只调 `vt[0x390]`（initHardwareCail(1)）；`[engine+0x10]!=0` 时直接返回成功。故 loadUVDFW 未执行属原生设计，失败点在 initHardwareCail。
- `initHardware` 成功后访问 `[engine+0x30]->vt[0x128](base+0x500, 1)` 与 `(base*2+0x500, 2)`：base=0 时写错 MMIO，必须先修正基址。
- `initHardwareCail`（0x43D1322，flag=1 分支）：取 `[engine+0x18]` 接口 → `vt[0x2B8]` 得客户端对象 → `客户端+0xD0` 提交消息；selector 8（0x20 字节）与 9（0x120 字节，含 `+0x280` FW 地址、`+0x254` isEncode）都必须返回 0，公共尾块再按 `+0x2B8` 环数量拷贝 16 字节项并 submit(1)，任一非 0 即 false。
- 该客户端 vtable 槽位于 gap 中另一个 fileset（0x94xxxxx–0xa4xxxxx，CAIL/AMDSupport 家族），X5000 符号无法静态定位 → 同样改为运行期按 vptr 接管。
- 本轮实现：
  - 路由 `AMDHardware::initializeExternalInterfaces`（0x43CA04C）：读 `*(void**)(hardware+0x338)`，并在引擎 init 钩子里于调用原函数**之前**再 `attachLiveSML(interface+0x338)`；把对象 vptr 的 `+0x58` 换成 `smlQueryLive`（链式调用原生；选择子 0x0C 应答 0x7800）。
  - 引擎 init 成功后强制 `+0x250=0x7800`（Raven `VCN_BASE__INST0_SEG0`），按 5/6 分别发布 `BaseDec/BaseEnc`。
  - `initHardwareCail` 钩子先 `attachCailSubmit`：接管 `[engine+0x18]->vt[0x2B8]` 客户端的 `+0xD0`，逐 selector 发布 `CailSubmit1/6/7/8/9`、`CailLastSelector/Result`；同时发布 `FirmwareAddress(+0x280)`/`FirmwareSize(+0x29C)`/`RingCount(+0x2B8)`。
  - 静态槽位补丁写后读回：`SMLGFX9Verify/SMLBaseVerify`。
- 下一步：若 `CailSubmit8/9/1` 显示拒绝，按 selector 追 CAIL 侧消息格式；`FirmwareAddress=0` 说明固件未被登记，需要另行处理（`+0x25D` 由 init 固定为 1，原生 APU 路径假定固件已由图形驱动载入）。

## 6. 第四轮：改用 Vega10 原生 `AMDUVD7HWEngine`（15.8.1，2026-10-31）

- 第四轮 ioreg（commit `e84d0a9` 的 CAIL 重试）：跳过 selector 8 后 selector 1 仍返回
  `0xFFFFFFFFE00002C7`（kIOReturnUnsupported）→ 该 CAIL 客户端对 VCN1 消息整体不接受，
  继续修 Vega20 CAIL 路径不可行；Vega20 引擎语义本身也不匹配 Raven。
- `AMDVega10Hardware::allocateHWEngines`（0x43c6bce，0xBE 字节）：数组基址 +0x3B8，
  `+0x3B8=GFX9PM4Engine(0x340)`、`+0x3C0/+0x3C8=GFX9SDMAEngine(0x250)×2`、
  **`+0x3E0=AMDUVD7HWEngine(0x438)`（只有 UVD0）**、`+0x3F0=VCE4v1HWEngine(0x3A0)`。
  VCN1 是 Vega10 世代 IP，原生类因此是 UVD7 而不是 UVD7v2。
- `AMDUVD7HWEngine::init`（0x43d252e，0xF8）：interface 空即 false；`hw=iface->vt[0x2C0]`；
  `hw->vt[0x58](hw, 0xC, 0, 1, &base)`（isEncode 恒 0）；成功后 `+0x210=2`、`+0x404=0xA`、
  word `+0x3B8=0x100`、**byte `+0x25C=1`**、`+0x434=0`；再用 `iface->vt[0x150]` 取 chip id，
  **`+0x25D=(chip==0x10000 || chip==0x20000)`**，`+0x250=base`。Raven 芯片 id 不在两者中，
  故 `+0x25D=0`，`initHardware` 会真正执行 `loadUVDFW`。
- `AMDUVD7HWEngine::initHardware`（0x43d27ec，0x96）：`[self+0x10]!=0` 直接 true；
  `[self+0x25D]==0` → `vt[0x338]`=loadUVDFW（失败即 false）；再 `vt[0x390]`=initHardwareCail(+0x25D&1)；
  成功后 `[self+0x30]->vt[0x128](base+0x500,1)` 与 `(base+0x500+[self+0x250],2)`。
- `UVDHWEngine::loadUVDFW`（0x43d0c40）：读 `[self+0x3BA]`（已缓存标志）、`[self+0x280]`（固件指针）、
  `[self+0x2A8]`、`[self+0x270]`，随后用 `[self+0x38]` 队列的
  `vt[0x140](queue,[self+0x2B0],&local,global,0x15,0x11,0,0)` —— **走 SML 队列，不碰 CAIL**。
- `AMDUVD7HWEngine::commitUVDFWMsg`（0x43d28a8）：把固件下载拆成 SML 寄存器写消息，寄存器基址
  `[self+0x250]` + `0x3C4/0x3C5/0x3C3`，经 `[self+0xC0]->vt[0x120]` 与 `[self+0x20]` 的
  `vt[0x1A0]/[0x1B0]/[0x258]/[0x248]/[0x140]/[0x148]/[0x160]` 提交。
- 虚表核对 `__ZTV30AMDRadeonX5000_AMDUVD7HWEngine` @0x45a0e20：`+0x118=0x43d252e`(init)、
  `+0x338=0x43d0c40`(loadUVDFW)、`+0x348=0x43d28a8`(commitUVDFWMsg)、`+0x368=0x43d27ec`(initHardware)、
  `+0x390=0x43d1322`(initHardwareCail)、`+0x2B8=0x43d2c10`；对照 UVD7v2 @0x45a2928：
  `+0x118=0x43d4fb8`、`+0x368=0x43d527a`、`+0x348=0x43d5330`。
- 本轮实现：VCN1 只分配一个 `AMDUVD7HWEngine`（槽 5），`hasUVD1=false`；prepare 的符号与槽位校验
  全部换成 UVD7；新增 `commitUVDFWMsg` 钩子发布 `FwMsgOpcode/FwMsgResult/FwMsgCount`；
  `initHardwareCail` 钩子的字段语义修正为 `+0x29C=RingCount`、`+0x2B8=RingArrayLow`。
- 下一步：若 `FwMsgCount` 不出现，说明 `+0x10` 提前返回或 `loadUVDFW` 未走到提交；若
  `FirmwareAddress(`+0x280`)==0`，需要找固件登记方（HWLibs 的 `_CailUvdLoadEngine` 或
  `AMDAccelVideoContext` 的会话建立），并检查 `Start/Power/Rings/Channels` 是否依次通过。


## 7. 第五轮：视频上下文链（15.8.1，2026-10-31）

- 第四轮 ioreg（`a93454f`）只有 `EngineInitResult=1/Type=5/BaseDec=0x7800`、`RingsDec=1`、
  `ChannelsDec=1`；`Hardware*/LoadUVDFW/FwMsg*/Cail*/Start/Power` 全部缺失 →
  `AMDUVDHWEngine::start`、`AMDUVD7HWEngine::initHardware`、`initHardwareCail` 从未执行，
  视频上下文根本没有创建（环与通道在 `AMDHWEngine::init` 阶段就分配完，不依赖 start）。
- 调用链证据：`AMDUVDHWEngine::start`（0x43D007C）在 0x43D00FF 调 `vt[0x368]`=initHardware；
  `AMDAccelUVDContext::init`（0x4393A9C）在 0x4393B9B、`AMDAccelVideoContext::contextStart`
  （0x437D086）也各自调 `vt[0x390]`=initHardwareCail —— 只要上下文建起来，`Cail*`/`Hardware*` 就会出现。
- **第一道闸门**：`AMDGFX9GraphicsAccelerator::newVideoContext`（0x4391ED8）=
  `if ((*(byte*)(this+0x1F10) & 8) == 0) return 0; else new Vega10VideoContext(0x1278)`。
  X5000 `__TEXT` 中直接写 +0x1F10 的指令命中 0 处 → 该字节由整块能力结构写入。
- `hardware->vt[0x158]` = 0x43C6E18 = `return self + 0x30`（`AMDVega10Hardware::getHWInfo`）⇒
  `Vega10VideoContext::contextStart` 读的 `info->hasUVD0/1/VCE`（+0x84/0x85/0x86）就是
  **hardware+0xB4/0xB5/0xB6**，与 fork 在 `>=13` 分支的字段偏移一致，偏移本身没问题。
- `AMDAccelVideoContext::getHWInfo`（0x437E9F8）填 `sHardwareInfo[0..0x45]`：`[0x44]=1` 由
  hardware+0xB5（hasUVD1）决定、`[0x45]` 由 hardware+0xB8 决定；UVD0 能力来自
  `hardware->vt[0x1C0]()` 返回结构体（`puVar3`）的 +0x120/+0x124。
- `AMDVega10Hardware::setupAndInitializeHWCapabilities`（0x43C6B8E）只写 +0x64/+0x6C/+0xA0/+0xBC，
  **不写 0xB4..0xB7** → hasUVD*/hasVCE 由 fork 的 wrap 决定；现在 wrap 之后立即发布
  `HasUVD0/HasUVD1/HasVCE`，可直接与 `VcQueue*` 对照。
- `Vega10VideoContext::getCapabilities`（0x4395160）把 caps[0..3] 取自 `+0x1240` 队列的 `vt[0x270]`、
  caps[4..7] 取自 `+0x1248`、caps[10..11] 取自 `+0x1250`；这三个队列是
  `Vega10VideoContext::contextStart`（0x4394AAA）按 hasUVD0/1/VCE 建的，队列为空则 caps 全 0。
- `Vega10VideoContext::newContext`（0x439532C）：`createInfo+8 < 6` 走解码分支；失败码
  `0xE00002C0`（`accel+0xC78` bit1）、`0xE00002BE`（AccelUVDContext 分配失败/芯片闸门/requestCapability）、
  `0xE00002C7`（`engine->vt[0x118]` init 失败）。
- 本轮实现：
  - 在已有的 `newVideo` 路由里接管闸门（该符号在 X5000 路由表中已占用，只能路由一次）：
    VCN1 时把 `+0x1F10 |= 8` 再调用原生，发布 `NewCtxFlag/NewCtxForced/NewCtxResult`。
  - 新增 6 条诊断路由：`AMDAccelVideoContext::contextStart`（`AccelVideoStart`）、
    `AMDVega10VideoContext::contextStart`（`VideoCtxStart`、`VcQueueUVD0/UVD1/VCE`）、
    `getCapabilities`（`CapResult/CapUVD0/CapUVD1/CapVCE`）、`newContext`
    （`NewCtxType/NewCtxCodec/NewCtxStatus`）、`AMDAccelUVDContext::init`（`UvdCtxInit`）、
    `requestCapability`（`ReqCap/ReqCapMode`）。
  - SML 基址改为「原生非 0 优先」：实机原生选择子 0x0C 返回 `0x7E00`（此前被硬写成 0x7800 覆盖），
    只有原生为 0 时才退回 `VCN_BASE__INST0_SEG0 = 0x7800`。
- 判读：`NewCtxFlag` 出现而 `NewCtxResult=0` → 闸门未开；`VcQueueUVD0=0` → hasUVD0 字节没生效；
  `CapUVD0=0` → 队列能力为 0（用户态不会选硬解）；`NewCtxStatus` 给出 newContext 的具体失败码；
  `UvdCtxInit=0` 且 `CailDec=0` → 卡在 CAIL 消息层。

## 8. 第五轮实机：上下文打通后的空窗

第五轮 ioreg（Safari 播放 1080p）把上一节推断的整条链全部点亮：`NewCtxFlag=255`（加速器 +0x1F10 的
bit3 本来就是置位的，`NewCtxForced` 并未改动它）、`NewCtxResult=1`、`AccelVideoStart=1`、
`VideoCtxStart=1`、`VcQueueUVD0=1`（UVD1/VCE 为 0，与 `HasUVD0=1/HasUVD1=0/HasVCE=0` 一致），
引擎侧 `EngineInitResult=1`、`EngineInitType=5`、`EngineBase=0x7800`（`EngineBaseForced=1`）、
`RingsDec=1`、`ChannelsDec=1`，SML 侧 `SMLPresent=1`、`SMLLiveHooked=1`、`SMLGFX9Verify=1`、
`SMLBaseVerify=1`、`SMLNativeStatus=0`、`SMLNativeBase=0x7E00`、`SMLBaseServed=5`。

空窗在上下文之后：`CapUVD0/CapResult`、`NewCtxType/NewCtxCodec/NewCtxStatus`、`UvdCtxInit`、
`ReqCap*`、`Cail*`、`FwMsg*`、`LoadUVDFW`、`HardwareInitDec`、`StartDec`、`PowerDec` 一个都没出现。
也就是说 video context 建好、start 成功，随后既没有人来问能力（`getCapabilities` 未被调用），
也没有人来建解码上下文（`newContext` 未被调用）；引擎的 `initHardware`（下载固件）在 `init` 之后
从未进入。`AMDHardware::initializeHWEngines` 与 `startHWEngines` 的钩子也没有触发。

下一处要定位的是「谁在用这个 video context」：`AMDAccelVideoContext` 同时是
`IOAccelContext2`/`IOUserClient` 的子类，用户态（AppleGVA 的 `AMDRadeonVADriver2`）与它的全部交互
都经过虚函数 `getTargetAndMethodForIndex(IOService**, uint32_t)`（0x437DA62，index 只接受
0x100..0x111，方法表基址 0x4584730，条目 0x30 字节；`*target` 被写成 self，`this+0x10E8`
指向该方法表）。因此本轮加了两个探针：

- `getTargetAndMethodForIndex` 钩子：按 index 置 `SelSeen` 位图（bit0 = 0x100），并记录
  `SelLast/SelCount`，前 24 次打印 `VCN1 ... index=0x%X`。
- `getHWInfo`（0x437E9F8）钩子：记录返回码与能力结构 0x00/0x40/0x44 处的值。0x44 由
  hardware+0xB5（`hasUVD1`）决定，正是用户态判「有没有第二个 UVD」的槽位。

判读：`SelCount=0` 说明用户态从没拿到这个 video context 的用户客户端（问题在加速器上报或
`IOServiceOpen` 的路由）；`SelLast` 停在某个选择子就顺该方法继续跟；出现 `HWInfoStatus` 而
`HWInfoTail=0` 则是上报的能力结构自相矛盾。同步抓全量属性（`grep '"NRed'`），
`NRedVCNUser*/NRedVCNDynamic*` 才反映用户态补丁此刻的真实状态。

## 9. 第六轮实机：用户态补丁表根本没有 Picasso

第六轮 ioreg 只多了三个新属性，却把空窗的原因指了出来：

- SelSeen=1（仅 bit0）、SelLast=0x100、SelCount=9：用户态确实打开了 video context 的用户客户端，
  但只用过选择子 0x100（getHWInfo 那一个），之后再没有 0x101/0x102 之类的方法调用。
- HWInfoStatus=0、HWInfoFirst=142、HWInfoMid=0、HWInfoTail=0：能力结构已返回给用户态。
- **NRedVCNUserRejected=3，同时 NRedVCNUserEnabled、NRedVCNUserApplied、NRedVCNDecoderPID
  全部缺失** —— 这是决定性的一条。

NootedRed/UserVideoDecode.cpp:441 的语义是「找不到设备补丁表就整条用户态链退出」：

    devicePatches = sequoia ? UserSequoiaVideoData::findDevice(deviceID) : UserVideoDecodeData::findDevice(deviceID);
    if (!devicePatches) { NRed::singleton().setProp32("NRedVCNUserRejected", 3); return; }

而 NootedRed/UserSequoiaVideoData.hpp 是 tools/build_video_decode_patch.py 生成的，仓库里那一份只有
0x15E7/0x1636/0x1638/0x164C 四个 VCN2 设备（git log -- NootedRed/UserSequoiaVideoData.hpp 只有上游
ac1a08e 一次提交），生成时 VCNCapabilities 里还没有 0x15D8。于是这台机器上：

1. identify 没被改写成 family 4 —— 未打补丁的 VADriver2 对 0x15D8 仍然判成未知家族；
2. 因此 VADriver2 从不选择 AMD 硬解，AppleGVA 也就不会来问 caps、不会建解码上下文；
3. 内核侧那条链（NewCtxResult=1、VideoCtxStart=1、VcQueueUVD0=1）虽然已经通了，但没人使用它。

修复就是重新生成补丁表：python3 tools/build_video_decode_patch.py --sequoia-cache
/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld，新表里多出 {0x15D8, Patches15D8}，
它的 identify 载荷注入 family 4，VCN2_ENCODE_LABELS 里的编码器条目按 VCN1 规则退化为原样字节。

判读：装上新 kext 后 NRedVCNUserRejected 应当消失并出现 NRedVCNUserEnabled=1、NRedVCNUserApplied、
NRedVCNDecoderPID（说明 AMDRadeonVADriver2 真的被打了补丁并选中硬解），SelSeen 也应超出 bit0。

构建提示：生成器在这台机器上一次完整运行要几分钟，因为每次 xcrun clang 调用在沙箱里要 8–10 秒
（compile_payload），不是卡死；用 nohup ... & 跑完再看 NootedRed/UserSequoiaVideoData.hpp 是否变化。
## 10. 第九轮准备：客户端身份与 VCN 基址

第七轮留下了两个必须用实机数据才能定下来的点：CAIL 客户端 `[hw+0x340]` 是什么对象，以及引擎的
`+0x250` 该用哪一个 VCN 基址。

身份判定不能再依赖滑动后的绝对地址，因此改用**虚表差值**：`AMDUVD7HWEngine` 的 `__ZTV`
（实机 0x45A0E20）作为参照，把对象 `*(void**)` 与它相减，差值不随 KASLR 变化，可以和切片里的静态
差值直接对照：

| 类 | `__ZTV` | 相对 UVD7 的差值 |
| --- | --- | --- |
| `AMDVega10Hardware` | 0x459C5C0 | −18528 |
| `AMDVega12Hardware` | 0x459D100 | −15648 |
| `AMDVega20Hardware` | 0x459DC40 | −12768 |
| `AMDHardware` | 0x459F088 | −7576 |
| `AMDUVDHWEngine` | 0x45A01C8 | −3160 |
| `AMDUVD7HWEngine` | 0x45A0E20 | 0 |
| `AMDSMLInterface` | 0x45A7CF0 | +28368 |
| `AMDSMLCPMsg` | 0x45A8C60 | +32320 |
| `AMDSMLGFX9Interface` | 0x45A8EE8 | +32968 |

新属性 `NRedVCN1DHwZtv`、`NRedVCN1DEngineZtv`、`NRedVCN1DSmlZtv`、`NRedVCN1DClientZtv` 分别是
硬件对象、引擎对象、`[hw+0x338]`、`[hw+0x340]` 的虚表差值。只要客户端不是上表里的类，它的差值就能在
X5000 切片的 `__ZTV` 表里唯一反查（`build/vcn/list_ztv.py`）。

基址方面，第五轮起 `smlQueryImpl` 已经改成原生优先，实机原生选择子 0x0C 返回 **0x7E00**，但 `uvd7Init`
仍然无条件把它改成 0x7800（`EngineBaseForced=1`）。这次改为**只在原生值缺失或越界（不在 0x7000–0x8FFF）
时**才退回 0x7800，并新增 `EngineBaseNative` 记录覆写前的值。理由：0x7E00 是 Apple 按本机 ASIC 算出来的，
0x7800 只是 Vega10 的 `VCN_BASE__INST0_SEG0` 猜测；`initHardware` 用这个基址去写 `base+0x500` 的两个
寄存器，基址错会波及 UVD 块的后续状态。

判读：`DSmlZtv=+32968` 说明实例是 `AMDSMLGFX9Interface`（静态补丁有效），`+28368` 是基类
`AMDSMLInterface`，`+32320` 是 `AMDSMLCPMsg`（**这一张表此前从未被静态补丁覆盖**，也是第四轮
`SMLBaseServed` 缺失的合理来源）；`DClientZtv` 若等于 `+32320`，说明 CAIL 提交走的是 SML CP 消息通道，
失败就落在 CP 消息环而不是 CAIL 的 caps 表。

## 11. 第九轮实机：CAIL 客户端就是 AmdCailServices

第九轮 41 条属性里，虚表差值把两件长期靠猜的事定死了（换算规则：静态差值 = 实测差值 − 0x10，
因为对象 vptr 指向 `__ZTV` 符号 + 0x10）。

| 对象 | 实测差值 | 归属 | 类 |
| --- | --- | --- | --- |
| 硬件对象 `[hw]` | −18496 | X5000 | `AMDVega10Hardware`（静态表 −18528） |
| 引擎对象 | +16 | X5000 | `AMDUVD7HWEngine`（同表，符合 +0x10 规则） |
| `[hw+0x338]` | 6232816 | **X5000HWLibs** | `AmdTtlServices`（`__ZTV14AmdTtlServices`=0x4B92900） |
| `[hw+0x340]` | 6278000 | **X5000HWLibs** | `AmdCailServices`（`__ZTV15AmdCailServices`=0x4B9D980） |

也就是说：`AMDHardware::initializeExternalInterfaces` 里 `vt[0x850]()` 拿到的是 Radeon 固件侧的
TTL 服务，`vt[0x858]()` 拿到的是 CAIL 服务——**两者都不在 AMDRadeonX5000 里**，所以早先给 X5000 的
`__ZTV34…AMDSMLGFX9Interface` 打 +0x58 补丁从来没有被调用过（实例根本不是那个类）。

### 两个槽位的真正身份

- `AmdTtlServices::queryHwBlockRegisterBase(hwblock_type, UInt16, UInt32, UInt32*)`（0x4671BC0，X=0x58）：
  引擎询问 VCN 寄存器基址走的就是它，原生对 hwblock 0xC 返回 **0x7E00**（第九轮 `EngineBaseNative`、
  `SMLNativeBase` 都是 0x7E00，且没有触发回退）。
- `AmdCailServices::uvdControl(CailUvdFuncId, CailUvdInput*, CailUvdOutput*)`（0x485CC70，X=0xD0）：
  `initHardwareCail` 的 `client->vt[0xD0](client, selector, 0, msg)` 就是它。入口两道闸门：
  `cmp byte ptr [rdi+0x14], 0; je → 返回 0xE00002C0`，`dec esi; cmp esi,8; ja → 返回 0xE00002C7`（合法 id 1–9）。

### 为什么第七轮所有提交都失败

`AmdCailServices::initialize`（0x485A440）与 `earlyInitialize`（0x485A120）自己也都以
`[+0x14] != 0` 为前提（否则分别返回 0xE00002C0）。`earlyInitialize` 之后做的事是：检查回调结构版本
`[rsi] == 0x358`、VBIOS 镜像签名 `[+0x15]==0x55 && [+0x16]==0xAA`、调 `_CAILGetExtensionSize`
（0x47EDBD0）分配 CAIL 适配器并存进 `[+0x20038]`；`initialize` 则把 `_CailInitialize` 的 +0x8…+0x40
拷进 `self+0x20400…0x20438`、取 `[+0x51]` 写成 `+0x20448`，再调 `_CAILPostVBIOS`（0x47EDBDB）。

而 `[+0x14]` 既不是构造器写的，也不是这两个函数写的（在 0x485A010–0x485D800 范围内没有任何
`mov byte ptr [reg+0x14], imm`），说明它是**由上层（服务对象的宿主类）在把 CAIL 服务交出去之前置位的**。
我们的 VCN1 路径把引擎挂进 X5000 之后直接进入 `initHardwareCail`，这一步没有发生，于是 `uvdControl`
对 selector 1/8/9 一律回 0xE00002C0——与第七轮 `CailDecFailed=8`、`CailRetryResult=0` 完全吻合。

### 本轮新增诊断

`CailReady`（`[client+0x14]`）、`CailVbios`（`[client+0x15]`）、`CailAdapterLow`（`[+0x20038]` 低 32 位）、
`CailEarly`/`CailEarlyCalled`（X=0x10）、`CailInit`/`CailInitCalled`（X=0x18）、`DSvcZtv`（`[hw+0x320]` 的差值）。

判读：`CailReady=0` 而 `CailEarlyCalled`/`CailInitCalled` 都缺失 → 上层从未初始化 CAIL 服务，下一步要
找到宿主的调用点（差值可先定位宿主类）；`CailReady=1` 但仍失败 → 问题在 `_CAILUvdControl` 内部，
按 `CailLastResult` 的具体码继续查。

## 12. 第十轮：CAIL 能力闸门与 _CailUvdInit

AmdCailServices::uvdControl(0x485CC70) 把任何非零 CAIL 返回都归一化成 0xE00002C7，所以在 HWLibs 层钩 _CAILUvdControl(0x47F05E7) 才能看到真实原因：

- _CAILUvdControl：[adapter+0x14] 是设备在位闸门（构造器 0x485A010 用 0F 95 写入，读 PCI 0xFFFF 判定）；id 只接受 1..9；能力闸门 _CailCapsEnabled(adapter+0x178, 0xD6) 与 (…, 0x15B) 都关时返回 1，位图在 adapter+0x358（16 dword）；test byte [adapter+0xE34],4 为 0 返回 3，bit17 置位返回 0xA。
- 原生 DDI 表 _CAIL_DDI_CAPS_PICASSO_A0(0x60F0440) 的 word6=0x20000000、word10=0x02000120，bit22（0xD6）与 bit27（0x15B）都是 0，所以 Picasso 上 CAIL 默认不给 UVD 能力，VCN1 必须自己置位。
- _CailUvdInit(0x47F64BF) 门禁依次是 [adapter+0x15D0]&0x10、输入版本 0x148、[adapter+0xE48] bit8/bit20、[adapter+0x370]&4 与 [adapter+0x378]&0x48000，全部为 0 时走 call qword ptr [adapter+0x12D8]（ASIC 专用 UVD init），其返回值就是 initHardwareCail 的错误码。

诊断字段：CailCapBase6/CailCapBase10（adapter 首次出现的原始能力位）、CailCapD6/CailCap15B（置位后）、CailLibFunc/CailLibResult（库层真实返回）、CailInitVersion/CailInitMode/CailE48/CailUvdW6/CailUvdW8/CailInitResult、CailInitPtr/CailInitPtrHi/CailInitFunc/CailInitDelta/CailE15D0。

当前阻塞：[adapter+0x12D8] 指向的 ASIC init 返回 1。用 CailInitDelta（与 _CailUvdInit 的地址差）比对即可确认是哪一个：Tahiti 0x11297、Dummy 0x1D28D、Bonaire 0x23710、Tonga 0x30806、Ellesmere 0x39F96。

## 13. 第十一轮：定位 ASIC init 为 _greenland_uvd_init

实机 dump 里没有 `CailInitDelta`，但已有的 `CailInitFunc=0x296476` 是相对 HWLibs `__TEXT` 基址
（0x45BB000）的偏移：`0x45BB000 + 0x296476 = 0x4851476`，符号表里正好是 `_greenland_uvd_init`。
它与 `_CailUvdInit`(0x47F64BF) 的差值是 **0x5AFB7**。上一轮的五项差值表（Tahiti 0x11297、Dummy 0x1D28D、
Bonaire 0x23710、Tonga 0x30806、Ellesmere 0x39F96）只枚举了大写 `*UvdInit` 拼写，漏掉了小写的
`_greenland_uvd_init`；Greenland 正是 Vega10 家族（Picasso 的 VCN1 走这一支）。

`CailInitDelta` 没写入的原因：`KernelPatcher::routeFunctionInternal` 在 wrapper 模式下执行
`if (org) *org = trampoline;`，所以 `cailUvdInitOriginal` 是 Lilu 的 trampoline，不是 `_CailUvdInit`
的符号地址；`ptr > cailUvdInitOriginal` 不成立。修正为记录 `RouteRequest::from`（solveSymbol 结果）。
按该基准，Greenland 的差值应为 0x5AFB7。

### 槽位怎么被装上

- `_Cail_Greenland_InitFunctionPointer`(0x484F9E4) 在 0x484FCA4 用
  `lea rax, [rip+0x17CB]` 得到 0x4851476，0x484FCAB `mov [adapter+0x12D8], rax`。
- `_CAILInitFunctionPointer`(0x4813BFF) 在能力位 0x148（word10 bit8）打开时调用它；Picasso 的
  `_CAIL_DDI_CAPS_PICASSO_A0` word10=0x02000120，bit8 已置位。随后 APU 能力位 0x53 再调
  `_Cail_Raven_InitFunctionPointer`，但后者不动 +0x12D8，所以该槽保持 Greenland。
- `_CAILAsicCapsInitTable` 里 Picasso 条目：familyId=0x8E、deviceId=0x15D8、revision=0、
  extRevision=0x41、ddiCaps=0x60F0440、goldenSettings=0x67348A0。按 `_CAILInitFunctionPointer`
  的读数约定，adapter+0x190=familyId=0x8E、+0x19C=extRevision=0x41（本轮新增 CailFamily/CailRevision 实机确认）。

### 返回 1 的路径

`_greenland_uvd_init`(0x4851476) 先看 `[adapter+0x190] == 0x8D`：Picasso 是 0x8E，不走 UVD 7.2 分支；
再看能力 0x133/0x134（word9 bit19/20，Picasso word9=0x09E20E10，均为 0），于是尾跳
`_uvd_init`(0x48410D8)：

- `test byte [adapter+0xE4A], 2`；置位直接返回 0（dump 里 CailE48=0，不置位）。
- 否则 `edx=1` 尾跳 `_uvd_init_engine`(0x48410F4)。

`_uvd_init_engine` 在 edx=1 时唯一能返回 1 的路径是 0x48416AB：

```
0x48416AB  mov r14d, 1
0x48416CA  call _Cail_MCILWaitFor(adapter, waitObj, 1, 1, 1, 0xBB8, 4)
0x48416D1  test eax, eax
0x48416D3  jne  0x4841B00        ; eax=r14d=1
```

waitObj 由 0x484168C 起填：`[0] = 0x83AF`、`[+8] = 0x200000002`（dword +8=2、+0xC=2），即等待
UVD 寄存器 0x83AF 的 bit1 变为 1，超时 0xBB8。另一个 `r14d=1`（0x484193B）只在第三个参数为 0 时可达，
`_uvd_init` 传的是 1，与本路径无关。

`_Cail_MCILWaitFor`(0x47FEB3E) 先看 `[adapter+0x18]` 和 `[[adapter+0x18]+0x168]`：任一为空立即返回 1；
否则调用该回调做等待，返回码非 0/2 时映射成 1。因此失败要么是 MCIL 对象/回调缺失，要么是 0x83AF bit1
在超时内没有置位。本轮新增 `CailMcilLow`/`CailMcilWaitLow` 区分这两种情况。

### 本轮改动

- 修正 `CailInitDelta` 基准（`initRoute.from`）。
- 新增 `CailFamily`、`CailRevision`、`CailMcilLow`、`CailMcilWaitLow`。
- 真实 init 返回非 0 时，把 `adapter+0x12D8` 换成返回 0 的桩再调一次 `_CailUvdInit`：
  `CailInitFirstResult` 保留原始失败码，`CailStubResult`/`CailInitResult` 是桩后的结果，
  `CailAsicStub`/`CailAsicStubPtr` 记录桩状态和原指针。
- 下一轮实机判读：`CailInitDelta==0x5AFB7`、`CailFamily==0x8E`、`CailStubResult==0`、
  `CailInitResult==0`，再看 `CapUVD0`/`CapResult` 是否非零；若 MCIL 回调为空，则 0x83AF 等待
  本来就无从谈起，桩是正确绕法。

## 14. 第十一轮实机：内核打通，用户态卡在 15D8 的 Vcn2Decode Manifest

新 dump（2026-11-07 18:00）证实上一轮推断：

- `CailInitDelta=372663`（0x5AFB7）→ `_greenland_uvd_init`；`CailFamily=142`（0x8E）、
  `CailRevision=66`（0x42，PCI revision 1 对应的 extRevision）。
- `CailInitFirstResult=1`、`CailStubResult=0`、`CailInitResult=0`：真实 Greenland init 仍失败，
  返回 0 的桩生效。
- `CailMcilLow`、`CailMcilWaitLow` 都非零，说明不是 MCIL 对象/回调缺失，而是 0x83AF bit1 等不到；
  桩绕过后 `InitHardwareCail=1`、`CailDec=1`、`HardwareInitDec=1`、`ProbeInitResult=1`，
  内核 VCN1 初始化链路已打通。`CailRetryFailedDec=0`（flag=0 重试成功）。

用户态仍没有硬解：`NRedVCN1CapUVD0`/`CapResult` 未出现（`getCapabilities` 从未被调用），
也没有 `NRedVCNUserApplied`/`NRedVCNDecoderPID`。现场证据：

- QuickTime Player(PID 12100) + VTDecoderXPCService(PID 12102) 存在；日志
  `(AppleGVA) GVA error: AVD_api.Create failed err=5`、`physicalAccelDispose ... == false`。
- 共享缓存阶段位：D00 Called、D01 OriginalError、D03 CacheRead、D04 CacheMismatch、
  D06 CodeMismatch、D09 NoMap；`UserLastError=4`（KERN_INVALID_ARGUMENT）；
  `DynamicMatched=1`、`DynamicReads=3295`，但没有任何 Applied 阶段。
- 缓存 UUID 四字（0x9028EC42/0x7E3140BC/0x144527BB/0x267FFF2B，按小端读出）就是
  `42EC2890BC40317EBB2745142BFF7F26`，即 `UserSequoiaVideoData.hpp` 的 CacheUUID。

失败点：`patchSharedRegion` 在 `selectFairPlayDecode` 成功后（`DRMVideoPatchCount` 已发布）调用
`VPF::ManifestData::find(Vcn2Decode, OsDarwin24, 0x15D8)`，而 `PatchManifestData.hpp` 里
Vcn2Decode 只有 15E7/1636/1638/164C，没有 15D8 → 返回 null → `stage(CodeMismatch)` +
`error(KERN_INVALID_ARGUMENT)`，与 dump 的 stage/error 完全一致。生成头本身是陈旧的结果：
`python3 tools/vpf/generate_manifest.py --check` 报「已陈旧或被手改」，因为 1276959 加了
`Patches15D8` 但没有重新生成 Manifest。

修复：运行 `python3 tools/vpf/generate_manifest.py --write`，只新增

```
Sites_Vcn2Decode_Darwin24_15D8 = makeSites(UserSequoiaVideoData::Patches15D8, ...)
{FeatureId::Vcn2Decode, OsDarwin24, 0x15D8, ..., 23}
```

共 33 个功能实例、354 个补丁站点；`--check`、`validate_manifest.py`、`test_vpf_manifest.py`、
`test_display_manifest.py` 全部通过。下一轮装新 kext 后应能看到 FairPlay 事务进入 Applied，
`UserApplied`/`DecoderPID` 出现，随后 `CapUVD0`/`CapResult` 应被 `getCapabilities` 写入。

## 15. 第十二轮：15D8 的 Manifest 补齐后暴露 planner 设备掩码缺位

新 dump（2026-11-07 21:06）里 `NRedVCNVPFPlannerRejected=1`，说明上一轮的 Manifest 修复生效了：
`ManifestData::find(Vcn2Decode, OsDarwin24, 0x15D8)` 已经能返回条目，失败点后移到
`adaptPatches`/`planVideoFeatures` 这一块（两者共用同一个诊断位）。`DRMVideoPatchCount=18`
是 `selectFairPlayDecode` 回写的「FairPlay 子集数量」，不是陈旧 build 的痕迹（23 项里选出 18 项）。

`planVideoFeatures` 调 `VPF::plan`，而 `dependencyPlanner` 的设备门禁是：

```cpp
constexpr UInt32 deviceBit(UInt32 id) { case 0x15D8: return 1U << 0; ... }
constexpr UInt32 VcnDeviceMask = (1U << 2) | (1U << 3) | (1U << 4) | (1U << 5);
```

0x15D8 的 bit0 不在 `VcnDeviceMask` 里，`gated()` 直接拒绝 `Vcn2Decode`，所以 FairPlay 与普通解码
都进不了安装阶段。`tools/vpf/planner_probe.cpp` 里原本还专门断言 0x15D8 是「video display device」
（只做显示、不做 VCN 视频），说明这个掩码一直没跟上 1276959 的 15D8 视频补丁表。

修复：

- `VcnDeviceMask` 加入 bit0；0x15DD 仍保持只有显示支持（没有 15DD 的视频补丁表/Manifest）。
- `planner_probe` 把 0x15D8 放进正向视频设备列表，改用 0x15DD 做「display-only 应拒绝」的用例。
- 重新冻结基线：`Vcn2Decode/Darwin24/15D8` 出现在计划里（23 个站点、3 张映像、reads=2358），
  与其它 Darwin24 VCN 设备逐项一致；计划总数 30→31。
- `test_vpf_video_equivalence.py` 现在把 9 个旧 Resolver 计划与 9 个当前 Manifest 计划逐字节比较，
  15D8 也通过了——说明补丁字节与旧路径一致。

下一轮实机应看到 FairPlay 事务进入 Applied（`UserD15`、`UserApplied`、`DecoderPID`），随后
`CapUVD0`/`CapResult` 应被 `getCapabilities` 写入。若还停在新阶段（Region/Protection/Write 等），
按 `DRMVideoLastStage` 与 `NRedVCNVPFQuarantined` 继续。

## 16. 第十三轮：用户态补丁生效，内核 `_CailUvdInit` 重复调用门禁

21:53 dump 是第一次用户态链路真正打通：

- `VCNUserApplied=16`、`VCNUserD15=1`（Applied）、`VCNVPFPath=1`、`VPFPlannerEnabled=448`、
  `VPFPlanState=2`（AllPatched）、`VCNGVASelectionFix=1`、`VCNDecoderPID=1027`；
  GVA 日志不再出现 `AVD_api.Create failed err=5`。
- VA 驱动开始查询内核：`NewCtxType=0`、`NewCtxCodec=3`、`NewCtxStatus=0`、`UvdCtxInit=1`、
  `ReqCap=1`、`ReqCapMode=6`、`CapResult=0`、`CapUVD0=0`。

但内核解码引擎这次反而没起来：`HardwareInitDec=0`、`StartDec=0`、`InitHardwareCail=0`、`CailDec=0`、
`CailLastResult=0xE00002C7`。

原因不在桩本身，而是 `_CailUvdInit` **对同一适配器不幂等**：

1. 第一次调用成功（槽被换成返回 0 的桩），末尾按 mode=0 置 `[adapter+0xE48] |= 0x100`。
2. `uvd7InitHardwareCail` 的 flag=0 重试再次走 `_CAILUvdControl(id=1)` → `_CailUvdInit`。
3. 库层门禁（0x47F64FF 起）看到 bit8 已置、mode=0，直接在 0x47F650F 返回 1，根本没走到
   `[adapter+0x12D8]`。dump 里的 `CailE48=0x100`、`CailInitFirstResult=1`、`CailStubResult=1` 正是这条。

这一轮第一次尝试比前几轮走得更远（用户态 VA patch 改变了初始化时序），所以第一次尝试自己就完成了
CAIL init；以前是 flag=1 尝试在 uvdControl(1) 之前失败，flag=0 重试才做第一次 init，因此没撞上门禁。

同时发现桩安装逻辑的另一个问题：`cailAsicInitStubActive` 是全局 bool，第二个 adapter 失败时不会再装桩
（dump 里 `CailAsicStubPtr` 是一个 adapter 的 Greenland 指针，而最后一次调用的 `CailInitPtr` 是另一个
adapter 的槽，`CailInitDelta=0x7EBA6AA1`）。

修复：

- `_CailUvdInit` 钩子在入口检查该 mode 的完成位（mode 0 → bit8，mode 1 → bit20）；已置位就直接返回 0，
  不再调原函数（等价于「这个 adapter 已经做过这次 init」）。
- 桩改为按 adapter 安装：只有当前槽不是 `cailAsicInitReturnZero` 时才替换，并记录
  `CailAsicStubAdapter`。
- 新增 `CailInitCalls`、`CailInitAlready` 诊断。

下一轮预期：`HardwareInitDec=1`、`StartDec=1`，随后 `CapUVD0`/`CapResult` 应为非零；若 Cap 仍为 0，
再查引擎 start / capability 填充路径。

## 17. 第十四轮：解码通道挂死与安全诊断构建

装上「重复 init 短路」构建后，登录约 55 秒（VTDecoderXPCService 启动）系统冻结。诊断报告
（`/Library/Logs/DiagnosticReports/Kernel_2026-11-07-22*.gpuRestart`）给出：

- Restart Channel: **12 UVDDec**；`UVD0: Enabled`；`SubmittedTS=2, CompletedTS=0`。
- FirstPendingCB: PID 550, `VTDecoderXPCServ`, SubmitContext=Video, VMID=3。
- `AMDRadeonX5000HWLibs` 日志：`HangFlags 0x4: FenceNotRetired=1, FullAsicReset=1`。
- 三次冻结分别在 22:34/22:36/22:38，都在启动后 ~55 秒。

结论：`adapter+0x12D8` 被换成返回 0 的桩后，`_greenland_uvd_init` 的整套 UVD 寄存器初始化被完全
跳过。上一轮让 `_CailUvdInit` 的重复调用短路成功后，`initHardwareCail` 返回成功、引擎 `start()`
继续执行，VA 驱动于是向 UVDDec 环提交真实解码命令；但 UVD 块从未被真正初始化，命令永远不完成，
GPU 看门狗做 FullAsicReset。冻结不是内存/显示损坏，而是 UVD 通道挂死。

因此桩只能作为诊断，不能作为可用路径。用户已回滚到上一版（UUID `E14758AF`，`HardwareInitDec=0`）
并稳定运行——没有引擎 start 就没有提交，也就不会挂死。

安全诊断构建（本轮）：

- 桩默认关闭，仅在 boot-arg `-NRedVCNAsicStub` 存在时启用；没有桩就没有成功的 init/start，
  不会向 UVDDec 提交命令，系统保持稳定。
- 解析 `_ulReadMmRegisterUlong`，在真实 init 前后读 `0x83AF`（`UVD_STATUS`）：
  `CailUvdStatusBefore` / `CailUvdStatusAfter`。Linux `vcn_v1_0_wait_for_idle` 等的是
  `UVD_STATUS == 0x2`（bit1 = idle），与 `_udv_init_engine` 的 waitObj `{0x83AF, mask=2, value=2}`
  完全一致；这两个读数会告诉我们真实 init 失败时状态是 0x0、0x1 还是别的值。
- 保留重复 init 短路（真实 init 失败时 E48 不会置位，短路不会触发）。
- 新增 `CailMmioReadResolved` 确认读函数解析成功。

下一步：用这个安全构建拿到 `CailUvdStatusBefore/After`，判断 UVD 块是否因为未上电/未解复位而没有
进入 idle；再决定是补时钟/复位序列，还是修正 APU 上 Greenland init 与 VCN1 的差异。

## 18. 第十五轮：UVD_STATUS 全 0，固件先行诊断

安全诊断构建（UUID `8F47944A`）实机稳定（uptime 7 分钟，无冻结），读数：

- `CailMmioReadResolved=1`；`CailUvdStatusBefore=0`、`CailUvdStatusAfter=0`；
  `CailInitCalls=6`，全部在 `_Cail_MCILWaitFor` 的 0x83AF bit1 等待上失败。
- `HardwareInitDec=0`、`StartDec=0`（预期，安全）；`HardwareSkipFirmware=1`、`ProbeSkipFirmware=1`、
  `LoadUVDFW=1`、`ProbeFirmware=1`。
- 用户态仍然是好的：`VCNUserApplied=16`、`VCNDecoderPID=1534`、`NewCtxStatus=0`、`UvdCtxInit=1`。

`UVD_STATUS` 前后都是 0（不是「忙」，是整个块没起来）。结合原生流程：

- `AMDUVD7HWEngine::init` 在 Raven 上把 `+0x25D` 设为 1（`HardwareSkipFirmware=1`），
  原生 `initHardware` 因此**跳过** `loadUVDFW`，只调 `initHardwareCail(+0x25D&1 = 1)`。
- 固件只在 `probeHardwareInit` 的**事后**诊断里单独 load 一次（`ProbeFirmware=1`），此时 CAIL
  init 已经跑完，UVD 块仍未启动。
- Linux `vcn_v1_0_wait_for_idle` 等的是 `mmUVD_STATUS == 0x2`；读到 0 说明 VCN 电源域/时钟/
  复位没被打开。CAIL 的寄存器初始化本身不负责上电。

诊断构建（UUID `898C4F45`，本轮）：第一次解码引擎 `initHardware` 时临时把 `+0x25D` 清 0，
让原生流程先执行 `loadUVDFW`，再以 `flag=0` 调 `initHardwareCail`；调用后恢复原值。
同时加安全闸：`initHardwareCail` 即使成功也强制返回 false（除非 boot-arg `-NRedVCNAllowStart`），
保证本轮不会 start 引擎、不会再向 UVDDec 提交。新增 `CailFirmwareFirst`、`CailStartBlocked`。

判读下一轮：

- `CailFirmwareFirst=1`、`LoadUVDFW=1`、`CailUvdStatusAfter=0x2`、`CailInitResult=0` →
  固件先行顺序正确，去掉/放开 no-start 闸后即可尝试真正的引擎 start 与解码提交。
- `LoadUVDFW=0` → boot 时的 SML 队列还不能下载固件，需要把加载移到 probe（在 init 之前）。
- `After` 仍为 0 → 固件载入不足以上电 UVD，需要补 Raven 的 VCN 电源域/时钟（PhwRaven PowerUpVCN 路径）。

## 19. 第十六轮：固件先行未触发/未验证，probe 改为先 load 再 init

10:08 dump（UUID `898C4F45`）仍然稳定，但读数需要小心解释：

- `CailFirmwareFirst=0` 是**最后一次** initHardware 写入的值；第一次是否触发过无法从 ioreg 判断。
- `CailUvdStatusBefore/After=0`、`CailInitCalls=5`、`CailInitResult=1`，`CailStartBlocked` 缺失
  （说明 `initHardwareCail` 从未成功过）。
- `LoadUVDFW=1` 是最后一次写入，无法区分 boot 时固件先行 load 与 probe 事后 load 的结果。
- 用户态仍正常（`VCNUserApplied=16`、`VCNDecoderPID=1779`）。

关键疑点：如果 boot 时的 `loadUVDFW` 失败，原生 `initHardware` 会在调用 `initHardwareCail` 之前
直接返回 false；probe 再在**事后** load 成功，但不会重新跑 init。这样「固件先行 → CAIL」的顺序
从未被真正验证。

本轮诊断（UUID `1BDAD9EC`）：

- `probeHardwareInit` 改为**先** `loadUVDFW`（已知在 probe 时机能成功），再临时清 `+0x25D` 调
  `initHardware`（内部会再次 load，大概率命中 `+0x3BA` 缓存，然后以 flag=0 走 `initHardwareCail`），
  调用后恢复；no-start 闸保持不变。
- 新增 `LoadUVDFWCalls`/`LoadUVDFWFirst`/`LoadUVDFWSecond`、`CailFirmwareFirstEver`、
  `CailUvdStatusFirstBefore/After`、`ProbeFirmwareFirst`，避免再被「最后一次写入」误导。

判读：

- `ProbeFirmwareFirst=1`、`LoadUVDFWFirst/Second=1`、`UvdStatusAfter=0x2`、`CailInitResult=0` →
  固件先行顺序正确，下一版放开 no-start 闸尝试真正的 start/解码。
- `LoadUVDFWFirst=0` → boot 时 SML 队列还不能下载固件；以 probe 的第二次 load 为准。
- `After` 仍为 0（即使 load 成功）→ 固件载入不足以上电 UVD，转 Raven VCN 电源域
  （`PhwRaven_PowerUpVCN` / `_PHM_PowerGateVCN`）。

## 20. 第十七轮：固件加载全部成功，UVD 仍然没上电；补 powerOnOff

10:22 dump（UUID `1BDAD9EC`）给出了决定性结果：

- `CailFirmwareFirstEver=1`、`ProbeFirmwareFirst=1`、`LoadUVDFWCalls=3`，
  `LoadUVDFWFirst=1`、`LoadUVDFWSecond=1` —— boot 的固件先行与 probe 的先行加载**都成功**。
- 但 `CailUvdStatusFirstBefore/After=0`、`CailUvdStatusBefore/After=0`，`CailInitResult=1`，
  `CailStartBlocked` 缺失，`HardwareInitDec=0`、`StartDec=0`。
- 系统稳定，用户态仍正常（`VCNUserApplied=16`、`VCNDecoderPID=768`）。

结论：固件加载不是瓶颈。**VCN 电源域/时钟从未被打开**，UVD_STATUS 始终是 0；CAIL 的寄存器
初始化本身不负责上电。

关键线索：dump 里从未出现 `PowerDec`/`PowerEnc`，说明 accelerator 从未调用引擎的 `powerOnOff`。
反汇编 `AMDUVDHWEngine::powerOnOff`（符号 0x43D1538，实际函数入口 0x43D152A）只有 6 条有效指令：

```
mov rdi, [self+0x18]        ; SML 接口
call [rax+0x2b8]            ; 取 CAIL 客户端
mov esi, 3
xor edx, edx
xor ecx, ecx
call [client_vt+0xc8]       ; 发送 CAIL selector 3
sete al
```

它完全忽略 `on` 参数，固定发送 selector 3（很可能是 UVD 电源/时钟消息）。我们的自定义引擎没有
被 accelerator 纳入电源流程，所以这条消息从未发出。

诊断构建（UUID `B4419BB1`，本轮）：在 `uvd7InitHardware` 里、原始 initHardware 之前，对解码引擎
手动调用一次 `enginePowerOriginal(self, true)`，发布 `PowerHooked`/`PowerForced`/`PowerDec`。
no-start 闸保持不变。

判读：

- `PowerForced=1`、`CailUvdStatusAfter=0x2`、`CailInitResult=0` → 就是缺这条电源消息；
  下一版放开 no-start 闸试真正的 start/解码。
- `PowerForced=0` 或 status 仍为 0 → selector 3 不是电源消息/不足以上电，转 Raven 电源域
  （`PhwRaven_PowerUpVCN` / `_PHM_PowerGateVCN` 或 PhwRaven 的 SMU message 2 路径）。

## 21. 第十八轮：selector 3 返回失败，转 PHM/SMU message 2

11-08 11:0x dump（UUID `B4419BB1`）给出明确结果：

- `PowerHooked=1`、`PowerForced=0`、`PowerDec=2`：手动 `enginePowerOriginal(self, true)`
  返回 **false**，selector 3 没有把 UVD 打开。
- `CailUvdStatusBefore/After=0`、`CailUvdStatusFirstBefore/After=0`、`CailInitResult=1`、
  `InitHardwareCail=0`、`HardwareInitDec=0`、`StartDec=0`。
- 固件与用户态读数不变：`LoadUVDFWFirst/Second=1`、`VCNUserApplied=16`、`DecoderPID=804`，
  系统稳定，无冻结。

反汇编补充：

- `AMDHardware::powerUpHWEngines`（0x43CA39E）遍历 `[self+0x3B8+i*8]` 的 11 个引擎，逐个调用
  `[engine_vt+0x138]`（即 `powerOnOff`）。我们的自定义引擎不在这个数组里（或注入晚于该调用），
  所以 accelerator 从未对 VCN 引擎发 selector 3。
- 真正的 VCN 电源域入口在 HWLibs：`PhwRaven_PowerUpVCN`（0x47BB071）。
  - 读 `[phw+0xA0]` 的 ASIC 状态；若 `[state+0x190]==0`，直接返回 1（认为已上电）。
  - 否则 `Raven_SendMsgToSmc(smc, 2)`（**SMU message 2 = PowerUpVcn**），再
    `Raven_ReadArgFromSmc`；返回值 < `0x1E080` 视为失败（返回 2）。
  - 成功后置 `[state+0x190]=0`，再发 `Raven_SendMsgToSmcWithParameter(smc, 0xC, ...)`；
    `[state+0x1A0]==0` 时还写 SOC15 寄存器并等 0xC1。
- `PhwRaven_PowerGateVCN(phw, gate)`：`gate!=0` → `PowerDownVCN`，`gate==0` → `PowerUpVCN`。
- `PHM_Initialize(pp, cfg)`（0x467ECFD）在 `pp+0x960` 分配并初始化 0x1AC0 字节的 PHM
  扩展块；`+0x88` = SMC 接口、`+0x90` = 第二个服务指针、`+0xA0` = ASIC 电源状态。
  这个扩展块就是所有 `PhwRaven_*` 函数的 `phw` 参数。
- 驱动自己的 `TF_PhwRaven_InitPowerGateState(phw)` 会先置 `[state+0x190]=1`，只有在
  `[state+0x1A0]==0` 时才调用 `PowerUpVCN`。Picasso 上很可能是 `[0x1A0]!=0` 导致这一步被跳过，
  VCN 域从未上电——与 UVD_STATUS 恒为 0 完全吻合。

本轮构建（build35，CDHash `0E74697C`）：

- 在 HWLibs 路由 `_PHM_Initialize`：进入时从 `pp+0x960` 捕获 PHM 扩展块；原始调用返回后读
  `[state+0x190/0x1A0/0x194/0x1C4]`，若 `[0x190]!=0` 则强制置 1 并直接调用
  `PhwRaven_PowerUpVCN(phm)`，发布 `RavenPowerUpResult`。
- 路由 `_TF_PhwRaven_InitPowerGateState`：记录驱动自身是否调用、返回值以及 `0x190` 的前后值。
- `uvd7InitHardware` 在 engine selector 3 之前若仍看到 `[0x190]!=0`，补一次 `PowerUpVCN`
  （`RavenPowerUpRetry`）。
- 保留 engine powerOnOff 强制调用和 no-start 闸。

判读：

- `PhmInitHooked=1`、`PhmInitCalled=1`、`PhmInitResult=1`、`RavenPowerUpResult=1`、
  `PhmState190After=0`、`CailUvdStatusBefore=0x2` → 电源域打通，CAIL init 应成功；
  下一版放开 no-start 闸，试真正的 start/解码。
- `RavenPowerUpResult=2` → SMU 拒绝 message 2（参数 < 0x1E080 或发送失败）；下一步 hook
  `_Raven_SendMsgToSmc` / `_Raven_ReadArgFromSmc` 读真实响应。
- `PhmInitHooked=0` → 符号未导出或路由失败；改从 `_PSM_ReArbit` 或 `AMDHardware` 对象字段抓 PHM。
- `PhmState1A0=1` 且 `InitPowerGate190After=1` → 驱动确实跳过了 PowerUpVCN，强制调用方向正确。
- `PhmState190After` 仍为 1 → PowerUpVCN 失败，看 `RavenPowerUpResult`。

## 22. 第十九轮：强制 PowerUpVCN 触发 panic，改为只读观测

build35（CDHash `0E74697C`）实机连续两次 panic：

- 11:11:53，启动后 43.1s，CPU 1/2/6/7 报 `NMIPI for unresponsive processor:
  TLB flush timeout`；CPU 7 的 RIP 落在 `AMDRadeonX6000Framebuffer`
  （0xffffff7f9d2d1abf），栈里还有 GPU 驱动多帧。
- 11:20:15，启动后 40.5s，同样的 TLB flush timeout，随后 `IPI timeout,
  unresponsive CPU bitmap: 0x1`。
- 两份 panic 都没有 NootedRed 栈帧，但时间点与 build35 在 `PHM_Initialize` 返回后
  强制调用 `PhwRaven_PowerUpVCN`（SMU message 2 + SOC15 寄存器写）完全吻合；40 秒
  左右正是登录后 WindowServer/显示电源仲裁开始工作的时机。**build35 不安全，不要安装。**

当前启动的 NootedRed 是 VCN 被拒绝的构建（`NRedVCNRejected=5`、
`NRedVCNUserRejected=5`、`NRedVCNUserEnabled=0`），没有 VCN1 探测，系统稳定；
这也解释了为什么当前 ioreg 里没有 `NRedVCN1*` 属性。

补充发现：`[state+0x1A0]` 不是固件能力位，而是 `PhwRaven_Initialize` 从注册表键
**`PP_EnableVCNPG`** 读出的覆盖值（`PECI_ReadRegistry`，0x47B72C7–0x47B72E6）。
`PP_EnableVCNPG==1` 表示启用 VCN 电源门控，`TF_PhwRaven_InitPowerGateState` 会跳过
初始化时的 `PowerUpVCN`；0 则让驱动自己在 init 任务里上电。这提供了一个比手动调
SMU 更安全的入口：让驱动自己的 TF 走 `PowerUpVCN` 分支，而不是从 PHM 钩子里强行调用。

build36（commit `428430a`，只读观测，不写 SMU/寄存器）：

- 只路由 `_TF_PhwRaven_InitPowerGateState`：捕获 `phm`，记录 `[state+0x190]` 与
  `[state+0x1A0]` 的前后值和函数返回值；原始函数照常执行。
- **不**路由 `_PHM_Initialize`，**不**调用 `PhwRaven_PowerUpVCN`，不写任何寄存器。
- `uvd7InitHardware` 只读 `PhmState190AtHardware`。
- 保留 B4419BB1 的 engine `powerOnOff(true)` 诊断和 no-start 闸。

判读：

- `InitPowerGateSeen=1`、`InitPowerGate1A0Before=1`、`InitPowerGate190After=1`
  → 驱动因 `PP_EnableVCNPG=1` 跳过上电；下一版在 TF hook 里**只**把 `[0x1A0]` 临时
  清 0，让驱动自己的 TF 任务调用 `PowerUpVCN`（正确时机/锁上下文），而不是从 PHM 钩子
  强行发 SMU。若仍 panic，再考虑把 `PP_EnableVCNPG` 作为设备属性注入为 0。
- `InitPowerGateSeen=1`、`InitPowerGate190After=0` → 驱动自己已经上电；之前
  UVD_STATUS=0 的原因在别处（时钟/复位或 CAIL 读取路径），查 PSM/时钟。
- `InitPowerGateSeen` 缺失 → TF 未被调用（PHM 初始化走了别的 ASIC 路径），需要另找
  入口（`PhwRaven_Initialize` 或 `PSM_ReArbit`）。
