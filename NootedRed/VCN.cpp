// 复用原生 VCN2 引擎和客户端，修正已核实的跨驱动调用。
#include <Headers/kern_api.hpp>
#include <Headers/kern_iokit.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/pci/IOPCIDevice.h>
#include <Kexts.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <VCN.hpp>
#include <VCNCapabilities.hpp>
#include <VCNKernelData.hpp>
#include <VCNKernelProfiles.hpp>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>

// 文件作用域（匿名 namespace 之外）：电源脉冲探针要读 UVD_STATUS（0x83AF）。
static UInt32 (*cailReadMmRegister)(void*, UInt32){nullptr};

namespace
{

    static const char*      path = "/System/Library/Extensions/AMDRadeonX6000.kext/Contents/MacOS/AMDRadeonX6000";
    KernelPatcher::KextInfo x6000{"com.apple.kext.AMDRadeonX6000",  &path, 1, {true}, {},
                                  KernelPatcher::KextInfo::Unloaded};
    bool                    requested{false}, has5000{false}, has6000{false}, hooks{false}, engineAllocated{false};
    bool                    loadAttempted{false}, loadCompleted{false};
    // VCN1（Raven/Picasso）全部使用 X5000 原生类，不加载 X6000。
    bool                    vcn1Prepared{false};
    OSMetaClass*            uvd7EngineMC{nullptr};
    mach_vm_address_t       smlQueryOriginalGFX9{0}, smlQueryOriginalBase{0};
    mach_vm_address_t       uvd7InitOriginal{0}, uvd7InitHardwareOriginal{0}, fwMessageOriginal{0};
    UInt32                  fwMessageCount{0};
    mach_vm_address_t       uvd7LoadUVDFWOriginal{0}, uvd7InitHardwareCailOriginal{0};
    // AMDUVD7HWEngine 的 __ZTV；对象虚表与它相减得到不随 KASLR 变化的类差值。
    mach_vm_address_t       uvd7VTable{0};
    UInt32                  vtableDelta(const void* object);
    UInt32                  uvd7InitHardwareCount{0}, smlQueryServedCount{0};
    UInt32                  uvd7InitCount{0}, uvd7LoadUVDFWCount{0}, uvd7InitHardwareCailCount{0};
    // VCN1 的 SML 实例由 services 的 +0x850 工厂创建，未必落在静态打补丁的两张虚表上；
    // 运行期按对象真实 vptr 再接管一次 +0x58，并把原生返回值原样链式调用。
    mach_vm_address_t       smlQueryOriginalLive{0}, externalInterfacesOriginal{0};
    UInt32                  smlLiveCount{0}, externalInterfacesCount{0};
    // CAIL 侧提交入口（[engine+0x18]->vt[0x2B8] 得到的客户端对象的 +0xD0）只在运行期可见。
    mach_vm_address_t       cailSubmitOriginal{0}, cailClient{0};
    // CAIL 服务对象的早期/完整初始化槽：硬件侧调用它们之后 uvdControl 才会工作。
    mach_vm_address_t       cailEarlyInitializeOriginal{0}, cailInitializeOriginal{0};
    bool                    cailSlotsHooked{false};
    UInt32                  cailSubmitCount{0};
    // 两个引擎共用同一个 CAIL 客户端，按 isEncode 分开记录首个失败的 selector。
    UInt32                  cailEncode{0}, cailFailed[2]{0, 0}, cailDecResult{0}, cailEncResult{0};
    // 原生首条固件消息被判为不支持时，退回不带该消息的序列再试一次；单独记录该次失败。
    UInt32                  cailRetryFailed[2]{0, 0};
    bool                    cailRetryActive{false};
    mach_vm_address_t       engineStartOriginal{0}, enginePowerOriginal{0};
    mach_vm_address_t       engineRingsOriginal{0}, engineChannelsOriginal{0};
    // 电源管理：TF_PhwRaven_InitPowerGateState 的 phm 参数就是 PHM 扩展块
    // （+0x88 = SMC 接口，+0xA0 = ASIC 电源状态）。这里只观测，不主动写硬件。
    mach_vm_address_t       initPowerGateOriginal{0};
    void*                   phmHandle{nullptr};
    // PhwRaven_Initialize 通过 PECI 读取 PP_EnableVCNPG（默认 1）。Picasso 的 personality 属性
    // 虽然出现在 IGPU 上，但驱动读到的仍是默认值（build37），所以直接在 PECI 层把该键钉成 0：
    // TF_PhwRaven_InitPowerGateState 便会在引擎 init 之前走 PowerUpVCN。
    mach_vm_address_t       peciReadRegistryOriginal{0};
    UInt32                  peciVcnPGCalls{0}, initPowerGateCalls{0};
    // PhwRaven 电源域入口与 PHM 门控调度：确认驱动自己的 TF 路径真的上了电，以及之后是否被 PSM 关掉。
    mach_vm_address_t       ravenPowerUpOriginal{0}, ravenPowerDownOriginal{0}, phmPowerGateVCNOriginal{0};
    UInt32                  ravenPowerUpCalls{0}, ravenPowerDownCalls{0}, phmPowerGateVCNCalls{0};
    // PSM 的媒体 block 使用与 UVD 门控：视频启动任务会调 PSM_UseAsicBlock（block 2/3/6），
    // 用来判断驱动自己的 PSM 路径是否真的激活了 UVD/VCN，而不是只有我们在裸发 PowerUpVCN。
    mach_vm_address_t       psmUseAsicBlockOriginal{0}, phmPowerGateUVDOriginal{0};
    UInt32                  psmUseAsicBlockCalls{0}, psmUseAsicBlockMask{0}, phmPowerGateUVDCalls{0};
    // PSM 仲裁：Raven 上真正调用 PHM_PowerGateVCN 的唯一入口（[psm+0x5f0]/[psm+0x690] 控制）。
    // PSM_UseAsicBlock 的分发目标 _PhwRaven_SetAsicBlockGating 是 `mov eax,1; ret` 空桩，
    // 所以只有这条路径能告诉我们媒体 block 是否真的被 PSM 打开了。
    mach_vm_address_t       psmReArbitOriginal{0};
    UInt32                  psmReArbitCalls{0};
    // PSM 客户端激活与 PEM 视频启动任务：判断视频栈到底有没有向 PSM 申请媒体 block。
    mach_vm_address_t       psmActivateClientOriginal{0}, pemVideoStartOriginal{0};
    UInt32                  psmActivateClientCalls{0}, pemVideoStartCalls{0};
    mach_vm_address_t       acceleratorStartOriginal{0};
    mach_vm_address_t       safeCastOriginal{0}, newSharedOriginal{0}, newClientOriginal{0}, newVideoOriginal{0};
    mach_vm_address_t       newVideo6000{0};
    mach_vm_address_t       clientStart5000{0}, clientStop5000{0}, clientStart6000{0}, clientStop6000{0};
    mach_vm_address_t       copy5000{0}, copy6000{0}, sml5000{0}, sml6000{0}, initHardwareOriginal{0};
    mach_vm_address_t       signalOriginal{0};
    // 诊断：视频上下文第一次查询能力时主动把解码引擎的 initHardware 跑一遍，
    // 这样结论不再依赖「有没有真的播放视频」。
    void*                   probeEngine{nullptr};
    UInt32                  probeInitCount{0}, probeFwCount{0};
    void                    (*notifyAccess)(void*){nullptr};
    // 用户态视频链：加速器 +0x1F10 的 bit3 是 newVideoContext 的闸门；
    // Vega10VideoContext 的 contextStart/newContext/getCapabilities 与
    // AMDAccelUVDContext 的 init/requestCapability 决定 AppleGVA 能否拿到硬件解码能力。
    mach_vm_address_t accelVideoContextStartOriginal{0}, videoContextStartOriginal{0};
    mach_vm_address_t videoGetCapabilitiesOriginal{0}, videoNewContextOriginal{0};
    mach_vm_address_t uvdContextInitOriginal{0}, uvdRequestCapabilityOriginal{0};
    UInt32            newVideoContextCount{0}, accelVideoContextStartCount{0}, videoContextStartCount{0};
    UInt32            videoGetCapabilitiesCount{0}, videoNewContextCount{0};
    UInt32            uvdContextInitCount{0}, uvdRequestCapabilityCount{0};
    // 用户客户端分派与 hwInfo 回报：确定 AppleGVA 在 contextStart 之后实际请求了哪些 selector，
    // 以及 getHWInfo 给出的能力字节（driver 依此决定是否继续创建解码上下文）。
    mach_vm_address_t accelDispatchOriginal{0}, accelHWInfoOriginal{0};
    UInt32            accelDispatchCount{0}, accelDispatchSeen{0}, accelHWInfoCount{0};
    OSMetaClass*            engineClass{nullptr};
    OSMetaClass*            sharedClass{nullptr};
    OSMetaClass*            clientClass{nullptr};
    struct Pair
    {
        const OSMetaClass* oldClass{nullptr};
        const OSMetaClass* newClass{nullptr};
    };
    Pair                  pairs[4];
    constexpr const char* classes[] = {"AMDGraphicsAccelerator", "AMDAccelVideoContext", "AMDAccelChannel",
                                       "IAMDHWChannel"};
    constexpr unsigned    lengths[] = {37, 35, 30, 28};
    UInt32                hardwareInitCount{0};

    UInt32 readState32(const void* state, const size_t offset)
    {
        UInt32 value{0};
        if (state != nullptr) { memcpy(&value, static_cast<const UInt8*>(state) + offset, sizeof(value)); }
        return value;
    }

    bool supported()
    {
        return requested && VCNCapabilities::rejection(NRed::singleton().getDeviceID(), currentKernelVersion()) == 0;
    }

    bool vcn1Device()
    {
        const auto* capability = VCNCapabilities::findDevice(NRed::singleton().getDeviceID());
        return capability && capability->generation == VCNCapabilities::Generation::VCN1 && capability->vaFamily != 0;
    }

    bool acceleratorStart(void* self, IOService* provider)
    {
        auto  original = reinterpret_cast<bool (*)(void*, IOService*)>(acceleratorStartOriginal);
        auto* pci      = OSDynamicCast(IOPCIDevice, provider);
        if (!supported() || !pci || WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigVendorID) != 0x1002
            || WIOKit::readPCIConfigValue(pci, WIOKit::kIOPCIConfigDeviceID) != NRed::singleton().getDeviceID())
        {
            return original(self, provider);
        }
        bool expected = false;
        if (__atomic_compare_exchange_n(&loadAttempted, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (vcn1Device()) {
                // VCN1 全部使用 X5000 原生类；无需可选加载 X6000。
                const bool complete = has5000 && vcn1Prepared;
                __atomic_store_n(&loadCompleted, complete, __ATOMIC_RELEASE);
                NRed::singleton().setProp32("NRedVCNNativeLoadStatus", kOSReturnSuccess);
                NRed::singleton().setProp32("NRedVCNNativeLoadReady", complete);
                SYSLOG("VCN", "VCN1 native path prepared=%u", complete);
            }
            else {
                // 在驱动 start 内请求可选加载，避免插件库依赖和加载回调内的递归请求。
                const auto status   = OSKextLoadKextWithIdentifier("com.apple.kext.AMDRadeonX6000");
                const bool complete = status == kOSReturnSuccess && has5000 && has6000 && hooks;
                __atomic_store_n(&loadCompleted, complete, __ATOMIC_RELEASE);
                NRed::singleton().setProp32("NRedVCNNativeLoadStatus", static_cast<UInt32>(status));
                NRed::singleton().setProp32("NRedVCNNativeLoadReady", complete);
                SYSLOG("VCN", "native load status=0x%X prepared=%u", status, complete);
            }
        }
        const auto result = original(self, provider);
        NRed::singleton().setProp32("NRedVCNAcceleratorStarted", result);
        if (!result && !engineAllocated) { __atomic_store_n(&loadCompleted, false, __ATOMIC_RELEASE); }
        return result;
    }

    template<typename T>
    bool resolve(KernelPatcher& patcher, size_t id, const char* name, T& target, mach_vm_address_t slide, size_t size)
    {
        target = reinterpret_cast<T>(patcher.solveSymbol(id, name, slide, size, true));
        patcher.clearError();
        if (!target) {
            SYSLOG("VCN", "missing symbol: %s", name);
            return false;
        }
        return true;
    }

    OSMetaClassBase* safeCast(const OSMetaClassBase* object, const OSMetaClass* target)
    {
        using Cast                             = OSMetaClassBase* (*)(const OSMetaClassBase*, const OSMetaClass*);
        auto                          original = reinterpret_cast<Cast>(safeCastOriginal);
        auto*                         result   = original(object, target);
        if (result || !object || !VCN::ready()) { return result; }
        for (const auto& pair : pairs) {
            const auto* alternative = target == pair.oldClass ? pair.newClass :
                                      target == pair.newClass ? pair.oldClass :
                                                                nullptr;
            if (!alternative) { continue; }
            result = original(object, alternative);
            if (result && result->getMetaClass()->getClassSize() >= target->getClassSize()) { return result; }
        }
        return nullptr;
    }

    bool verifyKernelABI(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size, bool old)
    {
        if (size < 16 || slide > ~mach_vm_address_t{0} - size) { return false; }
        const auto* guards = VCNKernelProfiles::guards(currentKernelVersion().major(), old);
        if (!guards) { return false; }
        static_assert(arrsize(VCNKernelData::Guards5000) == arrsize(VCNKernelData::Guards6000));
        mach_vm_address_t table{0};
        if (!resolve(patcher, id, old ? VCNKernelData::Table5000 : VCNKernelData::Table6000, table, slide, size)) {
            return false;
        }
        for (size_t i = 0; i < arrsize(VCNKernelData::Guards5000); ++i) {
            const auto& guard = guards[i];
            // __ZTV 的两个头部槽不属于对象地址点；先核对边界再读取指针和 getter。
            const auto offset = 2 * sizeof(void*) + guard.slot;
            if (offset > size - sizeof(void*) || table < slide || table - slide > size - sizeof(void*) - offset) {
                return false;
            }
            mach_vm_address_t target{0};
            memcpy(&target, reinterpret_cast<void*>(table + offset), sizeof(target));
            if (target < slide || target - slide >= size || guard.size > size - (target - slide)) { return false; }
            if (guard.target) {
                mach_vm_address_t expected{0};
                if (!resolve(patcher, id, guard.target, expected, slide, size) || target != expected) { return false; }
            }
            else if (!guard.code || !guard.size || memcmp(reinterpret_cast<void*>(target), guard.code, guard.size)) {
                return false;
            }
        }
        return true;
    }

