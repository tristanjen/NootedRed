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

## 23. 第二十轮：build36 只读观测确认驱动主动跳过 VCN 上电

11-08 11:3x dump（build36，CDHash `38DA3C47`）稳定，关键读数：

- `InitPowerGateSeen=1`、`InitPowerGateResult=1`、`InitPowerGate190Before=1`、
  `InitPowerGate190After=1`、`InitPowerGate1A0Before=1`、`InitPowerGate1A0After=1`。
- `PhmHandlePresent=1`、`PhmState190AtHardware=1`。
- `CailUvdStatusBefore/After=0`、`CailInitResult=1`、`InitHardwareCail=0`。
- `PowerForced=0`、`PowerDec=2`（engine selector 3 仍然失败）。

结论：`TF_PhwRaven_InitPowerGateState` 确实在启动时运行，但 `[state+0x1A0]==1` 让它
**主动跳过** `PowerUpVCN`；`[0x190]` 前后都是 1，VCN 一直处于门控状态。这不是失败路径，
而是驱动的正常策略（`PP_EnableVCNPG` 开启时的初始化行为）。

**修正**：实际读取的注册表键名是 **`CAIL_PP_EnableVCNPG`**，不是 `PP_EnableVCNPG`。
`AMDHardware::MCIL_GetRegistrykey`（0x43CC702）会在键名前拼 `CAIL_`，再对 `[self+0x10]`
（`IOPCIDevice`，`AMDHardware::init` 0x43C92C4 写入）调用 `getProperty`。
`getOSObjectData` 同时支持 OSData/OSNumber，因此 DeviceProperties 里用 `<integer>0</integer>`
或 4 字节 data 都可以。

下一步（build37）：属性不是来自 OpenCore DeviceProperties，而是 NootedRed 的
`Personalities/com.apple.kext.AMDRadeonX5000.xml` 经 `DriverInjector`（`IOCatalogue::addDrivers`
包装）合并进 AMD 驱动的 personality。已在该文件 **0x15D8** personality 的 `cail_properties`
里加入 `CAIL_PP_EnableVCNPG = 0`（和已有的 `CAIL_DisableGfxCGPowerGating = 1` 同一字典）；
build37（CDHash `A73797F8`）已把新 XML 嵌入二进制。安装后预期：

- `InitPowerGate1A0Before=0`、`InitPowerGate190After=0`；
- 驱动自己的 TF 调用 `PowerUpVCN`，`CailUvdStatusBefore=0x2`；
- `CailInitResult=0`（如果 CAIL 初始化随后成功）。
- 如果 `1A0Before` 仍为 1，说明属性没落在 `IGPU@0` 上或读取路径不同；退回在 TF hook 里
  临时清 `[0x1A0]`。

### build37 实测（11-08 12:0x dump）

`CAIL_PP_EnableVCNPG=0` 已经出现在 `IGPU@0` 的 ioreg 属性里（顶层和
`cail_properties` 字典都能看到），但驱动仍然读到 `[state+0x1A0]=1`：
`InitPowerGate1A0Before/After=1`、`InitPowerGate190After=1`、
`PhmState190AtHardware=1`、`CailUvdStatusBefore/After=0`、`CailInitResult=1`。
系统稳定。说明属性注入成功，但 `PhwRaven_Initialize` 读取的时机/对象与 IGPU@0 上这份
属性不一致（可能读发生在 `setupCAIL` 展平 `cail_properties` 之前，或读的是另一个
IOPCIDevice 实例）。

### build38（commit `e870420`，CDHash `6BAB212F`）：在驱动自己的 TF 任务里清 `[0x1A0]`

`initPowerGateHook` 在调用原始 `TF_PhwRaven_InitPowerGateState` 之前，把
`[state+0x1A0]` 临时清 0，让驱动自己的 TF 走完整的 `PowerUpVCN` 分支（message 2 +
message 0xC + SOC15 寄存器写/等待）；调用后恢复原值。新增
`InitPowerGate1A0Cleared`/`Restored` 探针。传 `-NRedVCNNoForcePG` 可跳过这次写入
（安全回退）。

## 24. 第二十一轮：build38 上电成功但被 PSM 重新门控；build39 留在 0

build38（CDHash `6BAB212F`）dump：

