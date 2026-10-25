// Vega 核显的视频能力；VCN1（Picasso）经 X5000 原生 UVD7v2 引擎接入，VCN2.2 经 X6000 桥接。
#pragma once
#include <IOKit/IOTypes.h>
#include <PenguinWizardry/KernelVersion.hpp>

namespace VCNCapabilities
{

    enum class Generation : UInt32
    {
        VCN1  = 0x0100,
        VCN22 = 0x0202
    };
    struct Capability
    {
        UInt32     deviceID;
        Generation generation;
        UInt32     vaFamily;
    };

    // vaFamily 是 VA 驱动的解码能力分类，不能当作 GPU 的 GFX/AddrLib 家族。
    // Renoir 与 Cezanne 共用 VCN 2.2 接入（分类 5 = Navi/VCN2）；
    // Picasso 走 VCN1 接入（分类 4 = Vega 20/UVD7v2）。Raven 保留分类但暂不接入。
    inline constexpr Capability Devices[] = {
        {0x15D8, Generation::VCN1, 4},  {0x15DD, Generation::VCN1, 0},  {0x15E7, Generation::VCN22, 5},
        {0x1636, Generation::VCN22, 5}, {0x1638, Generation::VCN22, 5}, {0x164C, Generation::VCN22, 5},
    };

    inline constexpr const Capability* findDevice(UInt32 deviceID)
    {
        for (const auto& capability : Devices) {
            if (capability.deviceID == deviceID) { return &capability; }
        }
        return nullptr;
    }

    // 3=未知设备，4=未适配的系统，5=未实现的视频代际；0 才允许进入驱动 ABI 核验。
    inline constexpr UInt32 rejection(UInt32 deviceID, const PenguinWizardry::KernelVersion& version)
    {
        const auto* capability = findDevice(deviceID);
        if (!capability) { return 3; }
        const bool videoFamily = (capability->generation == Generation::VCN22 && capability->vaFamily == 5)
                                 || (capability->generation == Generation::VCN1 && capability->vaFamily == 4);
        if (!videoFamily) { return 5; }
        if (!version.majorMatches(MACOS_14) && !version.majorMatches(MACOS_15) && !version.majorMatches(MACOS_26)) {
            return 4;
        }
        return 0;
    }

}    // namespace VCNCapabilities