    void* newShared()
    {
        if (VCN::allocated()) { return sharedClass->alloc(); }
        return reinterpret_cast<void* (*)()>(newSharedOriginal)();
    }
    void* newClient()
    {
        if (VCN::allocated()) { return clientClass->alloc(); }
        return reinterpret_cast<void* (*)()>(newClientOriginal)();
    }
    // AMDGFX9GraphicsAccelerator::newVideoContext 只在 self+0x1F10 的 bit3 置位时实例化视频上下文；
    // APU 上该位未置位时内核永远拿不到视频上下文，能力查询也就不会发生。
    void* allocateVideoContext(void* self)
    {
        using Function = void* (*)(void*);
        UInt8  flags{0};
        if (self) { memcpy(&flags, static_cast<UInt8*>(self) + 0x1F10, sizeof(flags)); }
        const auto count = __atomic_fetch_add(&newVideoContextCount, 1U, __ATOMIC_RELAXED);
        NRed::singleton().setProp32("NRedVCN1NewCtxFlag", flags);
        if (self && vcn1Device() && (flags & 0x8) == 0) {
            static_cast<UInt8*>(self)[0x1F10] = static_cast<UInt8>(flags | 0x8);
            NRed::singleton().setProp32("NRedVCN1NewCtxForced", 1);
        }
        auto* result = reinterpret_cast<Function>(newVideoOriginal)(self);
        NRed::singleton().setProp32("NRedVCN1NewCtxResult", result != nullptr);
        if (count < 8) { SYSLOG("VCN1", "newVideoContext flags=0x%X result=%p", static_cast<UInt32>(flags), result); }
        return result;
    }
    void* newVideo(void* self)
    {
        if (VCN::allocated()) { return reinterpret_cast<void* (*)(void*)>(newVideo6000)(self); }
        // VCN1 走原生 X5000 路径，这里只接管闸门，其余原样转发。
        if (vcn1Device()) { return allocateVideoContext(self); }
        return reinterpret_cast<void* (*)(void*)>(newVideoOriginal)(self);
    }
    bool clientStart(void* self, void* provider)
    {
        auto address = VCN::allocated() ? clientStart5000 : clientStart6000;
        return reinterpret_cast<bool (*)(void*, void*)>(address)(self, provider);
    }
    void clientStop(void* self, void* provider)
    {
        auto address = VCN::allocated() ? clientStop5000 : clientStop6000;
        reinterpret_cast<void (*)(void*, void*)>(address)(self, provider);
    }
    UInt64 surfaceCopy(void* self, void* input, UInt64 size, void* event)
    {
        auto address = VCN::allocated() ? copy5000 : copy6000;
        return reinterpret_cast<UInt64 (*)(void*, void*, UInt64, void*)>(address)(self, input, size, event);
    }
    void* createSML(UInt32 configuration)
    {
        // GFX9 专用配置保留原生 GFX9 SML；其他配置使用含 VCN 描述的通用接口。
        if (VCN::ready() && (configuration & 0xFFF8FFFFU) != 0) {
            // 原生 X6000 的宽松掩码会选择 GFX10；最低位只用于选择通用构造器，不传入构造器。
            return reinterpret_cast<void* (*)(UInt32)>(sml6000)(configuration | 1U);
        }
        return reinterpret_cast<void* (*)(UInt32)>(sml5000)(configuration);
    }
    void signalWork(void* hardware)
    {
        if (VCN::allocated()) { notifyAccess(hardware); }
        else {
            reinterpret_cast<void (*)(void*)>(signalOriginal)(hardware);
        }
    }
    bool initHardware(void* self, UInt32 channel)
    {
        const auto result = reinterpret_cast<bool (*)(void*, UInt32)>(initHardwareOriginal)(self, channel);
        NRed::singleton().setProp32("NRedVCNLastHardwareInit", result);
        auto count = __atomic_fetch_add(&hardwareInitCount, 1U, __ATOMIC_RELAXED);
        if (count < 16) { SYSLOG("VCN", "initHardware channel=%u result=%u", channel, result); }
        return result;
    }

    // SML 查询 +0x58：选择子 0x0C 取 VCN 基址。原生实现按 CAIL 家族应答；先观察原生结果，
    // 再以 Raven 基址表（与 Vega10 共表）应答，保证 APU 侧总是 0x7800。
    int smlQueryImpl(void* self, const UInt32 selector, const UInt32 isEncode, const UInt32 flag, void* out,
                     const mach_vm_address_t original)
    {
        if (selector == 0xC && out != nullptr) {
            UInt32 nativeBase = 0;
            const int native = reinterpret_cast<int (*)(void*, UInt32, UInt32, UInt32, void*)>(original)(
                self, selector, isEncode, flag, &nativeBase);
            NRed::singleton().setProp32("NRedVCN1SMLNativeStatus", static_cast<UInt32>(native));
            NRed::singleton().setProp32("NRedVCN1SMLNativeBase", nativeBase);
            // Raven 上原生 SML 也会给出 VCN 基址（实机 0x7E00）；原生非 0 时以原生为准，
            // 只有原生缺省时才退回 vega10_ip_offset.h 的 VCN_BASE__INST0_SEG0。
            constexpr UInt32 vcnBaseFallback{0x7800};
            *static_cast<UInt32*>(out) = nativeBase != 0 ? nativeBase : vcnBaseFallback;
            auto count = __atomic_fetch_add(&smlQueryServedCount, 1U, __ATOMIC_RELAXED);
            NRed::singleton().setProp32("NRedVCN1SMLBaseServed", count + 1);
            if (count < 16) {
                SYSLOG("VCN1", "SML base query: native=%d nativeBase=0x%X served=0x7800 isEncode=%u", native, nativeBase,
                       isEncode);
            }
            return 0;
        }
        return reinterpret_cast<int (*)(void*, UInt32, UInt32, UInt32, void*)>(original)(self, selector, isEncode, flag,
                                                                                        out);
    }
    int smlQueryGFX9(void* self, const UInt32 selector, const UInt32 isEncode, const UInt32 flag, void* out)
    {
        return smlQueryImpl(self, selector, isEncode, flag, out, smlQueryOriginalGFX9);
    }
    int smlQueryPlain(void* self, const UInt32 selector, const UInt32 isEncode, const UInt32 flag, void* out)
    {
        return smlQueryImpl(self, selector, isEncode, flag, out, smlQueryOriginalBase);
    }

    int smlQueryLive(void* self, const UInt32 selector, const UInt32 isEncode, const UInt32 flag, void* out)
    {
        if (smlQueryOriginalLive == 0) { return -1; }
        return smlQueryImpl(self, selector, isEncode, flag, out, smlQueryOriginalLive);
    }

    // 读对象真实 vptr 的 +0x58；未接管时换成 smlQueryLive（保留原值供链式调用）。
    void attachLiveSML(void* object)
    {
        if (!object) { return; }
        auto** const vptr = *reinterpret_cast<void***>(object);
        if (!vptr) { return; }
        auto&      slot    = vptr[0x58 / sizeof(void*)];
        const auto current = reinterpret_cast<mach_vm_address_t>(slot);
        NRed::singleton().setProp32("NRedVCN1SMLSlotLow", static_cast<UInt32>(current));
        if (current == reinterpret_cast<mach_vm_address_t>(&smlQueryLive)
            || current == reinterpret_cast<mach_vm_address_t>(&smlQueryGFX9)
            || current == reinterpret_cast<mach_vm_address_t>(&smlQueryPlain))
        {
            NRed::singleton().setProp32("NRedVCN1SMLLiveHooked", 1);
            return;
        }
        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) { return; }
        smlQueryOriginalLive = current;
        slot                 = reinterpret_cast<void*>(&smlQueryLive);
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        NRed::singleton().setProp32("NRedVCN1SMLLiveHooked", 1);
        const auto count = __atomic_fetch_add(&smlLiveCount, 1U, __ATOMIC_RELAXED);
        if (count < 8) {
            SYSLOG("VCN1", "live SML hooked: object=%p vptr=%p slot=%p", object, vptr,
                   reinterpret_cast<void*>(current));
        }
    }

    // CAIL 提交：initHardwareCail 用 selector 8/9/1 提交消息，失败即返回 false；
    // 逐个 selector 记录返回值，直接指出是哪一步被拒。
    int cailSubmit(void* client, const UInt32 selector, const UInt64 arg, void* message)
    {
        const auto result = reinterpret_cast<int (*)(void*, UInt32, UInt64, void*)>(cailSubmitOriginal)(client, selector,
                                                                                                        arg, message);
        NRed::singleton().setProp32("NRedVCN1CailLastSelector", selector);
        NRed::singleton().setProp32("NRedVCN1CailLastResult", static_cast<UInt32>(result));
        if (message != nullptr) {
            UInt32 opcode{0};
            memcpy(&opcode, message, sizeof(opcode));
            NRed::singleton().setProp32("NRedVCN1CailMsgOpcode", opcode);
        }
        // _CAILUvdControl 先用适配器状态决定返回值：+0xE34 的 bit2 必须置位、bit17 必须清零，
        // 再看 caps 0xD6/0x15B。把这三个判据一起发布，失败就不再需要靠猜。
        void* adapter{nullptr};
        memcpy(&adapter, static_cast<UInt8*>(client) + 0x20038, sizeof(adapter));
        if (adapter) {
            UInt32 flags{0};
            memcpy(&flags, static_cast<UInt8*>(adapter) + 0xE34, sizeof(flags));
            NRed::singleton().setProp32("NRedVCN1CailAdapterFlags", flags);
            NRed::singleton().setProp32("NRedVCN1CailGateBit2", (flags >> 2) & 1U);
            NRed::singleton().setProp32("NRedVCN1CailGateBit17", (flags >> 17) & 1U);
        }
        if (result != 0 && cailRetryActive && cailRetryFailed[cailEncode & 1] == 0) {
            cailRetryFailed[cailEncode & 1] = selector;
        }
        const char* property = nullptr;
        switch (selector) {
            case 1: property = "NRedVCN1CailSubmit1"; break;
            case 6: property = "NRedVCN1CailSubmit6"; break;
            case 7: property = "NRedVCN1CailSubmit7"; break;
            case 8: property = "NRedVCN1CailSubmit8"; break;
            case 9: property = "NRedVCN1CailSubmit9"; break;
            default: break;
        }
        if (property) { NRed::singleton().setProp32(property, static_cast<UInt32>(result)); }
        if (result != 0 && cailFailed[cailEncode & 1] == 0) { cailFailed[cailEncode & 1] = selector; }
        const auto count = __atomic_fetch_add(&cailSubmitCount, 1U, __ATOMIC_RELAXED);
        if (count < 32) {
            SYSLOG("VCN1", "CAIL submit selector=%u arg=0x%llX result=%d", selector, arg, result);
        }
        return result;
    }