- `InitPowerGate1A0Cleared=1`、`Restored=1`、`1A0Before=1`、`1A0After=0`、
  `190Before=1`、`190After=0`、`Result=1`——驱动自己的 TF 完整 `PowerUpVCN` 成功。
- 但 `PhmState190AtHardware=1`：到引擎 `initHardware` 时 PSM 已把 VCN 重新门控。
- `CailUvdStatusBefore/After=0`、`CailInitResult=1`；系统稳定。

原因：build38 在 TF 返回后把 `[0x1A0]` 恢复为 1。`PhwRaven_PowerDownVCN` 在
`[0x1A0]!=0` 时才会执行消息 0xB 并置 `[0x190]=1`；PSM 的 `PowerGateVCN(phm,1)`
因此在 engine init 前把 VCN 关掉。

build39（CDHash `EB3848FB`）：默认**不恢复** `[0x1A0]`，留在 0。这样
`PhwRaven_PowerDownVCN` 开头的 `cmp [0x1A0],0; je return 1` 直接跳过，VCN 保持
`[0x190]=0`。新增 `InitPowerGate1A0LeftZero` 探针；`-NRedVCNRestorePG` 可恢复 build38
行为。预期 `PhmState190AtHardware=0`、`CailUvdStatusBefore=0x2`、`CailInitResult=0`。

## 25. 第二十二轮：build39 在 probe 的 CAIL idle 等待中 panic

build39（UUID `9CFD1B1F`）实机 panic（`Kernel-2026-11-08-203225.panic`）：

- CPU 1 卡在 `_ulReadMmRegisterUlong+0x30`（读 0x83AF），上一层
  `_uvd_init_engine+0x60f`（`_Cail_MCILWaitFor` 等 bit1），再上 `_CailUvdInit+0x90`
  → `AmdCailServices::uvdControl+0x3c9`。
- NootedRed 栈（atos）：`cailSubmit` → `uvd7InitHardwareCail` → `uvd7InitHardware` →
  **`probeHardwareInit`** → `accelVideoContextStart` → `videoContextStart`。
- 触发进程：`avconferenced`（视频会议守护进程启动视频上下文，触发 probe）。
- 结论：build39 让 VCN 保持上电后，probe 里的 `_CailUvdInit` 真正进入 `_udv_init_engine`
  的 0x83AF idle 等待；但块虽然上电，PSM/时钟没有把它带到 idle（UVD_STATUS 仍为 0），
  等待变成自旋，最终 TLB flush 超时 panic。
- **build39 不安全**；EFI 已回滚到 stock 构建（UUID `30579C7E`），系统稳定。

方向调整：不要再从 PHM 强行上电。正确路径是让 PSM 把 VCN 当作活动 block 激活
（`PSM_UseAsicBlock` / `PSM_ActivateClient` / `PSM_ReArbit` → `PHM_PowerGateVCN(phm,0)`），
由 PSM 同时设置 VCN 时钟/DPG；否则 CAIL 的 0x83AF idle 等待永远不会满足。probe 也必须
默认关闭（`-NRedVCNProbe` 才启用），避免视频上下文启动时提前跑 CAIL init。

## 26. 第二十三轮：关闭 probe，观察 accelerator 的原生 powerOnOff/CAIL 函数

build39 panic 后回滚 stock；build40（CDHash `C29F2533`）：

- `probeHardwareInit` 默认关闭（`-NRedVCNProbe` 才启用），避免视频上下文启动时提前跑
  CAIL init；build39 的 panic 栈正是这条路径。
- `initPowerGateHook` 恢复只读：不写 `[0x1A0]`、不提前上电。
- `uvd7InitHardware` 不再默认强制 `enginePowerOriginal(self,true)`，只有
  `-NRedVCNForcePower` 才调用；新增 `PowerOnOffCalls`/`LastOn`/`LastResult`。
- `cailUvdControlHook` 新增 `CailFunc1..5Calls/Result`，用来区分 init/start/power。

判读：

- `PowerOnOffCalls=0` → accelerator 没调用引擎 `powerOnOff`（引擎数组/时序问题）。
- `PowerOnOffCalls>0`、`LastOn=1`、`LastResult=0` → 调用了但 CAIL func 3 失败。
- `PowerOnOffCalls>0`、`LastResult=1` → 原生上电成功；VCN 可能已上电，CAIL init 能否
  通过看 `CailInitResult`/`CailUvdStatusBefore`。
