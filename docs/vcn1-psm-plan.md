# VCN1 PSM 驱动唤醒方案（build47）

## 0. 背景与结论

build46 的 `-NRedVCNInitWindow` 实验在登录后约 47.3 秒复现了 build35 的
`NMIPI ... TLB flush timeout`：RIP 都是 `AMDRadeonX6000Framebuffer + 0x1cabf`
（`AmdRegisterAccess::readValidateReg32+0x8b`），panic 前一条是
`mov edx, [rcx+rax]`（MMIO 寄存器读）。这是“VCN 在 PSM 管辖之外上电 → GPU 寄存器
读挂死”的同一类失败，不是 VoodooI2C/定时器问题。

因此 build47 **不再**：

- 写 `[state+0x1A0]` / `[state+0x190]`；
- 直接调用 `PhwRaven_PowerUpVCN` / `PhwRaven_PowerDownVCN`；
- 使用 `-NRedVCNForceVCNPG0` 或任何强制 PECI 覆盖。

build47 只调用 PSM 自己的状态机入口，让 PSM 按驱动设计的顺序完成时钟/DPG/电源
门控，再让原有的解码引擎 `initHardware` 去等 `UVD_STATUS(0x83AF)`。

## 1. PSM 静态结论（X5000HWLibs 反汇编）

切片：`build/vcn/com_apple_kext_AMDRadeonX5000HWLibs.kc.bin`（当前系统
`SystemKernelExtensions.kc`，符号地址与运行时 slide 无关）。

### 1.1 `PSM_UseAsicBlock(psm, block)`

`0x46BD13E`：

- `[psm + block*12 + 0x204]` 是 block 引用计数；非 0 时只加一返回 1。
- block 2..5 另有互斥判断：`[psm+0x21C] + [psm+0x228] + [psm+0x234] + [psm+0x240] > 1`
  时直接返回 1（同时只允许一类媒体 block）。
- 首次使用时调用 `_PHM_IsSafeForAsicBlock(phm, block)`；不安全时置
  `[psm + block*12 + 0x208] = 1` 等待 `PSM_CompleteAsicBlockGating` 重试。
- 然后调用 `_PHM_SetAsicBlockGating(phm, block, 0)`；Raven 的实现
  `_PhwRaven_SetAsicBlockGating` 是 `mov eax,1; ret` 空桩，所以这一步不会真的给
  VCN 上电。
- 成功时清 `[+0x208]`，必要时 `_PECI_NotifyClockGatingChange`。

`PSM_UnuseAsicBlock(psm, block)`（`0x46BD28B`）减引用计数，归零时清状态并
`_PHM_SetAsicBlockGating(phm, block, 1)`。

结论：`PSM_UseAsicBlock` 只负责“登记 block 正在使用”，**不负责 VCN 电源**；
VCN 的实际上电在 `PSM_ReArbit`。

### 1.2 `PSM_ReArbit(psm, reason)` 的 VCN 分支

`0x46BB84E`，VCN 相关指令在 `+0x2BB` 之后：

```
cmp dword [psm+0x5F0], 1
jne skip0
  PHM_PowerGateVCN(phm, 0, 0)        ; on=0, VCN0
skip0:
PHM_IsVcn1Supported(phm)
je skip1
cmp dword [psm+0x690], 1
jne skip1
  PHM_PowerGateVCN(phm, 0, 1)        ; on=0, VCN1
skip1: ...
```

下电路径（`+0x3EE`）是同一组字段与 `on=1`：

```
cmp dword [psm+0x5F0], 0
jne skip2
  PHM_PowerGateVCN(phm, 1, 0)
cmp dword [psm+0x690], 0
jne skip2
  PHM_PowerGateVCN(phm, 1, 1)
```

即 `[psm+0x5F0]` / `[psm+0x690]` 是 VCN0/VCN1 的“目标电源状态”，由 PSM 状态机
在应用新的 power state 时写入；`PSM_ReArbit` 只负责执行。`PHM_PowerGateVCN` 再到
`PhwRaven_PowerGateVCN`：`on=0 → PowerUpVCN`，`on=1 → PowerDownVCN`。

在 `_PSM_`/`_PEM_`/`_PHM_` 前缀的全部函数里扫描，只有 `PSM_ReArbit` 直接读这两个
字段；写入发生在 PSM 的状态应用路径（`PSM_SetState`/`PSM_AdjustPowerState` 的状态
属性拷贝），不是单个字段赋值。因此**不要去写这两个字段**，只能让状态机自己算。

### 1.3 正常视频启动任务的基本 UVD 分支

`PEM_Task_VideoStartMultiUVDStates(task, event)`（`0x46C771A`）在 event 满足
`[event] & 0x40`、`[event+0x38] & 0x4` 时，按 event 的 HD/SD/MVC 位选择：

