// Vega 核显的视频接入：VCN2.2 设备桥接 X6000 引擎，VCN1（Picasso）用 X5000 原生 UVD7 引擎（Vega10 世代，与 VCN1 同 IP）。
#pragma once
#include <Headers/kern_patcher.hpp>

namespace VCN
{

    void  init();
    void  processKext(KernelPatcher& patcher, size_t id, mach_vm_address_t slide, size_t size);
    bool  ready();
    bool  allocated();
    void* allocateEngine();

    // VCN1（Raven/Picasso）：符号与虚表守卫核验通过后为真。
    bool vcn1Ready();
    // 分配一个 X5000 原生 AMDUVD7HWEngine；VCN1 只有这一个视频引擎（解码），编码需另寻路径。
    void* allocateUVD7Engine();

}    // namespace VCN