- 系统应稳定（probe 关闭、无强制写）。

## 27. 第二十四轮：build40 dump 与 build41 的 PECI 兜底

build40（CDHash `C29F2533`）实机 dump（2026-11-08 12:56）：

- `PowerOnOffCalls` **不出现**：反汇编 `__ZTV30AMDRadeonX5000_AMDUVD7HWEngine` 槽
  +0x138 指向 0x43d2b24，是 `mov al,1; ret` 的空桩。UVD7 覆盖了基类的
  `AMDUVDHWEngine::powerOnOff`（0x43d1538），所以钩子永远不会触发；accelerator 的
  `powerUpHWEngines` 调到的就是「直接返回 true」，不产生任何 CAIL 消息。
- `CailFunc1Calls=4`、`CailFunc1Result=1`；`CailFunc2..5` 从未调用。
  `CailLastSelector=1`、`CailLastResult=0xFFFFFFFFE00002C7`（即 `0xE00002C7`
  符号扩展），`CailSubmit8=0`、`CailSubmit9=0xFFFFFFFFE00002C7`。
- `InitPowerGate1A0Before/After=1`、`InitPowerGate190Before/After=1`、
  `PhmState190AtHardware=1`、`InitHardwareCail=0`、`HardwareInitDec=0`、`StartDec=0`、
  `CailUvdStatusBefore/After=0`。
- 结论：engine init 期间 UVD 块完全没上电，CAIL init 以返回 1 收场；accelerator 侧
  不会（也不需要）通过 UVD7 的 powerOnOff 上电。

属性链路核实：`CAIL_PP_EnableVCNPG=0` 确实以**扁平属性**出现在 IGPU（IOPCIDevice）
节点上（`ioreg -a` 结构确认），但 `PhwRaven_Initialize` 里的
`PECI_ReadRegistry(handle, "PP_EnableVCNPG", &out, 1)` 仍然走了「未找到 → 写默认值 1」
分支，于是 `[state+0x1A0]=1`。`MCIL_GetRegistrykey` 会拼 `CAIL_` 前缀后调用
`[hardware+0x10]`（IOPCIDevice）的虚表 +0x2B8；失败原因可能是转换/时机，但不必再猜。

build41（CDHash `37DF0FD4`，commit `a5a2085`）：

- 路由 `_PECI_ReadRegistry`，key 为 `PP_EnableVCNPG` 时把输出强制写 0 并返回 0。
  这样 `PhwRaven_Initialize` 得到 `[state+0x1A0]=0`，`TF_PhwRaven_InitPowerGateState`
  在引擎 init 前走 `PhwRaven_PowerUpVCN`；之后 `PhwRaven_PowerDownVCN` 因
  `[0x1A0]==0` 直接返回，VCN 保持上电。
- 观测：`PeciVCNPGCalls/Raw/Result/Forced`、`InitPowerGateCalls`、
  `RavenPowerUpCalls/Result/190Before/190After/1A0`、
  `RavenPowerDownCalls/Result/190Before/190After`、
  `PhmPowerGateVCNCalls/LastOn/Result/190Before/190After`、
  `InitHardwareCailRaw`（no-start 闸之前的真实结果）。
- probe 仍默认关闭，不强制 powerOnOff；预期系统稳定。

## 28. 第二十五轮：build41 冻屏/panic，build42 回到安全默认并观测 PSM

build41（CDHash `37DF0FD4`，UUID `90A2B51D`，commit `a5a2085`）实机：

- 13:07 安装，13:14:53 冻屏后 panic，`System uptime` 45.6s。
- panic 类型：`NMIPI for spinlock acquisition timeout`；CPU 4 的 fseventsd 线程在
  VFS 路径（`read` → ... → `thread_block_reason`）上持有 spinlock 阻塞，其余 CPU
  超时；栈里没有 NootedRed / AMD 帧。
- 时间点（40–46s）与 build35（强制 PowerUpVCN、VCN 持续上电后 TLB flush 超时）
  属于同一类；build41 是该次启动唯一增量。**判定 build41 不安全，EFI 回滚到 build40**
  （当前实机 kextstat UUID `FEB0F272`）。

