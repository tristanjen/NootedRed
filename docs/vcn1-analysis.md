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
- `initHardware`（0x43D527A）：`vt[+0x338]()`→固件就绪位（存 +0x25D）；
  `vt[+0x390](flag&1)`；随后对 iface `vt[+0x128](base+0x500, 1/2)` 两次
  （中断/事件路由，base=引擎+0x250）。
- 同伴类：`UVD7v2Dec/EncRing`、`UVD7v2Dec/EncChannel`、`UVD7v2EncLLQRing/Channel`；
  `AMDAccelUVDContext`（IOConnect 0x104/0x106 的内核侧：requestCapability、
  saveUVDSession/restoreUVDSession、setClocks、PM client）；
  `AMDVega10VideoContext`、`AMDAccelVideoContext`、`AMDGFX9GraphicsAccelerator::newVideoContext`。

### 与 fork 现状的关系

- fork 的 VCN2 方案：可选加载 X6000，把 `AMDVCN2HWEngine` 嫁接进 X5000 的
  `kAMDHWEngineTypeVCN0`(8) 槽 + 跨驱动类桥接（safeCast/ClientStart/SurfaceCopy/SML）。
- **VCN1 方案无需跨驱动**：全部类原生在 X5000。要点：
  1. `X5000::allocateHWEngines` 为 VCN1 设备加配两个 `AMDUVD7v2HWEngine`
     （槽 5/6，即 `pm4EngineField+5*8`、`+6*8`），并 `hasUVD0Field=true`
     （+0x90；必要时 UVD1 对应位）——**不是** hasVCN0；
  2. X5000 原生 `newVideoContext`/AccelShared/AccelUVDContext 路径保持原生；
  3. HWLibs/SML 查询 0x0C 需对 APU 返回 Raven VCN 基址（fork 已 hook
     `IAMDSMLInterface::createSMLInterface`，注入点现成；具体 SML 方法 0x58 的
     实现在 X5000HWLibs 切片待解）；
  4. 固件已就绪：`ativvaxy_rv.dat`（AMD Adrenaline 26.5.2，VCN1，ipVersion 0x0100）
     已打包并经 `wrapGetIpFw` 按名分发；Linux raven/picasso/raven2 VCN 固件三同源。

## 3. 已核实/待办

- 已核实：系统 15.8.1 与作者档案逐函数匹配；`NRedVCNRejected=5` 基线；显示链路全绿。
- 待办（内核）：SML `+0x58`(0x0C) 的实现与 APU 基址注入；ring 命令寄存器偏移
  与 Raven 对照；GFXOFF/时钟（AccelUVDContext::setClocks→SMU11）；
  `AMDGFX9GraphicsAccelerator::newVideoContext` 的上下文选择；
  kernel ABI 守卫（参照 `build_vcn_kernel_patch.py` 为 UVD7v2 相关 vtable 生成）。
- 待办（用户态）：`vcn_capabilities.py` 允许 VCN1 分类（15D8→family 4）；
  生成 `UserSequoiaVideoData` 的 15D8 identify 载荷；FairPlay 解码子集沿用；
  编码相关补丁（Vcn2 系）不进入 15D8 条目。
