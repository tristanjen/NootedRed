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