根因链：PECI 强制 `PP_EnableVCNPG=0` → `PhwRaven_Initialize` 写 `[state+0x1A0]=0`
→ `TF_PhwRaven_InitPowerGateState` 调 `PowerUpVCN`（VCN 上电）
→ 之后所有 `PhwRaven_PowerDownVCN` 因 `[0x1A0]==0` 直接返回
→ VCN 在 PSM 之外持续上电，PSM 内部状态与真实电源域不一致。

build42（CDHash `955FE564`，UUID `00A6452E`，commit `eb260d6`）：

- PECI 强制改为 `-NRedVCNForceVCNPG0` 门控，默认放行原值；只保留
  `PeciVCNPGCalls/Raw/Result` 观测（回答 build37 的属性为何读不到）。
- 默认行为回到 build40：不写 `[0x1A0]`/`[0x190]`、不调 PowerUp/DownVCN、
  probe 关闭、不强制 powerOnOff。
- 新增只读观测：
  - `_PSM_UseAsicBlock`（block 2/3/6 是 PEM 视频启动任务使用的媒体 block）
    → `PsmUseAsicBlockCalls/LastBlock/LastResult/BlockMask`；
  - `_PSM_ReArbit`（Raven 上唯一调用 `PHM_PowerGateVCN` 的入口）
    → `PsmReArbitCalls/LastReason/LastResult/Flag5F0/Flag690/GateDelta`；
  - `_PHM_PowerGateUVD` → `PhmPowerGateUVDCalls/LastOn/Result/190Before/190After`；
  - `UnsafePoweredAtInit` = `[state+0x190]==0`（引擎 init 时 VCN 仍上电）→ 1。
- 新增静态事实：`PSM_UseAsicBlock` 的分发目标 `_PhwRaven_SetAsicBlockGating`
  （0x47bab44）是 `mov eax,1; ret` 空桩，所以仅看 `PSM_UseAsicBlock` 不能判断媒体
  block 是否被真正上电；必须同时看 `PSM_ReArbit` 与 PHM 门控计数。
- 判据：默认路径应为 `InitPowerGate1A0Before=1`、`UnsafePoweredAtInit=0`。
  若播放时 `PsmUseAsicBlockBlockMask` 出现 bit 2/3/6、`PsmReArbitCalls>0` 且
  `PsmReArbitGateDelta>0`，或 `PhmPowerGateVCN/UVD` 的 `LastOn=0`，说明 PSM 自己会
  激活媒体 block，下一步在 PSM 上电后的安全时间点一次性重跑引擎 `initHardware`
  （保留 no-start 闸）；若 PSM 计数全 0，则改从视频上下文路径调用
  `PSM_UseAsicBlock`/`PSM_ActivateClient`，不再裸发 `PowerUpVCN`。

## 29. 第二十六轮：build42b 证实 PSM 只关不开，build43 做电源脉冲探针

build42b（CDHash `955FE564`，UUID `00A6452E`）实机 dump：

- 安全默认成立：`InitPowerGate1A0Before/After=1`、`PhmState190AtHardware=1`、
  `UnsafePoweredAtInit=0`、`PeciVCNPGCalls=1`、`Raw=1`、`Result=1`、`Forced` 不出现。
  这同时证实 build37 的谜底：驱动 `PECI_ReadRegistry("PP_EnableVCNPG")` 读到的是
  **默认值 1**，personality 的扁平属性没有被 `getProperty` 命中（或转换失败）。
- PSM 行为：`PsmReArbitCalls=7`、`PsmReArbitGateDelta=2`，
  `PhmPowerGateVCNCalls=7`/`LastOn=1`、`PhmPowerGateUVDCalls=7`/`LastOn=1`，
  `RavenPowerDownCalls=8` 全部空转（190 前后均为 1），`RavenPowerUpCalls` 不出现。
  `PsmUseAsicBlock*` 不出现（从未调用），`PsmReArbitFlag5F0=0`、`Flag690=0`。
- 结论：PSM 每轮仲裁都在关 VCN/UVD（on=1），两个“应开”标志
  `[psm+0x5F0]`/`[psm+0x690]` 始终为 0，视频栈没有向 PSM 申请媒体 block；
  引擎 init 失败 → 能力为 0 → VA 驱动不会发起真正的视频会话，形成死锁。