| 分类 | 状态设置 | block |
|---|---|---|
| `0x12`（基本 UVD） | `PSM_SetUvdState` | 3 |
| `0x13`（UVD HD） | `PSM_SetUvdHDState` | 4 |
| `0x14`（UVD SD） | `PSM_SetUvdSDState` | 5 |
| `0x11`（UVD MVC） | `PSM_SetUvdMVCState` | 2 |

每个分支的顺序都是：

```
PSM_GetStateByClassification(psm, class, 0, &stateIndex)
PSM_SetUvdXState(psm, stateIndex)
PSM_AdjustPowerState(psm, 0)
PSM_UseAsicBlock(psm, block)
```

`PSM_SetUvd*State` 是 `[psm+0x6E0]` 的间接调用；`PSM_InitializeFunctionPointer_Legacy`
把 `[psm+0x6E0]` 设为 `PSM_SetState_Legacy`，把 `[psm+0x740]` 设为
`PSM_AdjustPowerState_Legacy`。状态机在 `PSM_AdjustPowerState_Legacy` 里选择并应用
新的 power state，VCN 目标状态就在这一步写入 `[psm+0x5F0]/[0x690]`，随后
`PSM_CompleteAsicBlockGating` / 下一次 `PSM_ReArbit` 执行门控。

### 1.4 死锁

视频启动任务只有在真实视频会话启动后才由 PEM 调度；而真实会话需要解码引擎
`initHardware` 成功 → 需要 VCN 上电；而 PSM 只有在视频启动任务/状态机请求时才给
VCN 上电。build42b/43/44 的 dump 里 `PemVideoStartCalls` 不出现、
`PsmUseAsicBlock*` 不出现、`PsmReArbitFlag5F0/690=0`，正是这个环。

## 2. build47 设计

### 2.1 默认路径（无 boot-arg）

与 build46 默认一致，只观测，不改变电源：

- 保留已有的 PSM/PEM 钩子与属性。
- 新增 `PSM` 状态机符号解析（`solveSymbol`，不路由）：
  `_PSM_GetStateByClassification`、`_PSM_SetUvdState`、`_PSM_AdjustPowerState`、
  `_PSM_UnuseAsicBlock`、`_PSM_GetAsicBlockStatus`。
- 在 `PSM_ReArbit` / `PSM_ActivateClient` 钩子里捕获 `psm` 句柄（原子存储）。
- 在解码引擎 `uvd7InitHardware` 里，如果 PSM 唤醒曾持有 block，记录
  `PsmWakeConsumed=1`（仅观测）。
- 默认不调用任何 PSM 写 API。

### 2.2 可选唤醒（`-NRedVCNPsmWake`，默认关）

触发点：`Vega10VideoContext::contextStart` 原函数返回成功且 `+0x1240`（UVD0 队列）
非空之后。此时视频上下文已建立，解码引擎 `start`/`initHardware` 还没执行。

一次性序列（只调用 PSM 公开入口）：

```
if psm == null or phm == null or state == null: 跳过
if [state+0x190] == 0: 跳过（VCN 已在 PSM 外上电，处于危险状态）
if [psm+0x5F0] != 0 or [psm+0x690] != 0: 跳过（已经请求过）
GetStateByClassification(psm, 0x12, 0, &idx)
SetUvdState(psm, idx)
AdjustPowerState(psm, 0)
UseAsicBlock(psm, 3)
读 [psm+0x5F0]/[psm+0x690]/[state+0x190]
if 两个 flag 仍为 0:
    UnuseAsicBlock(psm, 3)        // 释放登记，不等 PSM 门控
    PsmWakeFailed=1，不再重试
else:
    PsmWakeAcquired=1             // 让原生的 initHardware 去跑
```

释放：

- 唤醒失败立即 `UnuseAsicBlock(3)`；
- 客户端 `AMDAccelSharedUserClient::stop` 时如果仍持有 block，`UnuseAsicBlock(3)`；
- 引擎 `initHardware` 执行时只置 `PsmWakeConsumed=1`，不主动释放；由 PSM 的
  客户端/block 生命周期在视频停止时回收。

安全约束：

- **不写** `[0x1A0]`/`[0x190]`/`[psm+0x5F0]`/`[psm+0x690]`；
- **不调用** `PhwRaven_PowerUpVCN/PowerDownVCN`；
- 只在 `[state+0x190] != 0`（门控安全）时执行；
- 只执行一次；失败即释放，不循环重试；
- 默认关，必须显式加 boot-arg。

### 2.3 新增属性

