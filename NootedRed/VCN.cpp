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
    OSMetaClass*            uvd7v2EngineMC{nullptr};
    mach_vm_address_t       smlQueryOriginal{0}, uvd7v2InitHardwareOriginal{0};
    UInt32                  uvd7v2InitHardwareCount{0}, smlQueryServedCount{0};
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

    // UVD7v2 引擎 init 经 SML 查询选择子 0x0C 取 VCN 基址；Raven/Picasso 与 Vega10 同基址表。
    int smlQueryBase(void* self, const UInt32 selector, const UInt32 isEncode, const UInt32 flag, void* out)
    {
        if (selector == 0xC && out != nullptr) {
            *static_cast<UInt32*>(out) = 0x7800;    // VCN_BASE__INST0_SEG0
            auto count = __atomic_fetch_add(&smlQueryServedCount, 1U, __ATOMIC_RELAXED);
            if (count < 16) { SYSLOG("VCN1", "SML base query served: isEncode=%u flag=%u", isEncode, flag); }
            return 0;
        }
        return reinterpret_cast<int (*)(void*, UInt32, UInt32, UInt32, void*)>(smlQueryOriginal)(self, selector,
                                                                                                 isEncode, flag, out);
    }

    bool uvd7v2InitHardware(void* self)
    {
        const auto result = reinterpret_cast<bool (*)(void*)>(uvd7v2InitHardwareOriginal)(self);
        NRed::singleton().setProp32("NRedVCN1LastHardwareInit", result);
        auto count = __atomic_fetch_add(&uvd7v2InitHardwareCount, 1U, __ATOMIC_RELAXED);
        if (count < 16) { SYSLOG("VCN1", "UVD7v2 initHardware result=%u", result); }
        return result;
    }

    // VCN1 守卫与钩子：UVD7v2 虚表槽位与具名符号一致后，才接管 SML 基址查询（选择子 0x0C）。
    bool prepareVCN1(KernelPatcher& patcher, const size_t id, const mach_vm_address_t slide, const size_t size)
    {
        mach_vm_address_t engineInit{0}, engineInitHardware{0}, engineVTable{0}, smlVTable{0};
        bool ok = resolve(patcher, id, "__ZN32AMDRadeonX5000_AMDUVD7v2HWEngine10gMetaClassE", uvd7v2EngineMC, slide,
                          size);
        ok &= resolve(patcher, id,
                      "__ZN32AMDRadeonX5000_AMDUVD7v2HWEngine4initEP30AMDRadeonX5000_IAMDHWInterface20_eAMD_HW_ENGINE_"
                      "TYPE",
                      engineInit, slide, size);
        ok &= resolve(patcher, id, "__ZN32AMDRadeonX5000_AMDUVD7v2HWEngine12initHardwareEv", engineInitHardware, slide,
                      size);
        ok &= resolve(patcher, id, "__ZTV32AMDRadeonX5000_AMDUVD7v2HWEngine", engineVTable, slide, size);
        ok &= resolve(patcher, id, "__ZTV34AMDRadeonX5000_AMDSMLGFX9Interface", smlVTable, slide, size);
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
            SYSLOG("VCN1", "UVD7v2 vtable mismatch: init=%p/%p hw=%p/%p", reinterpret_cast<void*>(slot(0x118)),
                   reinterpret_cast<void*>(engineInit), reinterpret_cast<void*>(slot(0x368)),
                   reinterpret_cast<void*>(engineInitHardware));
            return false;
        }
        const mach_vm_address_t smlSlot = smlVTable + 16 + 0x58;
        if (smlVTable < slide || smlSlot > slide + size - sizeof(void*)) { return false; }
        memcpy(&smlQueryOriginal, reinterpret_cast<void*>(smlSlot), sizeof(smlQueryOriginal));
        if (smlQueryOriginal == 0) { return false; }
        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) { return false; }
        const mach_vm_address_t hook = reinterpret_cast<mach_vm_address_t>(&smlQueryBase);
        memcpy(reinterpret_cast<void*>(smlSlot), &hook, sizeof(hook));
        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
        // initHardware 路由仅作诊断，失败不影响接入。
        KernelPatcher::RouteRequest route{"__ZN32AMDRadeonX5000_AMDUVD7v2HWEngine12initHardwareEv", uvd7v2InitHardware,
                                          uvd7v2InitHardwareOriginal};
        if (!patcher.routeMultiple(id, &route, 1, slide, size)) { patcher.clearError(); }
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

bool VCN::vcn1Ready() { return supported() && vcn1Device() && vcn1Prepared && uvd7v2EngineMC != nullptr; }
void* VCN::allocateUVD7v2Engine()
{
    if (!vcn1Ready()) {
        NRed::singleton().setProp32("NRedVCN1AllocationSkipped", requested);
        return nullptr;
    }
    return uvd7v2EngineMC->alloc();
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
            ok &=
                resolve(patcher, id, "__ZN29AMDRadeonX5000_AMDGFX9Hardware15notifyGfxAccessEv", notifyAccess, slide, size);
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
                // VCN1：X5000 原生 UVD7v2 引擎与 SML 基址查询；与 VCN2 桥接互不影响。
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