build43（CDHash `15BFC5D8`，UUID `028269CA`，commit `5777f9f`）：

- 在 `uvd7InitHardware` 原生 init 失败后做一次短促电源脉冲（只解码引擎、只一次）：
  临时清 `[0x1A0]` → `PhwRaven_PowerUpVCN`（走 1A0==0 的完整路径：SMU msg 2 + msg 0xC
  + `PhwRaven_GetSOC15RegisterOffset` 0xC0/0xC1 的 `PECI_WriteRegister/WaitOnRegister`）
  → 用 `_ulReadMmRegisterUlong(adapter, 0x83AF)` 最多读 3 次（间隔 1ms）→ 恢复 `[0x1A0]`
  → `PhwRaven_PowerDownVCN` 把 VCN 关回去。窗口几毫秒，**不跑 CAIL init**。
- 新增 `_PSM_ActivateClient`（client/args/result）与
  `_PEM_Task_VideoStartMultiUVDStates`（calls/result）观测，`PSM_ReArbit` 里补
  `PsmHandle`，用于判断视频栈是否真的向 PSM 注册过媒体客户端。
- 判据：`PowerProbeUpResult` 成功且 `190AfterUp=0`、`PowerProbeStatus` bit1=1
  → 上电后块是 idle 的，下一版可以在同一窗口里跑 `initHardwareCail`；
  `Status=0` → 还缺 PSM 的时钟/DPG 序列，需要查 `PhwRaven_SetDPGModeVCN` 与 msg 0xC。

## 30. 第二十七轮：build43 电源脉冲成立但 adapter 取错；build44 修正并在窗口内条件初始化

build43（CDHash `15BFC5D8`，UUID `028269CA`）实机 dump：

- 脉冲机制成立且系统稳定：`PowerProbeRun=1`、`PowerProbeUpResult=1`、
  `PowerProbe190AfterUp=0`（上电）、`PowerProbeDownResult=1`、
  `PowerProbe190AfterDown=1`（关回）、`PowerProbeDPG=0`。
- `PowerProbeAdapter=0`：adapter 取错了。CAIL client 不是 `[engine+0x18]`，而是它
  虚表 +0x2B8 的 getter 返回值；adapter 在 `client+0x20038`。因此 `PowerProbeStatus=0`
  只是没读，不代表块不 idle。
- PSM：`PsmActivateClientCalls=6`、`Last=3`、`Arg2=1`、`Arg3=0`、`Result=1`；
  `PemVideoStartCalls` 不出现 → 视频栈有客户端激活，但 PEM 的视频启动任务
  （`PEM_Task_VideoStartMultiUVDStates`）从未执行，PSM 的 VCN 开窗标志仍是 0。
  两个直接调用者是 `PEM_UpdateExclusiveWorkload`（client 0x1E）与
  `PEM_CWDDEPM_ActivateClient`（client 来自事件字段 +4，即这里的 3）。

build44（CDHash `A9936CCB`，UUID `D3B887A1`，commit `0e47a32`）：

- 探针改用 `cailClient`（或走 `[engine+0x18]` 虚表 +0x2B8 的 getter），从
  `client+0x20038` 取 adapter。
- 读 `UVD_STATUS(0x83AF)`（最多 3 次、间隔 1ms）；只有 bit1（0x2）置位时才在同一个
  电源窗口里调用原生 `initHardware`（`0x25D=1` → 跳过固件，只走 CAIL init），
  随后立刻恢复 `[0x1A0]` 并 `PowerDownVCN` 关回。
- 记录 `PSM_ActivateClient` 的调用者地址（client 3 单独记录）与第一次 `PSM_ReArbit`
  的调用者，便于用 `kextstat` 的 slide 反查驱动路径。
- 判据：`PowerProbeStatus` bit1=1 → 块是活的，看 `PowerProbeWindowInit` 与
  `CailInitResult`；若窗口内 init 成功，下一版去掉 no-start 闸验证 start。

## 31. 第二十八轮：build44 证明只上电不够；build45 把电源窗口覆盖到引擎 init

build44（CDHash `A9936CCB`，UUID `D3B887A1`）实机 dump（14:30）：