| 属性 | 含义 |
|---|---|
| `PsmWakeRun` | 第几次尝试（正常为 1） |
| `PsmWakeNoHandle` / `PsmWakeNoSymbol` | 前置条件缺失 |
| `PsmWakeSkipped` | 190/flag 前置条件不满足，未执行 |
| `PsmWakeFlag5F0Before/After` | PSM 的 VCN0 目标状态 |
| `PsmWakeFlag690Before/After` | PSM 的 VCN1 目标状态 |
| `PsmWake190Before/After` | `[phm+0xA0]` 的 VCN 门控状态 |
| `PsmWakeGetStateResult` / `PsmWakeStateIndex` | 取状态分类 0x12 的结果 |
| `PsmWakeSetUvdResult` / `PsmWakeAdjustResult` / `PsmWakeUseBlockResult` | 三个 PSM 调用的返回值 |
| `PsmWakeVcnGateDelta` | 唤醒前后 `PHM_PowerGateVCN` 调用次数差 |
| `PsmWakeFailed` / `PsmWakeAcquired` / `PsmWakeConsumed` / `PsmWakeReleased` | 生命周期 |
| `PsmWakeSymbols` | 5 个 solveSymbol 的位图 |

### 2.4 判读

| 读数 | 期望 / 含义 |
|---|---|
| `PsmWakeRun=1` | 触发点到达 |
| `PsmWakeGetStateResult=1` | 分类 0x12 的状态存在 |
| `PsmWakeSetUvdResult=1` / `AdjustResult=1` | 状态机接受了请求 |
| `PsmWakeFlag5F0After=1`（或 690） | PSM 已把 VCN 目标状态置为开 |
| `PsmWakeVcnGateDelta>0` | PSM 真的调用了 `PHM_PowerGateVCN` |
| `PsmWake190After=0` | VCN 已在 PSM 管辖下上电 |
| `PsmWakeFailed=1` | 任一步失败；看对应 result 与 flags |
| `PsmWakeAcquired=1` 且随后 `CailInitResult=0` / `InitHardwareCailRaw=1` | 目标达成，引擎 init 在 PSM 上电后成功 |

### 2.5 失败时的下一步

- `GetStateResult != 1`：分类 0x12 在当前 power state 表里不存在；需要枚举
  `PSM_EnumerateAllStates` / `PSM_ListPresentClassifications`，找可用的 UVD 分类。
- `SetUvdResult/AdjustResult` 成功但 flag 仍 0：`PSM_AdjustPowerState` 被
  `PHM_isAdjustPowerStateBlocked` 挡住，或选中的状态没有 VCN 属性；需要查
  `[psm+0x280]`（`PSM_ReArbitrateUVDState_Legacy` 会置 1）与状态属性表。
- flag=1 但 `GateDelta=0`：`PSM_ReArbit` 没有走 VCN 分支；查 `[psm+0x5F0]/[0x690]`
  是否在 `PSM_AdjustPowerState` 之后又被状态机清掉，或 `PHM_PowerGateVCN` 的钩子
  计数是否被其它路径抵消。
- flag=1、GateDelta>0、`190=0` 但引擎 init 仍失败：问题回到 CAIL/firmware
  （`loadUVDFW`/`initHardwareCail`），不再是电源问题。

## 3. 构建与实验步骤

1. 构建 build47；记录 CDHash/UUID；默认路径不改变电源。
2. 安装 build47 默认版 → 重启 → 空闲 60s → 播 30s QuickTime → dump：
   - 期望：系统稳定，`PsmWakeRun` 不出现（无 boot-arg），
     `InitPowerGate1A0Before/After=1`、`UnsafePoweredAtInit=0`。
3. 备份 `config.plist`，追加 `-NRedVCNPsmWake` → 重启 → 登录后播 30s QuickTime → dump。
4. 读 `PsmWake*`、`PsmReArbitFlag5F0/690`、`PsmReArbitGateDelta`、`CailInitResult`、
   `InitHardwareCailRaw`、`CailUvdStatusBefore/After`。
5. 实验后立即恢复 `config.plist` 备份，再决定是否保留 build47。

## 4. 风险

- 这是**第一次让 PSM 主动给 VCN 上电**的实验。虽然走的是驱动自己的状态机，但
  PSM 可能返回“已阻塞/无可用状态”，也可能在错误的时间应用状态；失败路径已尽量
  只读 + 释放，仍不能完全排除 panic。
- 触发点在视频上下文 start，若登录时也有视频上下文（avconferenced），唤醒可能在
  登录时发生；因此必须保留 `[state+0x190] != 0` 与 flag 前置检查，并在客户端 stop
  时释放。
- 任何一次实验后先恢复 config.plist，再重启；不要在默认启动里保留 `-NRedVCNPsmWake`。