// 早期初始化（X=0x10）与完整初始化（X=0x18）：前者只挂回调，后者才装载 VBIOS 并建适配器。
    // 客户端字段在早期初始化里才被填满（+0x15/0x16 是 VBIOS 签名，+0x20038 是 CAIL 适配器），
    // 所以每次调用之后都重新读一遍，避免 ioreg 里留下启动阶段的陈旧读数。
    void publishCailClient(void* self)
    {
        if (!self) { return; }
        UInt8   ready{0}, vbios{0}, signature{0};
        UInt64  adapter{0};
        memcpy(&ready, static_cast<UInt8*>(self) + 0x14, sizeof(ready));
        memcpy(&vbios, static_cast<UInt8*>(self) + 0x15, sizeof(vbios));
        memcpy(&signature, static_cast<UInt8*>(self) + 0x16, sizeof(signature));
        memcpy(&adapter, static_cast<UInt8*>(self) + 0x20038, sizeof(adapter));
        NRed::singleton().setProp32("NRedVCN1CailReady", ready);
        NRed::singleton().setProp32("NRedVCN1CailVbios", vbios);
        NRed::singleton().setProp32("NRedVCN1CailVbiosSignature", signature);
        NRed::singleton().setProp32("NRedVCN1CailAdapterLow", static_cast<UInt32>(adapter));
    }

    int cailEarlyInitialize(void* self, void* callbacks, void* init)
    {
        if (cailEarlyInitializeOriginal == 0) { return 0xE00002C0; }
        const auto result = reinterpret_cast<int (*)(void*, void*, void*)>(cailEarlyInitializeOriginal)(self, callbacks,
                                                                                                        init);
        NRed::singleton().setProp32("NRedVCN1CailEarlyCalled", 1);
        NRed::singleton().setProp32("NRedVCN1CailEarly", static_cast<UInt32>(result));
        publishCailClient(self);
        SYSLOG("VCN1", "CAIL earlyInitialize result=0x%X", result);
        return result;
    }

    int cailInitialize(void* self, void* init)
    {
        if (cailInitializeOriginal == 0) { return 0xE00002C0; }
        const auto result = reinterpret_cast<int (*)(void*, void*)>(cailInitializeOriginal)(self, init);
        NRed::singleton().setProp32("NRedVCN1CailInitCalled", 1);
        NRed::singleton().setProp32("NRedVCN1CailInit", static_cast<UInt32>(result));
        publishCailClient(self);
        SYSLOG("VCN1", "CAIL initialize result=0x%X", result);
        return result;
    }

    void hookCailClient(void* client)
    {
        if (!client || cailSlotsHooked) { return; }
        auto** const clientVptr = *reinterpret_cast<void***>(client);
        if (!clientVptr) { return; }
        cailSlotsHooked = true;
        NRed::singleton().setProp32("NRedVCN1CailClientSeen", 1);
        NRed::singleton().setProp32("NRedVCN1CailClientLow",
                                    static_cast<UInt32>(reinterpret_cast<uintptr_t>(client)));
        NRed::singleton().setProp32("NRedVCN1CailVptrLow",
                                    static_cast<UInt32>(reinterpret_cast<uintptr_t>(clientVptr)));
        UInt8 cailReady{0}, cailVbios{0};
        memcpy(&cailReady, static_cast<UInt8*>(client) + 0x14, sizeof(cailReady));
        memcpy(&cailVbios, static_cast<UInt8*>(client) + 0x15, sizeof(cailVbios));
        UInt64 cailAdapter{0};
        memcpy(&cailAdapter, static_cast<UInt8*>(client) + 0x20038, sizeof(cailAdapter));
        NRed::singleton().setProp32("NRedVCN1CailReady", cailReady);
        NRed::singleton().setProp32("NRedVCN1CailVbios", cailVbios);
        NRed::singleton().setProp32("NRedVCN1CailAdapterLow", static_cast<UInt32>(cailAdapter));
        const auto hookSlot = [clientVptr](const UInt32 offset, const mach_vm_address_t replacement,
                                          mach_vm_address_t& original) {
            auto& entry = clientVptr[offset / sizeof(void*)];
            if (reinterpret_cast<mach_vm_address_t>(entry) == replacement || original != 0) { return; }
            if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) { return; }
            original = reinterpret_cast<mach_vm_address_t>(entry);
            entry    = reinterpret_cast<void*>(replacement);
            MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        };
        hookSlot(0x10, reinterpret_cast<mach_vm_address_t>(&cailEarlyInitialize), cailEarlyInitializeOriginal);
        hookSlot(0x18, reinterpret_cast<mach_vm_address_t>(&cailInitialize), cailInitializeOriginal);
    }

    void attachCailSubmit(void* interface)
    {
        if (!interface || cailClient != 0) { return; }
        auto* const vptr = *reinterpret_cast<void***>(interface);
        if (!vptr) { return; }
        using ClientGetter = void* (*)(void*);
        auto* const getter = reinterpret_cast<ClientGetter>(vptr[0x2B8 / sizeof(void*)]);
        if (!getter) { return; }
        auto* const client = getter(interface);
        if (!client) { return; }
        auto** const clientVptr = *reinterpret_cast<void***>(client);
        if (!clientVptr) { return; }
        auto&      slot    = clientVptr[0xD0 / sizeof(void*)];
        const auto current = reinterpret_cast<mach_vm_address_t>(slot);
        if (current == reinterpret_cast<mach_vm_address_t>(&cailSubmit)) {
            cailClient = reinterpret_cast<mach_vm_address_t>(client);
            return;
        }
        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) { return; }
        cailSubmitOriginal = current;
        slot               = reinterpret_cast<void*>(&cailSubmit);
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        cailClient = reinterpret_cast<mach_vm_address_t>(client);
        NRed::singleton().setProp32("NRedVCN1CailHooked", 1);
        NRed::singleton().setProp32("NRedVCN1CailClientLow",
                                    static_cast<UInt32>(reinterpret_cast<uintptr_t>(client)));
        // 客户端身份发布给 ioreg，便于按 vptr/提交函数地址反查它所属的 fileset。
        NRed::singleton().setProp32("NRedVCN1CailVptrLow",
                                    static_cast<UInt32>(reinterpret_cast<uintptr_t>(clientVptr)));
        NRed::singleton().setProp32("NRedVCN1CailSubmitLow", static_cast<UInt32>(current));
        // uvdControl 自己在 [+0x14]==0 时直接返回 0xE00002C0：先把这份状态读出来。
        UInt8 cailReady{0}, cailVbios{0};
        memcpy(&cailReady, static_cast<UInt8*>(client) + 0x14, sizeof(cailReady));
        memcpy(&cailVbios, static_cast<UInt8*>(client) + 0x15, sizeof(cailVbios));
        UInt64 cailAdapter{0};
        memcpy(&cailAdapter, static_cast<UInt8*>(client) + 0x20038, sizeof(cailAdapter));
        NRed::singleton().setProp32("NRedVCN1CailReady", cailReady);
        NRed::singleton().setProp32("NRedVCN1CailVbios", cailVbios);
        NRed::singleton().setProp32("NRedVCN1CailAdapterLow", static_cast<UInt32>(cailAdapter));
        const auto hookSlot = [clientVptr](const UInt32 offset, const mach_vm_address_t replacement,
                                          mach_vm_address_t& original) {
            auto& entry = clientVptr[offset / sizeof(void*)];
            if (reinterpret_cast<mach_vm_address_t>(entry) == replacement || original != 0) { return; }
            if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) { return; }
            original = reinterpret_cast<mach_vm_address_t>(entry);
            entry    = reinterpret_cast<void*>(replacement);
            MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        };
        hookSlot(0x10, reinterpret_cast<mach_vm_address_t>(&cailEarlyInitialize), cailEarlyInitializeOriginal);
        hookSlot(0x18, reinterpret_cast<mach_vm_address_t>(&cailInitialize), cailInitializeOriginal);
        SYSLOG("VCN1", "CAIL submit hooked: interface=%p client=%p slot=%p", interface, client,
               reinterpret_cast<void*>(current));
    }

    // UVD7（Vega10/VCN1）把固件下发改写成 SML 寄存器写消息；commitUVDFWMsg 是这条路径的出口。
    UInt8 uvd7CommitFWMessage(void* self, void* message, const UInt64 arg1, const UInt64 arg2, const UInt64 arg3)
    {
        UInt32 opcode{0};
        if (message != nullptr) { memcpy(&opcode, message, sizeof(opcode)); }
        const auto result = reinterpret_cast<UInt8 (*)(void*, void*, UInt64, UInt64, UInt64)>(fwMessageOriginal)(
            self, message, arg1, arg2, arg3);
        NRed::singleton().setProp32("NRedVCN1FwMsgOpcode", opcode);
        NRed::singleton().setProp32("NRedVCN1FwMsgResult", result);
        const auto count = __atomic_fetch_add(&fwMessageCount, 1U, __ATOMIC_RELAXED);
        NRed::singleton().setProp32("NRedVCN1FwMsgCount", count + 1);
        if (count < 16) { SYSLOG("VCN1", "commitUVDFWMsg opcode=0x%X arg1=%llu result=%u", opcode, arg1, result); }
        return result;
    }

    // +0x338 是 SML 接口指针（由 services->vt[0x850] 创建）：一出现就接管其查询槽。
    void hookCailClient(void* client);

    void externalInterfaces(void* self)
    {
        reinterpret_cast<void (*)(void*)>(externalInterfacesOriginal)(self);
        void* sml{nullptr};
        memcpy(&sml, static_cast<UInt8*>(self) + 0x338, sizeof(sml));
        NRed::singleton().setProp32("NRedVCN1SMLPresent", sml != nullptr);
        void* client{nullptr};
        memcpy(&client, static_cast<UInt8*>(self) + 0x340, sizeof(client));
        void* services{nullptr};
        memcpy(&services, static_cast<UInt8*>(self) + 0x320, sizeof(services));
        NRed::singleton().setProp32("NRedVCN1DSvcZtv", vtableDelta(services));
        NRed::singleton().setProp32("NRedVCN1DSmlZtv", vtableDelta(sml));
        NRed::singleton().setProp32("NRedVCN1DClientZtv", vtableDelta(client));
        // 服务对象的 start 已经把 CAIL 实例交出来了：先按实例挂槽并读出就绪字节，
        // 这样即使这一轮没有播放视频（initHardwareCail 不会被调用）也能看到闸门状态。
        hookCailClient(client);
        attachLiveSML(sml);
        const auto count = __atomic_fetch_add(&externalInterfacesCount, 1U, __ATOMIC_RELAXED);
        if (count < 8) { SYSLOG("VCN1", "initializeExternalInterfaces self=%p sml=%p", self, sml); }
    }

    // 对象虚表与 UVD7 引擎虚表的差值不随 KASLR 滑动变化，用它静态判定实例属于哪个类。
    UInt32 vtableDelta(const void* object)
    {
        if (!object || uvd7VTable == 0) { return 0; }
        void* vptr{nullptr};
        memcpy(&vptr, object, sizeof(vptr));
        // 无符号相减后截断：负差值以补码出现，与静态 __ZTV 表逐项对照时按 32 位解释即可。
        return static_cast<UInt32>(reinterpret_cast<mach_vm_address_t>(vptr) - uvd7VTable);
    }

    // 引擎 init(interface, engineType)：成功后 +0x250=VCN 基址、+0x254=isEncode（24H32 已核对）。
    bool uvd7Init(void* self, void* interface, const UInt32 engineType)
    {
        void* sml{nullptr};
        if (interface) { memcpy(&sml, static_cast<UInt8*>(interface) + 0x338, sizeof(sml)); }
        attachLiveSML(sml);
        const auto result =
            reinterpret_cast<bool (*)(void*, void*, UInt32)>(uvd7InitOriginal)(self, interface, engineType);
        UInt32 base = 0, isEncode = 0;
        memcpy(&base, static_cast<UInt8*>(self) + 0x250, sizeof(base));
        memcpy(&isEncode, static_cast<UInt8*>(self) + 0x254, sizeof(isEncode));
        // 原生 SML 查询在 APU 上给出的是本机 VCN 基址（实机 0x7E00），只有它缺失或越界时才
        // 退回 vega10_ip_offset.h 的 VCN_BASE__INST0_SEG0，避免把 Apple 算好的值覆盖掉。
        NRed::singleton().setProp32("NRedVCN1EngineBaseNative", base);
        if (result && vcn1Device() && (engineType == 5 || engineType == 6)) {
            constexpr UInt32 vcnBaseFallback{0x7800};
            if (base < 0x7000 || base > 0x8FFF) {
                memcpy(static_cast<UInt8*>(self) + 0x250, &vcnBaseFallback, sizeof(vcnBaseFallback));
                base = vcnBaseFallback;
                NRed::singleton().setProp32("NRedVCN1EngineBaseForced", 1);
            }
        }
        NRed::singleton().setProp32(engineType == 6 ? "NRedVCN1EngineBaseEnc" : "NRedVCN1EngineBaseDec", base);
        NRed::singleton().setProp32("NRedVCN1EngineInitResult", result);
        NRed::singleton().setProp32("NRedVCN1EngineInitType", engineType);
        NRed::singleton().setProp32("NRedVCN1EngineBase", base);
        NRed::singleton().setProp32("NRedVCN1EngineIsEncode", isEncode);
        if (result && vcn1Device() && (isEncode & 1) == 0) { probeEngine = self; }
        // 实例身份：硬件对象、引擎对象各自的虚表相对 UVD7 虚表的偏移。
        NRed::singleton().setProp32("NRedVCN1DHwZtv", vtableDelta(interface));
        NRed::singleton().setProp32("NRedVCN1DEngineZtv", vtableDelta(self));
        auto count = __atomic_fetch_add(&uvd7InitCount, 1U, __ATOMIC_RELAXED);
        if (count < 16) {
            SYSLOG("VCN1", "engine init type=%u result=%u base=0x%X isEncode=%u", engineType, result, base, isEncode);
        }
        return result;
    }

    // 电源脉冲探针（只在解码引擎第一次 initHardware 失败后跑一次）：
    // 临时清 [0x1A0] 让 PhwRaven_PowerUpVCN 走完整路径（msg 0xC + 0xC0/0xC1 寄存器写），
    // 读一次 UVD_STATUS(0x83AF)，随后立刻恢复 [0x1A0] 并 PowerDownVCN 关回去。
    // 窗口只有几毫秒，不在这里跑 CAIL init：先回答“上电后块到底活不活”。
    void powerPulseProbe(void* engine)
    {
        static UInt32 pulses{0};
        if (__atomic_fetch_add(&pulses, 1U, __ATOMIC_RELAXED) != 0) { return; }
        void* phm = __atomic_load_n(&phmHandle, __ATOMIC_ACQUIRE);
        if (phm == nullptr || ravenPowerUpOriginal == 0 || ravenPowerDownOriginal == 0) {
            NRed::singleton().setProp32("NRedVCN1PowerProbeRun", 0);
            return;
        }
        void* state{nullptr};
        memcpy(&state, static_cast<UInt8*>(phm) + 0xA0, sizeof(state));
        if (state == nullptr || readState32(state, 0x190) == 0) {
            // 已经处于上电状态：不要在我们不理解的窗口里继续动电源。
            NRed::singleton().setProp32("NRedVCN1PowerProbeRun", 0);
            return;
        }
        void* client{nullptr};
        memcpy(&client, static_cast<UInt8*>(engine) + 0x18, sizeof(client));
        void* adapter{nullptr};
        if (client != nullptr) { memcpy(&adapter, static_cast<UInt8*>(client) + 0x20038, sizeof(adapter)); }
        NRed::singleton().setProp32("NRedVCN1PowerProbeRun", 1);
        NRed::singleton().setProp32("NRedVCN1PowerProbeAdapter", adapter != nullptr);
        NRed::singleton().setProp32("NRedVCN1PowerProbeDPG", readState32(state, 0x194));
        const auto saved1A0 = readState32(state, 0x1A0);
        const UInt32 zero{0};
        memcpy(static_cast<UInt8*>(state) + 0x1A0, &zero, sizeof(zero));
        const auto up = reinterpret_cast<int (*)(void*)>(ravenPowerUpOriginal)(phm);
        NRed::singleton().setProp32("NRedVCN1PowerProbeUpResult", static_cast<UInt32>(up));
        NRed::singleton().setProp32("NRedVCN1PowerProbe190AfterUp", readState32(state, 0x190));
        UInt32 status{0};
        if (cailReadMmRegister != nullptr && adapter != nullptr) {
            // 最多三次、间隔 1ms：SMU ack 之后块可能还要一点时间才报 idle。
            for (UInt32 i = 0; i < 3; ++i) {
                status = cailReadMmRegister(adapter, 0x83AF);
                if ((status & 2U) != 0) { break; }
                IODelay(1000);
            }
        }
        NRed::singleton().setProp32("NRedVCN1PowerProbeStatus", status);
        memcpy(static_cast<UInt8*>(state) + 0x1A0, &saved1A0, sizeof(saved1A0));
        const auto down = reinterpret_cast<int (*)(void*)>(ravenPowerDownOriginal)(phm);
        NRed::singleton().setProp32("NRedVCN1PowerProbeDownResult", static_cast<UInt32>(down));
        NRed::singleton().setProp32("NRedVCN1PowerProbe190AfterDown", readState32(state, 0x190));
        SYSLOG("VCN1", "power pulse up=%d status=0x%X down=%d 190=%u", up, status, down,
               readState32(state, 0x190));
    }

    bool uvd7InitHardware(void* self)
    {
        UInt8  early{0}, skipFirmware{0};
        UInt32 isEncode{0}, base{0};
        memcpy(&early, static_cast<UInt8*>(self) + 0x10, sizeof(early));
        memcpy(&skipFirmware, static_cast<UInt8*>(self) + 0x25D, sizeof(skipFirmware));
        memcpy(&isEncode, static_cast<UInt8*>(self) + 0x254, sizeof(isEncode));
        memcpy(&base, static_cast<UInt8*>(self) + 0x250, sizeof(base));
        NRed::singleton().setProp32("NRedVCN1HardwareEarlyFlag", early);
        NRed::singleton().setProp32("NRedVCN1HardwareSkipFirmware", skipFirmware);
        NRed::singleton().setProp32(isEncode ? "NRedVCN1HardwareBaseEnc" : "NRedVCN1HardwareBaseDec", base);
        // 只观察 PHM 状态，不再写 SMU/寄存器：上一版在 PHM 初始化后强制 PowerUpVCN，
        // 约 40 秒后触发 TLB flush 超时 panic（§22）。这里只读驱动的 TF 留下的状态。
        void* phm = __atomic_load_n(&phmHandle, __ATOMIC_ACQUIRE);
        NRed::singleton().setProp32("NRedVCN1PhmHandlePresent", phm != nullptr);
        if ((isEncode & 1U) == 0 && phm != nullptr) {
            void* state{nullptr};
            memcpy(&state, static_cast<UInt8*>(phm) + 0xA0, sizeof(state));
            const auto poweredNow = readState32(state, 0x190);
            NRed::singleton().setProp32("NRedVCN1PhmState190AtHardware", poweredNow);
            // [0x190]==0 表示 VCN 在引擎 init 时仍上电；build35/build41 都栽在这种状态，
            // 默认路径必须是 1（门控）。
            NRed::singleton().setProp32("NRedVCN1UnsafePoweredAtInit", poweredNow == 0 ? 1 : 0);
        }
        // 默认不再手动调 powerOnOff；让 accelerator 的 powerUpHWEngines 自己调用，
        // 用 PowerOnOffCalls/LastOn/LastResult 观察。只有 -NRedVCNForcePower 才强制调用。
        NRed::singleton().setProp32("NRedVCN1PowerHooked", enginePowerOriginal != 0);
        if ((isEncode & 1U) == 0 && enginePowerOriginal != 0
            && checkKernelArgument("-NRedVCNForcePower")) {
            static UInt32 forcedPowerCount{0};
            if (__atomic_fetch_add(&forcedPowerCount, 1U, __ATOMIC_RELAXED) == 0) {
                const auto powered = reinterpret_cast<bool (*)(void*, bool)>(enginePowerOriginal)(self, true);
                NRed::singleton().setProp32("NRedVCN1PowerForced", powered ? 1 : 0);
                NRed::singleton().setProp32(isEncode ? "NRedVCN1PowerEnc" : "NRedVCN1PowerDec",
                                            2U | (powered ? 1U : 0U));
                SYSLOG("VCN1", "forced powerOnOff(true) result=%u", powered ? 1U : 0U);
            }
        }
        // +0x25D!=0 时原生 initHardware 会跳过 loadUVDFW；但实测 UVD_STATUS(0x83AF) 前后都是 0，
        // 说明固件根本没被这条路径载入。诊断：第一次解码引擎 init 时临时清 0，让原生流程
        // 先 loadUVDFW，再以 flag=0 走 initHardwareCail；调用后恢复原值。
        bool firmwareFirst{false};
        if ((isEncode & 1U) == 0 && skipFirmware != 0) {
            static UInt32 firmwareFirstCount{0};
            if (__atomic_fetch_add(&firmwareFirstCount, 1U, __ATOMIC_RELAXED) == 0) {
                firmwareFirst                        = true;
                static_cast<UInt8*>(self)[0x25D] = 0;
                NRed::singleton().setProp32("NRedVCN1CailFirmwareFirstEver", 1);
            }
        }
        const auto result = reinterpret_cast<bool (*)(void*)>(uvd7InitHardwareOriginal)(self);
        if (firmwareFirst) { static_cast<UInt8*>(self)[0x25D] = skipFirmware; }
        NRed::singleton().setProp32("NRedVCN1CailFirmwareFirst", firmwareFirst ? 1 : 0);
        NRed::singleton().setProp32("NRedVCN1LastHardwareInit", result);
        NRed::singleton().setProp32(isEncode ? "NRedVCN1HardwareInitEnc" : "NRedVCN1HardwareInitDec", result);
        // 原生 init 失败后做一次短促电源脉冲，读完 UVD_STATUS 立即关回去（只解码引擎、只一次）。
        if ((isEncode & 1U) == 0 && vcn1Device()) { powerPulseProbe(self); }
        auto count = __atomic_fetch_add(&uvd7InitHardwareCount, 1U, __ATOMIC_RELAXED);
        if (count < 16) { SYSLOG("VCN1", "UVD7 initHardware result=%u fwFirst=%u", result, firmwareFirst ? 1U : 0U); }
        return result;
    }

    // 按实例虚表的槽位调用，走的仍是已路由的钩子，因此 initHardware 的观测值同样会更新。
    mach_vm_address_t engineSlot(void* engine, const mach_vm_address_t offset)
    {
        void*    vptr{nullptr};
        UInt64   target{0};
        memcpy(&vptr, engine, sizeof(vptr));
        if (!vptr) { return 0; }
        memcpy(&target, static_cast<UInt8*>(vptr) + offset, sizeof(target));
        return static_cast<mach_vm_address_t>(target);
    }

    bool probeHardwareInit()
    {
        if (!probeEngine || !vcn1Device()) { return false; }
        // probe 会在视频上下文启动时提前跑 CAIL init；build39 的 panic 正是这条路径
        // 在 0x83AF idle 等待里自旋。默认关闭，只有显式传 -NRedVCNProbe 才启用。
        if (!checkKernelArgument("-NRedVCNProbe")) { return false; }
        if (__atomic_load_n(&probeInitCount, __ATOMIC_RELAXED) != 0) { return false; }
        __atomic_add_fetch(&probeInitCount, 1U, __ATOMIC_RELAXED);
        UInt8 skipFirmware{0};
        memcpy(&skipFirmware, static_cast<UInt8*>(probeEngine) + 0x25D, sizeof(skipFirmware));
        NRed::singleton().setProp32("NRedVCN1ProbeSkipFirmware", skipFirmware);
        const auto init = engineSlot(probeEngine, 0x368);
        NRed::singleton().setProp32("NRedVCN1ProbeSlotValid", init != 0);
        // 原生顺序是 loadUVDFW → initHardwareCail；先加载固件，再初始化，避免在死块上等 idle。
        if (skipFirmware != 0) {
            const auto load = engineSlot(probeEngine, 0x338);
            if (load && __atomic_load_n(&probeFwCount, __ATOMIC_RELAXED) == 0) {
                __atomic_add_fetch(&probeFwCount, 1U, __ATOMIC_RELAXED);
                const auto fw = reinterpret_cast<bool (*)(void*)>(load)(probeEngine);
                NRed::singleton().setProp32("NRedVCN1ProbeFirmware", fw);
                SYSLOG("VCN1", "probe loadUVDFW result=%u", fw);
            }
        }
        const bool firmwareFirst = skipFirmware != 0;
        if (firmwareFirst) { static_cast<UInt8*>(probeEngine)[0x25D] = 0; }
        auto result = init ? reinterpret_cast<bool (*)(void*)>(init)(probeEngine) : false;
        if (firmwareFirst) { static_cast<UInt8*>(probeEngine)[0x25D] = skipFirmware; }
        NRed::singleton().setProp32("NRedVCN1ProbeFirmwareFirst", firmwareFirst ? 1 : 0);
        NRed::singleton().setProp32("NRedVCN1ProbeInitResult", result);
        SYSLOG("VCN1", "probe initHardware result=%u", result);
        return result;
    }

    // 每个引擎在 +0x254 记录 isEncode；按它分开发布结果，避免后一次调用覆盖前一次。
    void setEngineProp(const void* self, const char* decode, const char* encode, const UInt32 value)
    {
        UInt32 isEncode{0};
        if (self) { memcpy(&isEncode, static_cast<const UInt8*>(self) + 0x254, sizeof(isEncode)); }
        NRed::singleton().setProp32((isEncode & 1) ? encode : decode, value);
    }

    bool uvdEngineStart(void* self)
    {
        const auto result = reinterpret_cast<bool (*)(void*)>(engineStartOriginal)(self);
        setEngineProp(self, "NRedVCN1StartDec", "NRedVCN1StartEnc", result);
        SYSLOG("VCN1", "UVDHWEngine::start result=%u", result);
        return result;
    }

    bool uvdPowerOnOff(void* self, const bool on)
    {
        const auto result = reinterpret_cast<bool (*)(void*, bool)>(enginePowerOriginal)(self, on);
        static UInt32 calls{0};
        const auto    count = __atomic_fetch_add(&calls, 1U, __ATOMIC_RELAXED) + 1;
        NRed::singleton().setProp32("NRedVCN1PowerOnOffCalls", count);
        NRed::singleton().setProp32("NRedVCN1PowerOnOffLastOn", on ? 1 : 0);
        NRed::singleton().setProp32("NRedVCN1PowerOnOffLastResult", result ? 1 : 0);
        setEngineProp(self, "NRedVCN1PowerDec", "NRedVCN1PowerEnc", (on ? 2U : 0U) | (result ? 1U : 0U));
        SYSLOG("VCN1", "UVDHWEngine::powerOnOff on=%u result=%u call=%u", on, result, count);
        return result;
    }

    bool uvdAllocateRings(void* self)
    {
        const auto result = reinterpret_cast<bool (*)(void*)>(engineRingsOriginal)(self);
        setEngineProp(self, "NRedVCN1RingsDec", "NRedVCN1RingsEnc", result);
        return result;
    }

    bool uvdAllocateChannels(void* self)
    {
        const auto result = reinterpret_cast<bool (*)(void*)>(engineChannelsOriginal)(self);
        setEngineProp(self, "NRedVCN1ChannelsDec", "NRedVCN1ChannelsEnc", result);
        return result;
    }

    // initHardware 的两个子步骤：固件下载（SML/PSP）与 CAIL 硬件初始化。
    bool uvd7LoadUVDFW(void* self)
    {
        const auto result = reinterpret_cast<bool (*)(void*)>(uvd7LoadUVDFWOriginal)(self);
        NRed::singleton().setProp32("NRedVCN1LoadUVDFW", result);
        auto count = __atomic_fetch_add(&uvd7LoadUVDFWCount, 1U, __ATOMIC_RELAXED);
        NRed::singleton().setProp32("NRedVCN1LoadUVDFWCalls", count + 1);
        if (count == 0) { NRed::singleton().setProp32("NRedVCN1LoadUVDFWFirst", result); }
        if (count == 1) { NRed::singleton().setProp32("NRedVCN1LoadUVDFWSecond", result); }
        if (count < 16) { SYSLOG("VCN1", "loadUVDFW result=%u", result); }
        return result;
    }
    bool uvd7InitHardwareCail(void* self, const bool flag)
    {
        void* interface{nullptr};
        memcpy(&interface, static_cast<UInt8*>(self) + 0x18, sizeof(interface));
        attachCailSubmit(interface);
        // +0x280=固件 GPU 地址(u64)、+0x29C=环数量、+0x2B8=环描述数组指针（UVD7 已核对）。
        UInt64 firmware{0};
        UInt32 ringCount{0}, ringArray{0};
        memcpy(&firmware, static_cast<UInt8*>(self) + 0x280, sizeof(firmware));
        memcpy(&ringCount, static_cast<UInt8*>(self) + 0x29C, sizeof(ringCount));
        memcpy(&ringArray, static_cast<UInt8*>(self) + 0x2B8, sizeof(ringArray));
        NRed::singleton().setProp32("NRedVCN1FirmwareAddress", static_cast<UInt32>(firmware));
        NRed::singleton().setProp32("NRedVCN1RingCount", ringCount);
        NRed::singleton().setProp32("NRedVCN1RingArrayLow", ringArray);
        UInt32 isEncode{0};
        memcpy(&isEncode, static_cast<UInt8*>(self) + 0x254, sizeof(isEncode));
        cailEncode = isEncode & 1;
        auto result = reinterpret_cast<bool (*)(void*, bool)>(uvd7InitHardwareCailOriginal)(self, flag);
        NRed::singleton().setProp32("NRedVCN1CailFlag", flag);
        // flag=1 会在共享下载序列前多提交一条消息类，APU 侧的 CAIL 客户端把它判为不支持；
        // 退回 flag=0（跳过该类消息）再走一次，判断失败是否只来自这一条。
        if (!result && vcn1Device()) {
            NRed::singleton().setProp32("NRedVCN1CailRetry", 1);
            cailRetryActive = true;
            result          = reinterpret_cast<bool (*)(void*, bool)>(uvd7InitHardwareCailOriginal)(self, false);
            cailRetryActive = false;
            NRed::singleton().setProp32("NRedVCN1CailRetryResult", result);
            NRed::singleton().setProp32(isEncode & 1 ? "NRedVCN1CailRetryFailedEnc" : "NRedVCN1CailRetryFailedDec",
                                        cailRetryFailed[isEncode & 1]);
        }
        // 门控之前的真实结果：即使下面把 start 挡掉，也能判断 CAIL init 本身是否成功。
        NRed::singleton().setProp32("NRedVCN1InitHardwareCailRaw", result);
        // 在固件先行诊断确认 UVD 块真正进入 idle 之前，不让 initHardware 成功、引擎不 start，
        // 避免再次向未初始化的 UVDDec 环提交命令导致 GPU FullAsicReset。确认后再用 boot-arg 打开。
        const bool allowEngineStart = checkKernelArgument("-NRedVCNAllowStart");
        if (!allowEngineStart && result) {
            NRed::singleton().setProp32("NRedVCN1CailStartBlocked", 1);
            result = false;
        }
        NRed::singleton().setProp32("NRedVCN1InitHardwareCail", result);
        (isEncode ? cailEncResult : cailDecResult) = result;
        NRed::singleton().setProp32("NRedVCN1CailDec", cailDecResult);
        NRed::singleton().setProp32("NRedVCN1CailEnc", cailEncResult);
        NRed::singleton().setProp32("NRedVCN1CailDecFailed", cailFailed[0]);
        NRed::singleton().setProp32("NRedVCN1CailEncFailed", cailFailed[1]);
        auto count = __atomic_fetch_add(&uvd7InitHardwareCailCount, 1U, __ATOMIC_RELAXED);
        if (count < 16) { SYSLOG("VCN1", "initHardwareCail flag=%u result=%u", flag, result); }
        return result;
    }

    UInt8 accelVideoContextStart(void* self)
    {
        using Function    = UInt8 (*)(void*);
        auto  original    = reinterpret_cast<Function>(accelVideoContextStartOriginal);
        const auto result = original(self);
        NRed::singleton().setProp32("NRedVCN1AccelVideoStart", result);
        if (result) { probeHardwareInit(); }
        auto count = __atomic_fetch_add(&accelVideoContextStartCount, 1U, __ATOMIC_RELAXED);
        if (count < 8) { SYSLOG("VCN1", "AccelVideoContext::contextStart result=%u", result); }
        return result;
    }

    // Vega10VideoContext::contextStart 只在 hardware+0xB4/0xB5/0xB6（hasUVD0/1/VCE）为 1 时
    // 才把 UVD0/UVD1/VCE 队列建到 +0x1240/+0x1248/+0x1250；队列为空时 getCapabilities 全 0。
    UInt8 videoContextStart(void* self)
    {
        using Function    = UInt8 (*)(void*);
        auto  original    = reinterpret_cast<Function>(videoContextStartOriginal);
        const auto result = original(self);
        if (self) {
            void *uvd0{nullptr}, *uvd1{nullptr}, *vce{nullptr};
            memcpy(&uvd0, static_cast<UInt8*>(self) + 0x1240, sizeof(uvd0));
            memcpy(&uvd1, static_cast<UInt8*>(self) + 0x1248, sizeof(uvd1));
            memcpy(&vce, static_cast<UInt8*>(self) + 0x1250, sizeof(vce));
            NRed::singleton().setProp32("NRedVCN1VcQueueUVD0", uvd0 != nullptr);
            NRed::singleton().setProp32("NRedVCN1VcQueueUVD1", uvd1 != nullptr);
            NRed::singleton().setProp32("NRedVCN1VcQueueVCE", vce != nullptr);
        }
        NRed::singleton().setProp32("NRedVCN1VideoCtxStart", result);
        auto count = __atomic_fetch_add(&videoContextStartCount, 1U, __ATOMIC_RELAXED);
        if (count < 8) { SYSLOG("VCN1", "Vega10VideoContext::contextStart result=%u", result); }
        return result;
    }

    // caps 数组：0..3=UVD0 环能力、4..7=UVD1、10..11=VCE；前两组分别取自 +0x1240/+0x1248 上的队列。
    UInt32 videoGetCapabilities(void* self, void* caps, UInt64* out)
    {
        using Function    = UInt32 (*)(void*, void*, UInt64*);
        auto  original    = reinterpret_cast<Function>(videoGetCapabilitiesOriginal);
        probeHardwareInit();
        const auto result = original(self, caps, out);
        if (caps) {
            UInt64 words[12];
            memcpy(words, caps, sizeof(words));
            NRed::singleton().setProp32("NRedVCN1CapUVD0", static_cast<UInt32>(words[0]));
            NRed::singleton().setProp32("NRedVCN1CapUVD1", static_cast<UInt32>(words[4]));
            NRed::singleton().setProp32("NRedVCN1CapVCE", static_cast<UInt32>(words[10]));
        }
        NRed::singleton().setProp32("NRedVCN1CapResult", result);
        auto count = __atomic_fetch_add(&videoGetCapabilitiesCount, 1U, __ATOMIC_RELAXED);
        if (count < 8) { SYSLOG("VCN1", "getCapabilities result=0x%X", result); }
        return result;
    }

    // createInfo+4=codec、+8=类型（<6 为解码）；返回值决定用户态是否拿到硬件上下文。
    UInt32 videoNewContext(void* self, const void* createInfo, void* returnInfo, const UInt64 argument, UInt64* out)
    {
        using Function    = UInt32 (*)(void*, const void*, void*, UInt64, UInt64*);
        auto  original    = reinterpret_cast<Function>(videoNewContextOriginal);
        UInt32 type{0}, codec{0};
        if (createInfo) {
            memcpy(&type, static_cast<const UInt8*>(createInfo) + 8, sizeof(type));
            memcpy(&codec, static_cast<const UInt8*>(createInfo) + 4, sizeof(codec));
        }
        const auto result = original(self, createInfo, returnInfo, argument, out);
        NRed::singleton().setProp32("NRedVCN1NewCtxType", type);
        NRed::singleton().setProp32("NRedVCN1NewCtxCodec", codec);
        NRed::singleton().setProp32("NRedVCN1NewCtxStatus", result);
        auto count = __atomic_fetch_add(&videoNewContextCount, 1U, __ATOMIC_RELAXED);
        if (count < 8) { SYSLOG("VCN1", "video newContext type=%u codec=%u result=0x%X", type, codec, result); }
        return result;
    }

    bool uvdContextInit(void* self, void* accelerator, void* videoContext, const void* createInfo)
    {
        using Function    = bool (*)(void*, void*, void*, const void*);
        auto  original    = reinterpret_cast<Function>(uvdContextInitOriginal);
        const auto result = original(self, accelerator, videoContext, createInfo);
        NRed::singleton().setProp32("NRedVCN1UvdCtxInit", result);
        auto count = __atomic_fetch_add(&uvdContextInitCount, 1U, __ATOMIC_RELAXED);
        if (count < 8) { SYSLOG("VCN1", "AccelUVDContext::init result=%u", result); }
        return result;
    }

    bool uvdRequestCapability(void* self, void* returnInfo)
    {
        using Function    = bool (*)(void*, void*);
        auto  original    = reinterpret_cast<Function>(uvdRequestCapabilityOriginal);
        const auto result = original(self, returnInfo);
        UInt32 mode{0};
        if (returnInfo) { memcpy(&mode, returnInfo, sizeof(mode)); }
        NRed::singleton().setProp32("NRedVCN1ReqCapMode", mode);
        NRed::singleton().setProp32("NRedVCN1ReqCap", result);
        auto count = __atomic_fetch_add(&uvdRequestCapabilityCount, 1U, __ATOMIC_RELAXED);
        if (count < 8) { SYSLOG("VCN1", "requestCapability mode=%u result=%u", mode, result); }
        return result;
    }

    // IOUserClient::getTargetAndMethodForIndex：selector 0x100..0x111 都从这里分派，
    // 记录 driver 实际请求过的 selector 位图，直接指出它在 contextStart 之后停在哪一步。
    void* accelDispatch(void* self, void** target, UInt32 index)
    {
        using Function = void* (*)(void*, void**, UInt32);
        if (index >= 0x100 && index <= 0x111) {
            accelDispatchSeen |= 1U << (index - 0x100);
            NRed::singleton().setProp32("NRedVCN1SelSeen", accelDispatchSeen);
            NRed::singleton().setProp32("NRedVCN1SelLast", index);
        }
        const auto count = __atomic_fetch_add(&accelDispatchCount, 1U, __ATOMIC_RELAXED);
        NRed::singleton().setProp32("NRedVCN1SelCount", count + 1);
        // 用户客户端的第一次分派已经在解码会话之外，把硬件初始化提前跑掉，结论不再依赖播放。
        if (count < 4) { probeHardwareInit(); }
        if (count < 24) { SYSLOG("VCN1", "getTargetAndMethodForIndex index=0x%X", index); }
        return reinterpret_cast<Function>(accelDispatchOriginal)(self, target, index);
    }

    // getHWInfo 把硬件能力结构拷给 AppleGVA；status 与关键字决定 driver 是否继续建解码上下文。
    UInt32 accelGetHWInfo(void* self, void* info)
    {
        using Function    = UInt32 (*)(void*, void*);
        const auto result = reinterpret_cast<Function>(accelHWInfoOriginal)(self, info);
        NRed::singleton().setProp32("NRedVCN1HWInfoStatus", result);
        if (info) {
            UInt32 first{0}, mid{0}, tail{0};
            memcpy(&first, static_cast<UInt8*>(info) + 0x00, sizeof(first));
            memcpy(&mid, static_cast<UInt8*>(info) + 0x40, sizeof(mid));
            memcpy(&tail, static_cast<UInt8*>(info) + 0x44, sizeof(tail));
            NRed::singleton().setProp32("NRedVCN1HWInfoFirst", first);
            NRed::singleton().setProp32("NRedVCN1HWInfoMid", mid);
            NRed::singleton().setProp32("NRedVCN1HWInfoTail", tail);
        }
        auto count = __atomic_fetch_add(&accelHWInfoCount, 1U, __ATOMIC_RELAXED);
        if (count < 8) { SYSLOG("VCN1", "getHWInfo status=0x%X count=%u", result, count + 1); }
        return result;
    }

    // VCN1 守卫与钩子：UVD7 虚表槽位与具名符号一致后，才接管 SML 基址查询（选择子 0x0C）。
    bool prepareVCN1(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
    {
        mach_vm_address_t engineInit{0}, engineInitHardware{0}, engineVTable{0}, smlVTable{0}, smlVTableBase{0};
        mach_vm_address_t engineLoadUVDFW{0}, engineInitHardwareCail{0}, externalInterfacesFunction{0};
        mach_vm_address_t engineStart{0}, enginePower{0}, engineRings{0}, engineChannels{0};
        mach_vm_address_t accelVideoStartFunction{0}, videoStartFunction{0};
        mach_vm_address_t videoCapsFunction{0}, videoNewContextFunction{0};
        mach_vm_address_t uvdContextInitFunction{0}, uvdRequestCapabilityFunction{0};
        mach_vm_address_t accelDispatchFunction{0}, accelHWInfoFunction{0};
        bool ok = resolve(patcher, id, "__ZN30AMDRadeonX5000_AMDUVD7HWEngine10gMetaClassE", uvd7EngineMC, slide,
                          size);
        ok &= resolve(patcher, id,
                      "__ZN30AMDRadeonX5000_AMDUVD7HWEngine4initEP30AMDRadeonX5000_IAMDHWInterface20_eAMD_HW_ENGINE_"
                      "TYPE",
                      engineInit, slide, size);
        ok &= resolve(patcher, id, "__ZN30AMDRadeonX5000_AMDUVD7HWEngine12initHardwareEv", engineInitHardware, slide,
                      size);
        ok &= resolve(patcher, id, "__ZN29AMDRadeonX5000_AMDUVDHWEngine9loadUVDFWEv", engineLoadUVDFW, slide, size);
        mach_vm_address_t fwMessageFunction{0};
        resolve(patcher, id, "__ZN30AMDRadeonX5000_AMDUVD7HWEngine14commitUVDFWMsgEP12_SML_UVD_MSGyyy",
                fwMessageFunction, slide, size);
        ok &= resolve(patcher, id, "__ZN29AMDRadeonX5000_AMDUVDHWEngine16initHardwareCailEb", engineInitHardwareCail,
                      slide, size);
        ok &= resolve(patcher, id, "__ZTV30AMDRadeonX5000_AMDUVD7HWEngine", engineVTable, slide, size);
        uvd7VTable = engineVTable;
        ok &= resolve(patcher, id, "__ZTV34AMDRadeonX5000_AMDSMLGFX9Interface", smlVTable, slide, size);
        // 原生 createSMLInterface 按配置创建 GFX9 或基类实例；两张虚表都要接管。
        resolve(patcher, id, "__ZTV30AMDRadeonX5000_AMDSMLInterface", smlVTableBase, slide, size);
        // 引擎启动/上电/环与通道分配：定位失败点在 CAIL 之前还是之内。
        resolve(patcher, id, "__ZN29AMDRadeonX5000_AMDUVDHWEngine5startEv", engineStart, slide, size);
        resolve(patcher, id, "__ZN29AMDRadeonX5000_AMDUVDHWEngine10powerOnOffEb", enginePower, slide, size);
        resolve(patcher, id, "__ZN30AMDRadeonX5000_AMDUVD7HWEngine15allocateHWRingsEv", engineRings, slide, size);
        resolve(patcher, id, "__ZN30AMDRadeonX5000_AMDUVD7HWEngine18allocateHWChannelsEv", engineChannels, slide,
                size);
        // +0x338 的 SML 由该函数挂载；运行期 vptr 接管依赖它，取不到也不阻断静态路径。
        resolve(patcher, id, "__ZN26AMDRadeonX5000_AMDHardware28initializeExternalInterfacesEv",
                externalInterfacesFunction, slide, size);
        // 视频上下文链：上下文启动、能力查询与 UVD 上下文初始化（加速器闸门走已有的 newVideo 路由）。
        resolve(patcher, id, "__ZN35AMDRadeonX5000_AMDAccelVideoContext12contextStartEv", accelVideoStartFunction,
                slide, size);
        resolve(patcher, id, "__ZN36AMDRadeonX5000_AMDVega10VideoContext12contextStartEv", videoStartFunction, slide,
                size);
        resolve(patcher, id, "__ZN36AMDRadeonX5000_AMDVega10VideoContext15getCapabilitiesEP22sAMDHWEngineCapabilityPy",
                videoCapsFunction, slide, size);
        resolve(patcher, id,
                "__ZN36AMDRadeonX5000_AMDVega10VideoContext10newContextEP23sVideoContextCreateInfoP23sVideoContextReturnInfoyPy",
                videoNewContextFunction, slide, size);
        resolve(patcher, id,
                "__ZN33AMDRadeonX5000_AMDAccelUVDContext4initEP37AMDRadeonX5000_AMDGraphicsAcceleratorP35AMDRadeonX5000_AMDAccelVideoContextP23sVideoContextCreateInfo",
                uvdContextInitFunction, slide, size);
        resolve(patcher, id, "__ZN33AMDRadeonX5000_AMDAccelUVDContext17requestCapabilityEP23sVideoContextReturnInfo",
                uvdRequestCapabilityFunction, slide, size);
        // 用户客户端分派与能力回报：定位 driver 在 contextStart 之后的请求序列。
        resolve(patcher, id, "__ZN35AMDRadeonX5000_AMDAccelVideoContext26getTargetAndMethodForIndexEPP9IOServicej",
                accelDispatchFunction, slide, size);
        resolve(patcher, id, "__ZN35AMDRadeonX5000_AMDAccelVideoContext9getHWInfoEP13sHardwareInfo",
                accelHWInfoFunction, slide, size);
        if (!ok) { return false; }
        // __ZTV 的两个头部槽不属于槽位地址；init=+0x118、initHardware=+0x368（24H32 已核对）。
        const auto slot = [engineVTable, slide, size](const UInt32 offset) -> mach_vm_address_t {
            if (engineVTable < slide || offset > size || engineVTable + 16 + offset > slide + size - sizeof(void*)) {
                return 0;
            }
            mach_vm_address_t target{0};
            memcpy(&target, reinterpret_cast<void*>(engineVTable + 16 + offset), sizeof(target));
            return target;
        };
        if (slot(0x118) != engineInit || slot(0x368) != engineInitHardware) {
            SYSLOG("VCN1", "UVD7 vtable mismatch: init=%p/%p hw=%p/%p", reinterpret_cast<void*>(slot(0x118)),
                   reinterpret_cast<void*>(engineInit), reinterpret_cast<void*>(slot(0x368)),
                   reinterpret_cast<void*>(engineInitHardware));
            return false;
        }
        const auto hookSmlSlot = [slide, size](const mach_vm_address_t table, const mach_vm_address_t hook,
                                               mach_vm_address_t& original) -> bool {
            const mach_vm_address_t slotAddress = table + 16 + 0x58;
            if (table < slide || slotAddress > slide + size - sizeof(void*)) { return false; }
            memcpy(&original, reinterpret_cast<void*>(slotAddress), sizeof(original));
            if (original == 0) { return false; }
            memcpy(reinterpret_cast<void*>(slotAddress), &hook, sizeof(hook));
            return true;
        };
        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) { return false; }
        const bool smlGFX9 = hookSmlSlot(smlVTable, reinterpret_cast<mach_vm_address_t>(&smlQueryGFX9),
                                         smlQueryOriginalGFX9);
        const bool smlBase = smlVTableBase != 0
                                 ? hookSmlSlot(smlVTableBase, reinterpret_cast<mach_vm_address_t>(&smlQueryPlain),
                                               smlQueryOriginalBase)
                                 : false;
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        if (!smlGFX9 && !smlBase) { return false; }
        NRed::singleton().setProp32("NRedVCN1SMLGFX9Hooked", smlGFX9);
        NRed::singleton().setProp32("NRedVCN1SMLBaseHooked", smlBase);
        // 写回校验：只读页或写保护失效时，ioreg 立刻可见，不必再猜。
        mach_vm_address_t smlGFX9Now{0}, smlBaseNow{0};
        memcpy(&smlGFX9Now, reinterpret_cast<void*>(smlVTable + 16 + 0x58), sizeof(smlGFX9Now));
        if (smlVTableBase != 0) {
            memcpy(&smlBaseNow, reinterpret_cast<void*>(smlVTableBase + 16 + 0x58), sizeof(smlBaseNow));
        }
        NRed::singleton().setProp32("NRedVCN1SMLGFX9Verify",
                                    smlGFX9Now == reinterpret_cast<mach_vm_address_t>(&smlQueryGFX9));
        NRed::singleton().setProp32("NRedVCN1SMLBaseVerify",
                                    smlBaseNow == reinterpret_cast<mach_vm_address_t>(&smlQueryPlain));
        // init/initHardware/loadUVDFW/initHardwareCail 路由仅作诊断，失败不影响接入。
        KernelPatcher::RouteRequest routes[] = {
            {"__ZN30AMDRadeonX5000_AMDUVD7HWEngine4initEP30AMDRadeonX5000_IAMDHWInterface20_eAMD_HW_ENGINE_TYPE",
             uvd7Init, uvd7InitOriginal},
            {"__ZN30AMDRadeonX5000_AMDUVD7HWEngine12initHardwareEv", uvd7InitHardware, uvd7InitHardwareOriginal},
            {"__ZN29AMDRadeonX5000_AMDUVDHWEngine9loadUVDFWEv", uvd7LoadUVDFW, uvd7LoadUVDFWOriginal},
            {"__ZN29AMDRadeonX5000_AMDUVDHWEngine16initHardwareCailEb", uvd7InitHardwareCail,
             uvd7InitHardwareCailOriginal},
        };
        if (!patcher.routeMultiple(id, routes, arrsize(routes), slide, size)) { patcher.clearError(); }
        if (fwMessageFunction != 0) {
            KernelPatcher::RouteRequest route{
                "__ZN30AMDRadeonX5000_AMDUVD7HWEngine14commitUVDFWMsgEP12_SML_UVD_MSGyyy", uvd7CommitFWMessage,
                fwMessageOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        if (externalInterfacesFunction != 0) {
            KernelPatcher::RouteRequest route{"__ZN26AMDRadeonX5000_AMDHardware28initializeExternalInterfacesEv",
                                              externalInterfaces, externalInterfacesOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        if (engineStart != 0) {
            KernelPatcher::RouteRequest route{"__ZN29AMDRadeonX5000_AMDUVDHWEngine5startEv", uvdEngineStart,
                                              engineStartOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        if (enginePower != 0) {
            KernelPatcher::RouteRequest route{"__ZN29AMDRadeonX5000_AMDUVDHWEngine10powerOnOffEb", uvdPowerOnOff,
                                              enginePowerOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        if (engineRings != 0) {
            KernelPatcher::RouteRequest route{"__ZN30AMDRadeonX5000_AMDUVD7HWEngine15allocateHWRingsEv",
                                              uvdAllocateRings, engineRingsOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        if (engineChannels != 0) {
            KernelPatcher::RouteRequest route{"__ZN30AMDRadeonX5000_AMDUVD7HWEngine18allocateHWChannelsEv",
                                              uvdAllocateChannels, engineChannelsOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        if (accelVideoStartFunction != 0) {
            KernelPatcher::RouteRequest route{"__ZN35AMDRadeonX5000_AMDAccelVideoContext12contextStartEv",
                                              accelVideoContextStart, accelVideoContextStartOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        if (videoStartFunction != 0) {
            KernelPatcher::RouteRequest route{"__ZN36AMDRadeonX5000_AMDVega10VideoContext12contextStartEv",
                                              videoContextStart, videoContextStartOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        if (videoCapsFunction != 0) {
            KernelPatcher::RouteRequest route{
                "__ZN36AMDRadeonX5000_AMDVega10VideoContext15getCapabilitiesEP22sAMDHWEngineCapabilityPy",
                videoGetCapabilities, videoGetCapabilitiesOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        if (videoNewContextFunction != 0) {
            KernelPatcher::RouteRequest route{
                "__ZN36AMDRadeonX5000_AMDVega10VideoContext10newContextEP23sVideoContextCreateInfoP23sVideoContextReturnInfoyPy",
                videoNewContext, videoNewContextOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        if (uvdContextInitFunction != 0) {
            KernelPatcher::RouteRequest route{
                "__ZN33AMDRadeonX5000_AMDAccelUVDContext4initEP37AMDRadeonX5000_AMDGraphicsAcceleratorP35AMDRadeonX5000_AMDAccelVideoContextP23sVideoContextCreateInfo",
                uvdContextInit, uvdContextInitOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        if (uvdRequestCapabilityFunction != 0) {
            KernelPatcher::RouteRequest route{
                "__ZN33AMDRadeonX5000_AMDAccelUVDContext17requestCapabilityEP23sVideoContextReturnInfo",
                uvdRequestCapability, uvdRequestCapabilityOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        if (accelDispatchFunction != 0) {
            KernelPatcher::RouteRequest route{
                "__ZN35AMDRadeonX5000_AMDAccelVideoContext26getTargetAndMethodForIndexEPP9IOServicej", accelDispatch,
                accelDispatchOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        if (accelHWInfoFunction != 0) {
            KernelPatcher::RouteRequest route{"__ZN35AMDRadeonX5000_AMDAccelVideoContext9getHWInfoEP13sHardwareInfo",
                                              accelGetHWInfo, accelHWInfoOriginal};
            if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
        }
        return true;
    }

    bool install(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
    {
        if (size < 6 || slide > ~mach_vm_address_t{0} - size) { return false; }
        size_t      patchCount{0};
        const auto* patches = VCNKernelProfiles::patches(currentKernelVersion().major(), patchCount);
        if (!patches || patchCount == 0 || patchCount > 32) { return false; }
        mach_vm_address_t addresses[32]{};
        mach_vm_address_t signal{0};
        if (!resolve(patcher, id, "__ZN26AMDRadeonX6000_AMDHardware22signalGPUWorkSubmittedEv", signal, slide, size)) {
            return false;
        }
        for (size_t i = 0; i < patchCount; ++i) {
            const auto&       patch = patches[i];
            mach_vm_address_t function{0};
            if (!resolve(patcher, id, patch.symbol, function, slide, size)) { return false; }
            addresses[i] = function + patch.offset;
            if (patch.newSlot == 0) {
                const auto next = addresses[i] + 5;
                if (next < addresses[i]
                    || (signal >= next ? signal - next > 0x7FFFFFFFULL : next - signal > 0x80000000ULL))
                {
                    return false;
                }
            }
            if (function > ~mach_vm_address_t{0} - patch.offset || addresses[i] < slide
                || addresses[i] > slide + size - sizeof(patch.original)
                || memcmp(reinterpret_cast<void*>(addresses[i]), patch.original, sizeof(patch.original)))
            {
                SYSLOG("VCN", "kernel code mismatch: %s+0x%X", patch.symbol, patch.offset);
                return false;
            }
        }
        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) { return false; }
        for (size_t i = 0; i < patchCount; ++i) {
            const auto& patch = patches[i];
            UInt8       replacement[6];
            memcpy(replacement, patch.original, sizeof(replacement));
            if (patch.newSlot) { memcpy(replacement + 2, &patch.newSlot, sizeof(patch.newSlot)); }
            else {
                const auto displacement = static_cast<SInt32>(signal - addresses[i] - 5);
                replacement[0]          = 0xE8;
                memcpy(replacement + 1, &displacement, sizeof(displacement));
                replacement[5] = 0x90;
            }
            memcpy(reinterpret_cast<void*>(addresses[i]), replacement, sizeof(replacement));
        }
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        KernelPatcher::RouteRequest routes[] = {
            {"__ZN26AMDRadeonX6000_AMDHardware22signalGPUWorkSubmittedEv", signalWork, signalOriginal},
            {"__ZN39AMDRadeonX6000_AMDAccelSharedUserClient5startEP9IOService", clientStart, clientStart6000},
            {"__ZN39AMDRadeonX6000_AMDAccelSharedUserClient4stopEP9IOService", clientStop, clientStop6000},
            {"__ZN29AMDRadeonX6000_AMDAccelShared11SurfaceCopyEPjyP12IOAccelEvent", surfaceCopy, copy6000},
            {"__ZN30AMDRadeonX6000_AMDVCN2HWEngine12initHardwareE21eAMDAccelVideoChannel", initHardware,
             initHardwareOriginal},
        };
        if (!patcher.routeMultiple(id, routes, arrsize(routes), slide, size)) {
            patcher.clearError();
            if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                for (size_t i = 0; i < patchCount; ++i) {
                    memcpy(reinterpret_cast<void*>(addresses[i]), patches[i].original, 6);
                }
                MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
            }
            return false;
        }
        return true;
    }

}    // namespace

void VCN::init()
{
    requested = checkKernelArgument("-NRedVideoFix");
    // X6000 仅 VCN2.2 设备需要可选加载；注册保持不变，加载决策在 acceleratorStart 内按设备分流。
    if (requested) { lilu.onKextLoadForce(&x6000); }
}
bool VCN::ready()
{
    return supported() && !vcn1Device() && __atomic_load_n(&loadCompleted, __ATOMIC_ACQUIRE) && has5000 && has6000
           && hooks;
}
bool  VCN::allocated() { return ready() && engineAllocated; }
void* VCN::allocateEngine()
{
    if (!ready()) {
        NRed::singleton().setProp32("NRedVCNAllocationSkipped", requested);
        return nullptr;
    }
    auto* engine    = engineClass->alloc();
    engineAllocated = engine != nullptr;
    if (!engineAllocated) { __atomic_store_n(&loadCompleted, false, __ATOMIC_RELEASE); }
    NRed::singleton().setProp32("NRedVCNEngineAllocated", engineAllocated);
    return engine;
}

bool VCN::vcn1Ready() { return supported() && vcn1Device() && vcn1Prepared && uvd7EngineMC != nullptr; }
void* VCN::allocateUVD7Engine()
{
    if (!vcn1Ready()) {
        NRed::singleton().setProp32("NRedVCN1AllocationSkipped", requested);
        return nullptr;
    }
    return uvd7EngineMC->alloc();
}

void VCN::processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size)
{
    if (!supported()) { return; }
    const auto old = id == kextRadeonX5000.loadIndex;
    if (!old && id != x6000.loadIndex) { return; }
    if (!verifyKernelABI(patcher, id, slide, size, old)) {
        if (old) { has5000 = false; }
        else {
            has6000 = hooks = false;
        }
        __atomic_store_n(&loadCompleted, false, __ATOMIC_RELEASE);
        NRed::singleton().setProp32("NRedVCNKernelABIRejected", 1);
        NRed::singleton().setProp32(old ? "NRedVCNX5000Prepared" : "NRedVCNX6000Prepared", 0);
        SYSLOG("VCN", "%s ABI mismatch; bridge disabled", old ? "X5000" : "X6000");
        return;
    }
    NRed::singleton().setProp32(old ? "NRedVCNX5000ABIValidated" : "NRedVCNX6000ABIValidated", 1);
    bool ok = true;
    for (size_t i = 0; i < arrsize(classes); ++i) {
        char name[112];
        snprintf(name, sizeof(name), "__ZN%uAMDRadeonX%s_%s10gMetaClassE", lengths[i], old ? "5000" : "6000",
                 classes[i]);
        auto& target  = old ? pairs[i].oldClass : pairs[i].newClass;
        ok           &= resolve(patcher, id, name, target, slide, size);
    }
        if (old) {
            ok &= resolve(patcher, id, "__ZN39AMDRadeonX5000_AMDAccelSharedUserClient5startEP9IOService", clientStart5000,
                          slide, size);
            ok &= resolve(patcher, id, "__ZN39AMDRadeonX5000_AMDAccelSharedUserClient4stopEP9IOService", clientStop5000,
                          slide, size);
            ok &= resolve(patcher, id, "__ZN29AMDRadeonX5000_AMDAccelShared11SurfaceCopyEPjyP12IOAccelEvent", copy5000,
                          slide, size);
        // Sequoia (24H32) 的正确前缀是 30；旧构建可能是 29，两者都尝试。
        ok &= resolve(patcher, id, "__ZN30AMDRadeonX5000_AMDGFX9Hardware15notifyGfxAccessEv", notifyAccess, slide, size)
              || resolve(patcher, id, "__ZN29AMDRadeonX5000_AMDGFX9Hardware15notifyGfxAccessEv", notifyAccess, slide,
                         size);
            KernelPatcher::RouteRequest routes[] = {
                {"__ZN37AMDRadeonX5000_AMDGraphicsAccelerator5startEP9IOService", acceleratorStart,
                 acceleratorStartOriginal},
                {"__ZN37AMDRadeonX5000_AMDGraphicsAccelerator9newSharedEv", newShared, newSharedOriginal},
                {"__ZN37AMDRadeonX5000_AMDGraphicsAccelerator19newSharedUserClientEv", newClient, newClientOriginal},
                {"__ZN41AMDRadeonX5000_AMDGFX9GraphicsAccelerator15newVideoContextEv", newVideo, newVideoOriginal},
                {"__ZN31AMDRadeonX5000_IAMDSMLInterface18createSMLInterfaceEj", createSML, sml5000},
            };
            if (ok) {
                ok = patcher.routeMultiple(id, routes, arrsize(routes), slide, size);
                patcher.clearError();
            }
            has5000 = ok;
            if (ok) {
                KernelPatcher::RouteRequest route{"__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass", safeCast,
                                                  safeCastOriginal};
                ok = patcher.routeMultipleLong(KernelPatcher::KernelID, &route, 1);
                patcher.clearError();
                has5000 = ok;
            }
            if (vcn1Device()) {
                // VCN1：X5000 原生 UVD7 引擎与 SML 基址查询；与 VCN2 桥接互不影响。
                vcn1Prepared = prepareVCN1(patcher, id, slide, size);
                NRed::singleton().setProp32("NRedVCN1Prepared", vcn1Prepared);
                if (!vcn1Prepared) { NRed::singleton().setProp32("NRedVCN1AbiRejected", 1); }
                SYSLOG("VCN", "VCN1 prepared=%u", vcn1Prepared);
            }
        }
    else {
        ok &= resolve(patcher, id, "__ZN30AMDRadeonX6000_AMDVCN2HWEngine10gMetaClassE", engineClass, slide, size);
        ok &= resolve(patcher, id, "__ZN29AMDRadeonX6000_AMDAccelShared10gMetaClassE", sharedClass, slide, size);
        ok &= resolve(patcher, id, "__ZN39AMDRadeonX6000_AMDAccelSharedUserClient10gMetaClassE", clientClass, slide,
                      size);
        ok &= resolve(patcher, id, "__ZN42AMDRadeonX6000_AMDGFX10GraphicsAccelerator15newVideoContextEv", newVideo6000,
                      slide, size);
        ok &= resolve(patcher, id, "__ZN31AMDRadeonX6000_IAMDSMLInterface18createSMLInterfaceEj", sml6000, slide, size);
        if (ok) { ok = install(patcher, id, slide, size); }
        has6000 = hooks = ok;
        NRed::singleton().setProp32("NRedVCNIdleGfxOffFix", ok);
    }
    NRed::singleton().setProp32(old ? "NRedVCNX5000Prepared" : "NRedVCNX6000Prepared", ok);
    SYSLOG("VCN", "%s prepared=%u ready=%u", old ? "X5000" : "X6000", ok, ready());
}

// CAIL 库层：AmdCailServices::uvdControl 把任何非零返回都归一化成 0xE00002C7，
// 钩到库函数本身才能看到真实原因（1=caps 未启用、2=非法 id、3=适配器未就绪、0xA=不支持）。
static mach_vm_address_t cailUvdControlOriginal{0};

static int cailUvdControlHook(void* adapter, const UInt32 function, void* input, void* output)
{
    // _CAILUvdControl 要求能力位图里 0xD6(word6 bit22) 或 0x15B(word10 bit27) 至少一个置位，
    // 而 Raven/Picasso 的静态 DDI 能力表两位都是 0，只能由驱动在运行期补上；先记为探针。
    if (adapter != nullptr) {
        auto* const words = reinterpret_cast<UInt32*>(static_cast<UInt8*>(adapter) + 0x358);
        if (function >= 1 && function <= 9) {
            // 只记一次「原始值」，避免被下面强制写入后的读数覆盖。
            static bool loggedBase{false};
            if (!loggedBase) {
                loggedBase = true;
                NRed::singleton().setProp32("NRedVCN1CailCapBase6", words[6]);
                NRed::singleton().setProp32("NRedVCN1CailCapBase10", words[10]);
            }
            // Raven/Picasso 的静态 DDI 能力表没有 0xD6(word6 bit22)/0x15B(word10 bit27)，
            // 而 _CAILUvdControl 要求二者之一置位才会走到 _CailUvdInit。
            words[6] |= 1U << 22;
            words[10] |= 1U << 27;
            NRed::singleton().setProp32("NRedVCN1CailCapD6", (words[6] >> 22) & 1U);
            NRed::singleton().setProp32("NRedVCN1CailCap15B", (words[10] >> 27) & 1U);
        }
    }
    const auto result = reinterpret_cast<int (*)(void*, UInt32, void*, void*)>(cailUvdControlOriginal)(adapter, function,
                                                                                                      input, output);
    NRed::singleton().setProp32("NRedVCN1CailLibFunc", function);
    NRed::singleton().setProp32("NRedVCN1CailLibResult", static_cast<UInt32>(result));
    // 按 CAIL 函数号记录调用次数和最后一次返回值，用来区分 init(1)/start(2)/power(3)。
    static UInt32 funcCalls[6]{};
    static UInt32 funcResults[6]{};
    static const char* const funcCallKeys[6] = {
        nullptr, "NRedVCN1CailFunc1Calls", "NRedVCN1CailFunc2Calls", "NRedVCN1CailFunc3Calls",
        "NRedVCN1CailFunc4Calls", "NRedVCN1CailFunc5Calls"};
    static const char* const funcResultKeys[6] = {
        nullptr, "NRedVCN1CailFunc1Result", "NRedVCN1CailFunc2Result", "NRedVCN1CailFunc3Result",
        "NRedVCN1CailFunc4Result", "NRedVCN1CailFunc5Result"};
    if (function >= 1 && function <= 5) {
        const auto calls = __atomic_fetch_add(&funcCalls[function], 1U, __ATOMIC_RELAXED) + 1;
        funcResults[function] = static_cast<UInt32>(result);
        NRed::singleton().setProp32(funcCallKeys[function], calls);
        NRed::singleton().setProp32(funcResultKeys[function], funcResults[function]);
    }
    if (adapter != nullptr) {
        // 能力位图在适配器 +0x358（_CailCapsEnabled 取基址 +0x1E0）：0xD6 = word6 bit22、0x15B = word10 bit27。
        UInt32 words[16]{};
        memcpy(words, static_cast<UInt8*>(adapter) + 0x358, sizeof(words));
        UInt32 set{0};
        for (const auto word : words) { set += static_cast<UInt32>(__builtin_popcount(word)); }
        NRed::singleton().setProp32("NRedVCN1CailCapBits", set);
        NRed::singleton().setProp32("NRedVCN1CailCapD6", (words[6] >> 22) & 1U);
        NRed::singleton().setProp32("NRedVCN1CailCap15B", (words[10] >> 27) & 1U);
    }
    static UInt32 count{0};
    const auto    seen = __atomic_fetch_add(&count, 1U, __ATOMIC_RELAXED);
    if (seen < 16) { SYSLOG("VCN1", "CAIL library uvdControl func=%u result=%d", function, result); }
    return result;
}

// _CailUvdInit(adapter, input)：id 1 的尾跳目标，真正的失败码从这里返回。
static mach_vm_address_t cailUvdInitOriginal{0};
static mach_vm_address_t cailUvdInitSymbol{0};
static mach_vm_address_t cailLibrarySlide{0};
static mach_vm_address_t cailAsicInitStubOriginal{0};
static bool              cailAsicInitStubActive{false};
// HWLibs 的 MMIO 读（定义在文件顶部）：用来在真实 init 前后读 0x83AF（UVD_STATUS）。

// adapter+0x12D8 的替代实现：真实 Greenland init 死在 _Cail_MCILWaitFor（寄存器 0x83AF bit1），
// 先返回成功验证 _CAILUvdControl 后续链路。
static int cailAsicInitReturnZero(void*, void*) { return 0; }

static int cailUvdInitHook(void* adapter, void* input)
{
    static UInt32 initCalls{0};
    const auto    callIndex = __atomic_fetch_add(&initCalls, 1U, __ATOMIC_RELAXED);
    NRed::singleton().setProp32("NRedVCN1CailInitCalls", callIndex + 1);
    // _CailUvdInit 对同一适配器重复调用不是幂等的：成功后 [adapter+0xE48] 会置 bit8（mode 0）或
    // bit20（mode 1），再次进入时门禁直接返回 1。initHardwareCail 的 flag=0 重试正好会撞上。
    bool   alreadyInitialised{false};
    UInt32 doneBit{0}, initMode{0}, initE48{0};
    if (adapter != nullptr) {
        const auto read32 = [](const void* base, const size_t offset) {
            UInt32 value{0};
            memcpy(&value, static_cast<const UInt8*>(base) + offset, sizeof(value));
            return value;
        };
        const auto read64 = [](const void* base, const size_t offset) {
            UInt64 value{0};
            memcpy(&value, static_cast<const UInt8*>(base) + offset, sizeof(value));
            return value;
        };
        initE48 = read32(adapter, 0xE48);
        NRed::singleton().setProp32("NRedVCN1CailE48", initE48);
        NRed::singleton().setProp32("NRedVCN1CailUvdW6", read32(adapter, 0x370));
        NRed::singleton().setProp32("NRedVCN1CailUvdW8", read32(adapter, 0x378));
        // _greenland_uvd_init 按 [0x190]/[0x19C]（CAIL family/extRevision）与能力位 0x133/0x134 选 UVD 7.x 路径。
        NRed::singleton().setProp32("NRedVCN1CailFamily", read32(adapter, 0x190));
        NRed::singleton().setProp32("NRedVCN1CailRevision", read32(adapter, 0x19C));
        // _Cail_MCILWaitFor 先检查 [adapter+0x18] 与它 +0x168 的回调；任一为空会立即返回 1。
        const auto mcil = read64(adapter, 0x18);
        NRed::singleton().setProp32("NRedVCN1CailMcilLow", static_cast<UInt32>(mcil));
        if (mcil != 0) {
            NRed::singleton().setProp32(
                "NRedVCN1CailMcilWaitLow",
                static_cast<UInt32>(read64(reinterpret_cast<const void*>(static_cast<uintptr_t>(mcil)), 0x168)));
        }
        UInt32 lo{0}, hi{0};
        memcpy(&lo, static_cast<const UInt8*>(adapter) + 0x12D8, sizeof(lo));
        memcpy(&hi, static_cast<const UInt8*>(adapter) + 0x12DC, sizeof(hi));
        NRed::singleton().setProp32("NRedVCN1CailInitPtr", lo);
        NRed::singleton().setProp32("NRedVCN1CailInitPtrHi", hi);
        // 减去 kext 的 slide 才能和符号表里的地址对上。
        const auto ptr = (static_cast<mach_vm_address_t>(hi) << 32) | lo;
        if (cailLibrarySlide != 0 && ptr > cailLibrarySlide) {
            NRed::singleton().setProp32("NRedVCN1CailInitFunc", static_cast<UInt32>(ptr - cailLibrarySlide));
        }
        // cailUvdInitOriginal 是 KernelPatcher 的 trampoline，不是 _CailUvdInit 的符号地址；
        // 差值必须相对 initRoute.from（= solveSymbol 结果）才有意义。
        if (cailUvdInitSymbol != 0 && ptr > cailUvdInitSymbol) {
            NRed::singleton().setProp32("NRedVCN1CailInitDelta", static_cast<UInt32>(ptr - cailUvdInitSymbol));
        }
        NRed::singleton().setProp32("NRedVCN1CailE15D0", read32(adapter, 0x15D0) & 0xFFU);
        if (input != nullptr) {
            NRed::singleton().setProp32("NRedVCN1CailInitVersion", read32(input, 0x0));
            initMode = read32(input, 0x144);
            NRed::singleton().setProp32("NRedVCN1CailInitMode", initMode);
            doneBit              = initMode == 1 ? 0x100000U : 0x100U;
            alreadyInitialised   = (initE48 & doneBit) != 0;
        }
    }
    if (alreadyInitialised) {
        // 同一适配器已经完成过该模式的 UVD init；重复执行只会被库层门禁拒绝。
        NRed::singleton().setProp32("NRedVCN1CailInitAlready", 1);
        NRed::singleton().setProp32("NRedVCN1CailInitResult", 0);
        if (callIndex < 8) {
            SYSLOG("VCN1", "CAIL UvdInit already initialised mode=%u e48=0x%X", initMode, initE48);
        }
        return 0;
    }
    if (cailReadMmRegister != nullptr && adapter != nullptr) {
        const auto status = cailReadMmRegister(adapter, 0x83AF);
        NRed::singleton().setProp32("NRedVCN1CailUvdStatusBefore", status);
        if (callIndex == 0) { NRed::singleton().setProp32("NRedVCN1CailUvdStatusFirstBefore", status); }
    }
    auto result = reinterpret_cast<int (*)(void*, void*)>(cailUvdInitOriginal)(adapter, input);
    NRed::singleton().setProp32("NRedVCN1CailInitFirstResult", static_cast<UInt32>(result));
    if (cailReadMmRegister != nullptr && adapter != nullptr) {
        const auto status = cailReadMmRegister(adapter, 0x83AF);
        NRed::singleton().setProp32("NRedVCN1CailUvdStatusAfter", status);
        if (callIndex == 0) { NRed::singleton().setProp32("NRedVCN1CailUvdStatusFirstAfter", status); }
    }
    // 真实 init 失败后把该槽换成返回 0 的桩再走一遍。注意：桩会跳过 UVD 寄存器初始化，
    // 引擎即使“启动成功”也无法处理解码命令，因此默认关闭，只有显式 boot-arg 才启用。
    const bool allowAsicStub = checkKernelArgument("-NRedVCNAsicStub");
    if (allowAsicStub && result != 0 && adapter != nullptr) {
        // 每个适配器独立替换：不同引擎/服务实例的 adapter 不是同一个对象，共用全局 bool 会漏装。
        auto* const      slot = reinterpret_cast<mach_vm_address_t*>(static_cast<UInt8*>(adapter) + 0x12D8);
        const auto       stub = reinterpret_cast<mach_vm_address_t>(&cailAsicInitReturnZero);
        if (*slot != stub) {
            if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                cailAsicInitStubOriginal = *slot;
                *slot                    = stub;
                MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                cailAsicInitStubActive = true;
                NRed::singleton().setProp32("NRedVCN1CailAsicStub", 1);
                NRed::singleton().setProp32("NRedVCN1CailAsicStubPtr", static_cast<UInt32>(cailAsicInitStubOriginal));
                NRed::singleton().setProp32("NRedVCN1CailAsicStubAdapter",
                                            static_cast<UInt32>(reinterpret_cast<uintptr_t>(adapter)));
            }
        }
        if (*slot == stub) {
            result = reinterpret_cast<int (*)(void*, void*)>(cailUvdInitOriginal)(adapter, input);
            NRed::singleton().setProp32("NRedVCN1CailStubResult", static_cast<UInt32>(result));
        }
    }
    NRed::singleton().setProp32("NRedVCN1CailInitResult", static_cast<UInt32>(result));
    static UInt32 count{0};
    if (__atomic_fetch_add(&count, 1U, __ATOMIC_RELAXED) < 8) {
        SYSLOG("VCN1", "CAIL UvdInit result=%d stub=%u", result, cailAsicInitStubActive ? 1U : 0U);
    }
    return result;
}

// 只观察驱动的 TF_PhwRaven_InitPowerGateState：不写 [0x1A0]，不提前上电。
// build39 的强制/留在 0 让 CAIL 在 probe 里进入 0x83AF idle 等待并 panic（§25）。
static int initPowerGateHook(void* phm)
{
    const auto callIndex = __atomic_fetch_add(&initPowerGateCalls, 1U, __ATOMIC_RELAXED) + 1;
    NRed::singleton().setProp32("NRedVCN1InitPowerGateCalls", callIndex);
    if (phm != nullptr) { __atomic_store_n(&phmHandle, phm, __ATOMIC_RELEASE); }
    void* state{nullptr};
    if (phm != nullptr) { memcpy(&state, static_cast<UInt8*>(phm) + 0xA0, sizeof(state)); }
    if (phm != nullptr) {
        const auto value = reinterpret_cast<mach_vm_address_t>(phm);
        NRed::singleton().setProp32("NRedVCN1PhmHandle", static_cast<UInt32>(value & 0xFFFFFFFFULL));
        NRed::singleton().setProp32("NRedVCN1PhmHandleHi", static_cast<UInt32>(value >> 32));
    }
    NRed::singleton().setProp32("NRedVCN1InitPowerGateSeen", 1);
    NRed::singleton().setProp32("NRedVCN1InitPowerGate190Before", readState32(state, 0x190));
    NRed::singleton().setProp32("NRedVCN1InitPowerGate1A0Before", readState32(state, 0x1A0));
    const auto result = reinterpret_cast<int (*)(void*)>(initPowerGateOriginal)(phm);
    NRed::singleton().setProp32("NRedVCN1InitPowerGateResult", static_cast<UInt32>(result));
    NRed::singleton().setProp32("NRedVCN1InitPowerGate190After", readState32(state, 0x190));
    NRed::singleton().setProp32("NRedVCN1InitPowerGate1A0After", readState32(state, 0x1A0));
    SYSLOG("VCN1", "TF_PhwRaven_InitPowerGateState result=%d", result);
    return result;
}

// PECI 注册表读取层：PhwRaven_Initialize 用 key="PP_EnableVCNPG"、默认 1 读取该键。
// personality 里的 CAIL_PP_EnableVCNPG 在 build37 没有被读到（1A0 仍是 1），这里只做观测。
// 注意：把该键钉成 0 会让 [state+0x1A0]=0，之后 PowerDownVCN 全部直接返回，VCN 在 PSM
// 管辖之外持续上电——build41 正是这样在 45.6 秒后冻屏/panic（与 build35 同类）。
// 因此强制路径必须显式传 -NRedVCNForceVCNPG0，默认放行原值。
static int peciReadRegistryHook(void* handle, const char* key, void* out, const UInt32 defaultValue)
{
    const auto result = reinterpret_cast<int (*)(void*, const char*, void*, UInt32)>(peciReadRegistryOriginal)(
        handle, key, out, defaultValue);
    if (key != nullptr && out != nullptr && strcmp(key, "PP_EnableVCNPG") == 0) {
        UInt32 raw{defaultValue};
        memcpy(&raw, out, sizeof(raw));
        const auto calls = __atomic_fetch_add(&peciVcnPGCalls, 1U, __ATOMIC_RELAXED) + 1;
        NRed::singleton().setProp32("NRedVCN1PeciVCNPGCalls", calls);
        NRed::singleton().setProp32("NRedVCN1PeciVCNPGRaw", raw);
        NRed::singleton().setProp32("NRedVCN1PeciVCNPGResult", static_cast<UInt32>(result));
        if (checkKernelArgument("-NRedVCNForceVCNPG0")) {
            const UInt32 forced{0};
            memcpy(out, &forced, sizeof(forced));
            NRed::singleton().setProp32("NRedVCN1PeciVCNPGForced", 1);
            SYSLOG("VCN1", "PP_EnableVCNPG raw=%u result=%d -> forced 0 (boot-arg)", raw, result);
            return 0;
        }
    }
    return result;
}

// 驱动自己的 VCN 电源入口：确认 TF 路径真的调到 PowerUpVCN，以及之后有没有被关掉。
static int ravenPowerUpHook(void* phm)
{
    void* state{nullptr};
    if (phm != nullptr) { memcpy(&state, static_cast<UInt8*>(phm) + 0xA0, sizeof(state)); }
    const auto before = readState32(state, 0x190);
    const auto result = reinterpret_cast<int (*)(void*)>(ravenPowerUpOriginal)(phm);
    const auto calls  = __atomic_fetch_add(&ravenPowerUpCalls, 1U, __ATOMIC_RELAXED) + 1;
    NRed::singleton().setProp32("NRedVCN1RavenPowerUpCalls", calls);
    NRed::singleton().setProp32("NRedVCN1RavenPowerUpResult", static_cast<UInt32>(result));
    NRed::singleton().setProp32("NRedVCN1RavenPowerUp190Before", before);
    NRed::singleton().setProp32("NRedVCN1RavenPowerUp190After", readState32(state, 0x190));
    NRed::singleton().setProp32("NRedVCN1RavenPowerUp1A0", readState32(state, 0x1A0));
    SYSLOG("VCN1", "PhwRaven_PowerUpVCN result=%d 190=%u->%u call=%u", result, before,
           readState32(state, 0x190), calls);
    return result;
}

static int ravenPowerDownHook(void* phm)
{
    void* state{nullptr};
    if (phm != nullptr) { memcpy(&state, static_cast<UInt8*>(phm) + 0xA0, sizeof(state)); }
    const auto before = readState32(state, 0x190);
    const auto result = reinterpret_cast<int (*)(void*)>(ravenPowerDownOriginal)(phm);
    const auto calls  = __atomic_fetch_add(&ravenPowerDownCalls, 1U, __ATOMIC_RELAXED) + 1;
    NRed::singleton().setProp32("NRedVCN1RavenPowerDownCalls", calls);
    NRed::singleton().setProp32("NRedVCN1RavenPowerDownResult", static_cast<UInt32>(result));
    NRed::singleton().setProp32("NRedVCN1RavenPowerDown190Before", before);
    NRed::singleton().setProp32("NRedVCN1RavenPowerDown190After", readState32(state, 0x190));
    SYSLOG("VCN1", "PhwRaven_PowerDownVCN result=%d 190=%u->%u call=%u", result, before,
           readState32(state, 0x190), calls);
    return result;
}

// PSM/PEM 的门控调度入口：看 PSM 是否在引擎 init 前后请求开/关 VCN。
static int phmPowerGateVCNHook(void* phm, const UInt32 on, const UInt32 arg)
{
    void* state{nullptr};
    if (phm != nullptr) { memcpy(&state, static_cast<UInt8*>(phm) + 0xA0, sizeof(state)); }
    const auto before = readState32(state, 0x190);
    const auto result = reinterpret_cast<int (*)(void*, UInt32, UInt32)>(phmPowerGateVCNOriginal)(phm, on, arg);
    const auto calls  = __atomic_fetch_add(&phmPowerGateVCNCalls, 1U, __ATOMIC_RELAXED) + 1;
    NRed::singleton().setProp32("NRedVCN1PhmPowerGateVCNCalls", calls);
    NRed::singleton().setProp32("NRedVCN1PhmPowerGateVCNLastOn", on);
    NRed::singleton().setProp32("NRedVCN1PhmPowerGateVCNResult", static_cast<UInt32>(result));
    NRed::singleton().setProp32("NRedVCN1PhmPowerGateVCN190Before", before);
    NRed::singleton().setProp32("NRedVCN1PhmPowerGateVCN190After", readState32(state, 0x190));
    return result;
}

// UVD 门控：照旧驱动可能走 PHM_PowerGateUVD 而不是 VCN 版本（block 分发器决定）。
static int phmPowerGateUVDHook(void* phm, const UInt32 on, const UInt32 arg)
{
    void* state{nullptr};
    if (phm != nullptr) { memcpy(&state, static_cast<UInt8*>(phm) + 0xA0, sizeof(state)); }
    const auto before = readState32(state, 0x190);
    const auto result = reinterpret_cast<int (*)(void*, UInt32, UInt32)>(phmPowerGateUVDOriginal)(phm, on, arg);
    const auto calls  = __atomic_fetch_add(&phmPowerGateUVDCalls, 1U, __ATOMIC_RELAXED) + 1;
    NRed::singleton().setProp32("NRedVCN1PhmPowerGateUVDCalls", calls);
    NRed::singleton().setProp32("NRedVCN1PhmPowerGateUVDLastOn", on);
    NRed::singleton().setProp32("NRedVCN1PhmPowerGateUVDResult", static_cast<UInt32>(result));
    NRed::singleton().setProp32("NRedVCN1PhmPowerGateUVD190Before", before);
    NRed::singleton().setProp32("NRedVCN1PhmPowerGateUVD190After", readState32(state, 0x190));
    return result;
}

// PSM 媒体 block 使用：PEM 的视频启动任务按状态调用 PSM_UseAsicBlock(block=2/3/6)。
// 这是判断「PSM 自己有没有激活 UVD/VCN」最直接的信号。
static int psmUseAsicBlockHook(void* psm, const UInt32 block)
{
    const auto result = reinterpret_cast<int (*)(void*, UInt32)>(psmUseAsicBlockOriginal)(psm, block);
    const auto calls  = __atomic_fetch_add(&psmUseAsicBlockCalls, 1U, __ATOMIC_RELAXED) + 1;
    UInt32 mask = __atomic_load_n(&psmUseAsicBlockMask, __ATOMIC_RELAXED);
    if (block < 32) {
        mask = __atomic_fetch_or(&psmUseAsicBlockMask, 1U << block, __ATOMIC_RELAXED) | (1U << block);
    }
    // 前 8 次或出现新 block 时才写属性，避免在 PSM 热路径上反复分配。
    static UInt32 publishedMask{0};
    if (calls <= 8 || (mask & ~publishedMask) != 0) {
        publishedMask = mask;
        NRed::singleton().setProp32("NRedVCN1PsmUseAsicBlockCalls", calls);
        NRed::singleton().setProp32("NRedVCN1PsmUseAsicBlockLastBlock", block);
        NRed::singleton().setProp32("NRedVCN1PsmUseAsicBlockLastResult", static_cast<UInt32>(result));
        NRed::singleton().setProp32("NRedVCN1PsmUseAsicBlockBlockMask", mask);
    }
    return result;
}

// PSM 仲裁：读 [psm+0x5f0]/[psm+0x690]（VCN 相关开关），并统计本次仲裁是否真的触发了
// PHM_PowerGateVCN/UVD。只在门控计数变化或前 16 次调用时写属性。
static int psmReArbitHook(void* psm, const UInt32 reason)
{
    const auto flag5f0   = readState32(psm, 0x5F0);
    const auto flag690   = readState32(psm, 0x690);
    const auto vcnBefore = __atomic_load_n(&phmPowerGateVCNCalls, __ATOMIC_RELAXED);
    const auto uvdBefore = __atomic_load_n(&phmPowerGateUVDCalls, __ATOMIC_RELAXED);
    const auto result    = reinterpret_cast<int (*)(void*, UInt32)>(psmReArbitOriginal)(psm, reason);
    const auto vcnAfter  = __atomic_load_n(&phmPowerGateVCNCalls, __ATOMIC_RELAXED);
    const auto uvdAfter  = __atomic_load_n(&phmPowerGateUVDCalls, __ATOMIC_RELAXED);
    const auto calls     = __atomic_fetch_add(&psmReArbitCalls, 1U, __ATOMIC_RELAXED) + 1;
    const auto gateDelta = (vcnAfter - vcnBefore) + (uvdAfter - uvdBefore);
    if (calls <= 16 || gateDelta != 0) {
        const auto psmValue = reinterpret_cast<mach_vm_address_t>(psm);
        NRed::singleton().setProp32("NRedVCN1PsmHandle", static_cast<UInt32>(psmValue & 0xFFFFFFFFULL));
        NRed::singleton().setProp32("NRedVCN1PsmHandleHi", static_cast<UInt32>(psmValue >> 32));
        NRed::singleton().setProp32("NRedVCN1PsmReArbitCalls", calls);
        NRed::singleton().setProp32("NRedVCN1PsmReArbitLastReason", reason);
        NRed::singleton().setProp32("NRedVCN1PsmReArbitLastResult", static_cast<UInt32>(result));
        NRed::singleton().setProp32("NRedVCN1PsmReArbitFlag5F0", flag5f0);
        NRed::singleton().setProp32("NRedVCN1PsmReArbitFlag690", flag690);
        NRed::singleton().setProp32("NRedVCN1PsmReArbitGateDelta", gateDelta);
    }
    return result;
}

// PSM 客户端激活：视频栈如果向 PSM 注册了媒体客户端，这里会看到对应的 client id。
static int psmActivateClientHook(void* psm, const UInt32 client, const UInt32 arg2, const UInt32 arg3,
                                 const UInt32 arg4)
{
    const auto result = reinterpret_cast<int (*)(void*, UInt32, UInt32, UInt32, UInt32)>(psmActivateClientOriginal)(
        psm, client, arg2, arg3, arg4);
    const auto calls = __atomic_fetch_add(&psmActivateClientCalls, 1U, __ATOMIC_RELAXED) + 1;
    if (calls <= 16) {
        NRed::singleton().setProp32("NRedVCN1PsmActivateClientCalls", calls);
        NRed::singleton().setProp32("NRedVCN1PsmActivateClientLast", client);
        NRed::singleton().setProp32("NRedVCN1PsmActivateClientArg2", arg2);
        NRed::singleton().setProp32("NRedVCN1PsmActivateClientArg3", arg3);
        NRed::singleton().setProp32("NRedVCN1PsmActivateClientResult", static_cast<UInt32>(result));
    }
    return result;
}

// PEM 的视频启动任务：确认视频栈是否真的把 UVD 多状态启动任务跑起来。
static int pemVideoStartHook(void* task, void* event)
{
    const auto result = reinterpret_cast<int (*)(void*, void*)>(pemVideoStartOriginal)(task, event);
    const auto calls  = __atomic_fetch_add(&pemVideoStartCalls, 1U, __ATOMIC_RELAXED) + 1;
    if (calls <= 16) {
        NRed::singleton().setProp32("NRedVCN1PemVideoStartCalls", calls);
        NRed::singleton().setProp32("NRedVCN1PemVideoStartResult", static_cast<UInt32>(result));
    }
    return result;
}

void VCN::hookCailLibrary(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
{
    if (!vcn1Device()) { return; }
    cailLibrarySlide = slide;
    KernelPatcher::RouteRequest route{"_CAILUvdControl", cailUvdControlHook, cailUvdControlOriginal};
    if (!patcher.routeMultiple(id, &route, 1, slide, size)) {
        patcher.clearError();
        NRed::singleton().setProp32("NRedVCN1CailLibraryHooked", 0);
        return;
    }
    KernelPatcher::RouteRequest initRoute{"_CailUvdInit", cailUvdInitHook, cailUvdInitOriginal};
    if (!patcher.routeMultiple(id, &initRoute, 1, slide, size)) {
        patcher.clearError();
        NRed::singleton().setProp32("NRedVCN1CailInitHooked", 0);
    }
    else {
        // RouteRequest::from 是 solveSymbol 的运行时符号地址（cailUvdInitOriginal 是 trampoline）。
        cailUvdInitSymbol = initRoute.from;
        NRed::singleton().setProp32("NRedVCN1CailInitHooked", 1);
    }
    cailReadMmRegister =
        reinterpret_cast<UInt32 (*)(void*, UInt32)>(patcher.solveSymbol(id, "_ulReadMmRegisterUlong"));
    patcher.clearError();
    NRed::singleton().setProp32("NRedVCN1CailMmioReadResolved", cailReadMmRegister != nullptr);
    // 只观测驱动的电源路径：不路由 PHM_Initialize，也不主动写 SMU/寄存器。
    KernelPatcher::RouteRequest gateRoute{"_TF_PhwRaven_InitPowerGateState", initPowerGateHook,
                                          initPowerGateOriginal};
    if (!patcher.routeMultiple(id, &gateRoute, 1, slide, size)) {
        patcher.clearError();
        NRed::singleton().setProp32("NRedVCN1InitPowerGateHooked", 0);
    }
    else {
        NRed::singleton().setProp32("NRedVCN1InitPowerGateHooked", 1);
    }
    // PECI 层钉住 PP_EnableVCNPG，并观测驱动自己的 VCN 电源入口与 PHM 门控调度。
    KernelPatcher::RouteRequest peciRoute{"_PECI_ReadRegistry", peciReadRegistryHook, peciReadRegistryOriginal};
    if (!patcher.routeMultiple(id, &peciRoute, 1, slide, size)) {
        patcher.clearError();
        NRed::singleton().setProp32("NRedVCN1PeciHooked", 0);
    }
    else {
        NRed::singleton().setProp32("NRedVCN1PeciHooked", 1);
    }
    KernelPatcher::RouteRequest powerUpRoute{"_PhwRaven_PowerUpVCN", ravenPowerUpHook, ravenPowerUpOriginal};
    if (!patcher.routeMultiple(id, &powerUpRoute, 1, slide, size)) {
        patcher.clearError();
        NRed::singleton().setProp32("NRedVCN1RavenPowerUpHooked", 0);
    }
    else {
        NRed::singleton().setProp32("NRedVCN1RavenPowerUpHooked", 1);
    }
    KernelPatcher::RouteRequest powerDownRoute{"_PhwRaven_PowerDownVCN", ravenPowerDownHook, ravenPowerDownOriginal};
    if (!patcher.routeMultiple(id, &powerDownRoute, 1, slide, size)) {
        patcher.clearError();
        NRed::singleton().setProp32("NRedVCN1RavenPowerDownHooked", 0);
    }
    else {
        NRed::singleton().setProp32("NRedVCN1RavenPowerDownHooked", 1);
    }
    KernelPatcher::RouteRequest gateVcnRoute{"_PHM_PowerGateVCN", phmPowerGateVCNHook, phmPowerGateVCNOriginal};
    if (!patcher.routeMultiple(id, &gateVcnRoute, 1, slide, size)) {
        patcher.clearError();
        NRed::singleton().setProp32("NRedVCN1PhmGateVCNHooked", 0);
    }
    else {
        NRed::singleton().setProp32("NRedVCN1PhmGateVCNHooked", 1);
    }
    KernelPatcher::RouteRequest gateUvdRoute{"_PHM_PowerGateUVD", phmPowerGateUVDHook, phmPowerGateUVDOriginal};
    if (!patcher.routeMultiple(id, &gateUvdRoute, 1, slide, size)) {
        patcher.clearError();
        NRed::singleton().setProp32("NRedVCN1PhmGateUVDHooked", 0);
    }
    else {
        NRed::singleton().setProp32("NRedVCN1PhmGateUVDHooked", 1);
    }
    KernelPatcher::RouteRequest psmBlockRoute{"_PSM_UseAsicBlock", psmUseAsicBlockHook, psmUseAsicBlockOriginal};
    if (!patcher.routeMultiple(id, &psmBlockRoute, 1, slide, size)) {
        patcher.clearError();
        NRed::singleton().setProp32("NRedVCN1PsmBlockHooked", 0);
    }
    else {
        NRed::singleton().setProp32("NRedVCN1PsmBlockHooked", 1);
    }
    KernelPatcher::RouteRequest psmArbRoute{"_PSM_ReArbit", psmReArbitHook, psmReArbitOriginal};
    if (!patcher.routeMultiple(id, &psmArbRoute, 1, slide, size)) {
        patcher.clearError();
        NRed::singleton().setProp32("NRedVCN1PsmReArbitHooked", 0);
    }
    else {
        NRed::singleton().setProp32("NRedVCN1PsmReArbitHooked", 1);
    }
    KernelPatcher::RouteRequest psmClientRoute{"_PSM_ActivateClient", psmActivateClientHook,
                                               psmActivateClientOriginal};
    if (!patcher.routeMultiple(id, &psmClientRoute, 1, slide, size)) {
        patcher.clearError();
        NRed::singleton().setProp32("NRedVCN1PsmClientHooked", 0);
    }
    else {
        NRed::singleton().setProp32("NRedVCN1PsmClientHooked", 1);
    }
    KernelPatcher::RouteRequest pemVideoRoute{"_PEM_Task_VideoStartMultiUVDStates", pemVideoStartHook,
                                              pemVideoStartOriginal};
    if (!patcher.routeMultiple(id, &pemVideoRoute, 1, slide, size)) {
        patcher.clearError();
        NRed::singleton().setProp32("NRedVCN1PemVideoHooked", 0);
    }
    else {
        NRed::singleton().setProp32("NRedVCN1PemVideoHooked", 1);
    }
    NRed::singleton().setProp32("NRedVCN1CailLibraryHooked", 1);
}