- `PowerProbeAdapter=1`（adapter 解析修好了）、`PowerProbeUpResult=1`、
  `PowerProbe190AfterUp=0`（VCN 上电）、`PowerProbeDownResult=1`、
  `PowerProbe190AfterDown=1`（关回），系统稳定。
- **`PowerProbeStatus=0`、`PowerProbeWindowInit` 不出现**：`PowerUpVCN` 能打开电源域，
  但 UVD 块不会 idle。结合首次 `initHardware` 是在 gated 状态下装载固件的，结论是：
  固件必须在 VCN 上电状态下装载/启动，0x83AF idle 位才会置起。
- PSM 侧不变：`PsmActivateClientCalls=6`/`Last=3`/`Result=1`，`PemVideoStartCalls`
  不出现，PSM 仍只关门不开门。调用者地址落在 KernelPatcher 的 trampoline 区
  （不在任何已加载 kext 范围内），`__builtin_return_address(0)` 经 route 钩子拿不到
  真实调用者，后续不再依赖它。

build45（CDHash `217B04E3`，UUID `44B5526C`，commit `16c6574`）：初始化电源窗口。

- `initPowerGateHook` 在第一次 TF 调用前把 `[state+0x1A0]` 清 0 并**保持**：TF 因此走
  完整 `PhwRaven_PowerUpVCN`（SMU msg 2 + msg 0xC + `PECI_WriteRegister` 0xC0/0xC1）。
- 窗口一直保持到解码引擎第一次 `initHardware`（`0x25D` 被清 0 → `loadUVDFW` 在 VCN
  上电状态下执行；随后 `initHardwareCail` 也在上电状态下执行）。
- `uvd7InitHardware` 返回后立即 `closeInitWindow`：恢复 `[0x1A0]`、调用
  `PhwRaven_PowerDownVCN` 关回，并发布 `InitWindowClosed/DownResult/190AfterDown`。
- 安全护栏：若 TF 到达时 `uvd7InitHardwareCount != 0`（引擎 init 已跑过），不开窗
  （`InitWindowSkipped=1`）；旧的电源脉冲探针改为 `-NRedVCNPowerProbe` 才运行。
- 判据：`InitWindowOpened=1`、`Closed=1`、`190AfterDown=1` 说明窗口闭合；
  `CailInitResult=0` + `InitHardwareCailRaw=1` 说明 CAIL init 首次在上电状态下成功，
  下一版把窗口延长到 `uvdEngineStart` 之后并去掉 no-start 闸。

## 32. 第二十九轮：build45 的 TF 窗口冻屏/panic；build46 改为自包含、默认关闭

build45（CDHash `217B04E3`，UUID `44B5526C`）实机：约 155.6 秒后冻屏并 panic
（`Kernel-2026-11-08-144526.panic`）：

- `NMIPI for spinlock acquisition timeout`，锁 `0xffffff80171b1d88`（内核数据）。
- CPU 6（锁持有者）的栈是定时器中断：`lapic_interrupt → rtclock_intr → timer_intr →
  timer_queue_expire_with_options → thread_timer_expire → thread_setrun`；被中断上下文是
  **VoodooI2C**（boot args 含 `-vi2c-force-polling`）。CPU 4（imagent）等同一把锁。
- panic 文件里 **没有 NootedRed / AMD 帧**（grep ChefKiss = 0）。
- 结论：不能把这次 panic 直接归因于 VCN；但 build45 的窗口在 TF 打开、只由解码引擎
  `uvd7InitHardware` 关闭，若 `uvd7Init` 失败或 `initHardware` 未执行就泄漏，[0x1A0]
  停在 0、VCN 在 PSM 外长期上电（build35/build41 的状态）。机制必须移除。

build46（CDHash `D8326E76`，UUID `85F706FB`，commit `240f2c1`）：

- `initPowerGateHook` 恢复只读；TF 窗口删除。
- 初始化电源窗口移到 `uvd7InitHardware` 内部，默认关闭，只有
  `-NRedVCNInitWindow` 才启用：保存 [0x1A0] → 清 0 → `PhwRaven_PowerUpVCN`（完整路径）→
  `loadUVDFW` + `initHardwareCail` → 无论结果如何立即恢复 [0x1A0] 并
  `PhwRaven_PowerDownVCN`。窗口只存在于这一次函数调用内。
