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
    UInt32                  uvd7InitHardwareCount{0}, smlQueryServedCount{0};
    UInt32                  uvd7InitCount{0}, uvd7LoadUVDFWCount{0}, uvd7InitHardwareCailCount{0};
    // VCN1 的 SML 实例由 services 的 +0x850 工厂创建，未必落在静态打补丁的两张虚表上；
    // 运行期按对象真实 vptr 再接管一次 +0x58，并把原生返回值原样链式调用。
    mach_vm_address_t       smlQueryOriginalLive{0}, externalInterfacesOriginal{0};
    UInt32                  smlLiveCount{0}, externalInterfacesCount{0};
    // CAIL 侧提交入口（[engine+0x18]->vt[0x2B8] 得到的客户端对象的 +0xD0）只在运行期可见。
    mach_vm_address_t       cailSubmitOriginal{0}, cailClient{0};
    UInt32                  cailSubmitCount{0};
    // 两个引擎共用同一个 CAIL 客户端，按 isEncode 分开记录首个失败的 selector。
    UInt32                  cailEncode{0}, cailFailed[2]{0, 0}, cailDecResult{0}, cailEncResult{0};
    // 原生首条固件消息被判为不支持时，退回不带该消息的序列再试一次；单独记录该次失败。
    UInt32                  cailRetryFailed[2]{0, 0};
    bool                    cailRetryActive{false};
    mach_vm_address_t       engineStartOriginal{0}, enginePowerOriginal{0};
    mach_vm_address_t       engineRingsOriginal{0}, engineChannelsOriginal{0};
    mach_vm_address_t       acceleratorStartOriginal{0};
    mach_vm_address_t       safeCastOriginal{0}, newSharedOriginal{0}, newClientOriginal{0}, newVideoOriginal{0};
    mach_vm_address_t       newVideo6000{0};
    mach_vm_address_t       clientStart5000{0}, clientStop5000{0}, clientStart6000{0}, clientStop6000{0};
    mach_vm_address_t       copy5000{0}, copy6000{0}, sml5000{0}, sml6000{0}, initHardwareOriginal{0};
    mach_vm_address_t       signalOriginal{0};
    void                    (*notifyAccess)(void*){nullptr};
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
    void* newVideo(void* self)
    {
        if (VCN::allocated()) { return reinterpret_cast<void* (*)(void*)>(newVideo6000)(self); }
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
            *static_cast<UInt32*>(out) = 0x7800;    // VCN_BASE__INST0_SEG0（Raven 与 Vega10 同表）
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
    void externalInterfaces(void* self)
    {
        reinterpret_cast<void (*)(void*)>(externalInterfacesOriginal)(self);
        void* sml{nullptr};
        memcpy(&sml, static_cast<UInt8*>(self) + 0x338, sizeof(sml));
        NRed::singleton().setProp32("NRedVCN1SMLPresent", sml != nullptr);
        attachLiveSML(sml);
        const auto count = __atomic_fetch_add(&externalInterfacesCount, 1U, __ATOMIC_RELAXED);
        if (count < 8) { SYSLOG("VCN1", "initializeExternalInterfaces self=%p sml=%p", self, sml); }
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
        // Raven/Picasso 的 VCN 基址就是 vega10_ip_offset.h 的 VCN_BASE__INST0_SEG0；
        // 原生 SML 在 APU 上不提供该值，统一覆写，保证 initHardware 的 MMIO 偏移正确。
        if (result && vcn1Device() && (engineType == 5 || engineType == 6)) {
            constexpr UInt32 vcnBase{0x7800};
            memcpy(static_cast<UInt8*>(self) + 0x250, &vcnBase, sizeof(vcnBase));
            base = vcnBase;
            NRed::singleton().setProp32("NRedVCN1EngineBaseForced", 1);
        }
        NRed::singleton().setProp32(engineType == 6 ? "NRedVCN1EngineBaseEnc" : "NRedVCN1EngineBaseDec", base);
        NRed::singleton().setProp32("NRedVCN1EngineInitResult", result);
        NRed::singleton().setProp32("NRedVCN1EngineInitType", engineType);
        NRed::singleton().setProp32("NRedVCN1EngineBase", base);
        NRed::singleton().setProp32("NRedVCN1EngineIsEncode", isEncode);
        auto count = __atomic_fetch_add(&uvd7InitCount, 1U, __ATOMIC_RELAXED);
        if (count < 16) {
            SYSLOG("VCN1", "engine init type=%u result=%u base=0x%X isEncode=%u", engineType, result, base, isEncode);
        }
        return result;
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
        const auto result = reinterpret_cast<bool (*)(void*)>(uvd7InitHardwareOriginal)(self);
        NRed::singleton().setProp32("NRedVCN1LastHardwareInit", result);
        NRed::singleton().setProp32(isEncode ? "NRedVCN1HardwareInitEnc" : "NRedVCN1HardwareInitDec", result);
        auto count = __atomic_fetch_add(&uvd7InitHardwareCount, 1U, __ATOMIC_RELAXED);
        if (count < 16) { SYSLOG("VCN1", "UVD7 initHardware result=%u", result); }
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
        setEngineProp(self, "NRedVCN1PowerDec", "NRedVCN1PowerEnc", (on ? 2U : 0U) | (result ? 1U : 0U));
        SYSLOG("VCN1", "UVDHWEngine::powerOnOff on=%u result=%u", on, result);
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

    // VCN1 守卫与钩子：UVD7 虚表槽位与具名符号一致后，才接管 SML 基址查询（选择子 0x0C）。
    bool prepareVCN1(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
    {
        mach_vm_address_t engineInit{0}, engineInitHardware{0}, engineVTable{0}, smlVTable{0}, smlVTableBase{0};
        mach_vm_address_t engineLoadUVDFW{0}, engineInitHardwareCail{0}, externalInterfacesFunction{0};
        mach_vm_address_t engineStart{0}, enginePower{0}, engineRings{0}, engineChannels{0};
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