- `accelVideoContextStart` 增加兜底：若窗口仍打开则强制 `closeInitWindow` 并发布
  `InitWindowForcedClosed=1`。
- 默认（无 boot-arg）与 build44 完全一致，保证稳定性；实验通过 OpenCore
  `NVRAM/Add/boot-args` 追加 `-NRedVCNInitWindow` 显式开启。

## 33. 第三十轮：build46 的窗口实验复现 build35 的 TLB flush panic

2026-11-08 15:01 把 `-NRedVCNInitWindow` 追加到 OpenCore `NVRAM/Add/boot-args`
（备份 `config.plist.bak-20261108-150146`），15:03 启动含 build46 的系统。启动后约
47.3 秒、登录进桌面几秒后冻屏并 panic（panic 文件在下次启动时写出：
`Kernel-2026-11-08-150645.panic`）：

- `Panic(CPU 0, time 47259910154): NMIPI for unresponsive processor: TLB flush timeout`；
  uptime `47260046800` ns ≈ 47.26 s。第二次 panic 是 `IPI timeout, unresponsive CPU
  bitmap: 0x1`，panicked task 为 `avconferenced`（登录时探测视频能力的进程）。
- RIP `0xffffff7fa14d1abf`；`AMDRadeonX6000Framebuffer` 载入范围
  `0xffffff7fa14b5000`–`0xffffff7fa173efff`，所以 kext 内偏移 = **`0x1cabf`**。
- 用当前系统的 `SystemKernelExtensions.kc` 导出
  `com.apple.kext.AMDRadeonX6000Framebuffer.kc.bin`（`__TEXT` 文件偏移
  `0xbfcc000`）后解析：`0x1cabf` = `__ZN32AMDRadeonX6000_AmdRegisterAccess17
  readValidateReg32Ej + 0x8b`（`readValidateReg32(unsigned int)+0x8b`）。
- 反汇编：`+0x1cabc` 是 `mov edx, dword ptr [rcx + rax]`（从
  `[object+0x38] + reg*4` 读寄存器），`+0x1cabf` 是随后的
  `mov rax, qword ptr [r14]`；即 CPU 卡在 **MMIO 寄存器读** 之后。
- **与 build35 的 panic 完全相同**：11:11:53 的 panic RIP 为
  `0xffffff7f9d2d1abf`，基址 `0xffffff7f9d2b5000`，kext 内偏移同样是 `0x1cabf`，
  uptime 43.1 s。两份 panic 都停在 `readValidateReg32+0x8b` 这一条指令上，
  说明窗口实验复现了 build35 的“VCN 上电/门控状态不正确 → GPU 寄存器读挂死”
  这一类失败，而不是 VoodooI2C/定时器问题。

实验没有拿到 ioreg dump：窗口很可能在 `uvd7InitHardwareOriginal(self)` 内的寄存器读
处挂住，函数没有返回，`closeInitWindow` 也就没有执行；这与“关窗后 VCN 仍上电”的
build35 状态在外部表现上一致。无论是“init 挂住导致窗口未关”还是“关窗失败”，
结论相同：**这个窗口不安全，不能重试，也不能默认打开**。

panic 后 EFI 已被恢复为 stock NootedRed（UUID `30579C7E`，二进制与
`NootedRed-stock-30579C7E-EFI-backup.kext` 相同，mtime 2026-11-01 11:13:54）；
`config.plist` 已从 `config.plist.bak-20261108-150146` 恢复，boot-args 中不再有
`-NRedVCNInitWindow`。当前系统以 stock kext 运行。

下一步不再从 NootedRed 裸调 `PhwRaven_PowerUpVCN`。优先方向是让 PSM 自己激活媒体
block（`PSM_UseAsicBlock`/`PSM_ActivateClient`/PEM 视频启动，或让 `PSM_ReArbit` 在
`[psm+0x5F0]`/`[0x690]` 置位后调用 `PHM_PowerGateVCN(phm,0)`），这样电源域、时钟与
DPG 由驱动自己的电源管理序列协调；另一条线是查清 `readValidateReg32` 卡住的具体
寄存器（`loadUVDFW` 的 SML/PSP 读还是 `initHardwareCail` 的 `UVD_STATUS(0x83AF)` 读），
再决定是改固件装载路径还是改电源时序。

## 34. 第三十一轮：build47 的 PSM 驱动唤醒（不裸调 PowerUpVCN）

build46 的窗口 panic 与 build35 同 RIP（`readValidateReg32+0x8b`）后，放弃“自己写
`[0x1A0]` 再调 `PowerUpVCN`”的路线。重新反汇编 X5000HWLibs 后确认 PSM 的真实路径：

- `PSM_UseAsicBlock(psm, block)` 只维护 `[psm + block*12 + 0x204]` 引用计数并调用
  `_PHM_SetAsicBlockGating`（Raven 是空桩），不负责 VCN 电源。
- `PSM_ReArbit` 在 `[psm+0x5F0]`/`[psm+0x690]`（VCN0/VCN1 目标状态）为 1 时调用
  `PHM_PowerGateVCN(phm, 0, 0/1)`，为 0 时调用 `(phm, 1, 0/1)`；真正上电/下电在这里。
- `PEM_Task_VideoStartMultiUVDStates` 的基本 UVD 分支（分类 `0x12`、block 3）顺序是
  `PSM_GetStateByClassification` → `PSM_SetUvdState` → `PSM_AdjustPowerState` →
  `PSM_UseAsicBlock`。`PSM_AdjustPowerState_Legacy` 是状态机，应用新 power state 时写
  VCN 目标状态，再由 ReArbit 执行。

build47（CDHash `DBBC2952`，UUID `E5F48284`）据此实现：

- 默认只解析 PSM 状态机符号并捕获 `psm` 句柄，不改变电源行为。
- `-NRedVCNPsmWake`（默认关）在 `Vega10VideoContext::contextStart` 成功且 UVD0 队列
  非空后复刻上面的基本 UVD 序列；失败立即 `UnuseAsicBlock(3)`，客户端 stop 时释放。
- 不写 `[0x1A0]`/`[0x190]`，不调用 `PhwRaven_PowerUpVCN/PowerDownVCN`。
- 完整设计、属性表与判读见 `docs/vcn1-psm-plan.md`。

## 35. 第三十二轮：build47 唤醒返回 0xF；build48 分类探测

2026-11-08 15:49 的 build47 `-NRedVCNPsmWake` 实测：

- `PsmWakeRun=1`、`PsmWakeSymbols=31`、`PsmWake190Before=1`、
  `PsmWakeFlag5F0/690Before=0`：触发点与安全前置都满足。
- **`PsmWakeGetStateResult=15`**（0xF）、`PsmWakeStateIndex=0`、`PsmWakeFailed=1`；
  `SetUvd/Adjust/UseBlock` 均未执行，PSM 没有上电，系统稳定运行。
- 0xF 是 `PSM_GetStateByClassification_Legacy` 的“状态未找到”：
  `PSM_SearchForState(psm, class, ...)` 遍历 `[psm+0x88]` 的状态链表，用
  `PSM_StateMatchesClassification` 比较 `[state+0x28]`/`[state+0x2C]` 的分类位；
  找不到时回退 `PSM_PSDT_EnumerateAllStates`，仍找不到才返回 0xF。
  因此 **当前 PSM 状态表没有分类 0x12 的状态**，不是调用顺序/参数问题。

静态补充：

- `PSM_ListPresentClassifications(psm, UInt32* out)` 的 wrapper 在 `0x46BCDA4`
  （`jmp [psm+0x6D8]`），Legacy 实现在 `0x46BE8A2`：清零 `*out` 后枚举全部状态，
  `PSM_ListPresentClassifications_Callback`（`0x46BF555`）把分类 0..0x16 的存在
  情况写成一个 24 位小端位图（bit N = 分类 N）。
- 分类 0x11..0x14 对应位图 bit 17..20（byte2 的 bit1..bit4）。

build48（CDHash `A5E04CEB`，UUID `A0F67623`）据此改为分类探测版：

- 唤醒前调用 `PSM_ListPresentClassifications` 发布 `PsmWakeClassesMask`；
- 依次探测 0x12→3、0x13→4、0x14→5、0x11→2，发布每个分类的 result/index；
- 取第一个存在的分类，调用对应 `PSM_SetUvd*State` → `PSM_AdjustPowerState` →
  `PSM_UseAsicBlock(block)`；
- 四个都不存在则 `PsmWakeNoState=1`，释放并停止，不写任何电源状态。
