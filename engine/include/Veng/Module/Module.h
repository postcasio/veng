#pragma once

#include <Veng/Veng.h>
#include <Veng/Module/ApplicationRegistry.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    // Forward-declared, defined only in the editor framework. libveng never sees
    // its definition; a non-editor host passes Editor = nullptr. Held by pointer,
    // so the incomplete type suffices.
    class EditorRegistry;

    // The host's side of the module contract: what a loaded module registers
    // into. The host owns these for the module's whole lifetime; a module
    // registers into them and never frees a host object. Registration is
    // GPU-free — a factory and reflected type descriptors — so no live
    // Context/AssetManager is needed here; the host owns the registries and
    // threads them into the Application it later constructs.
    struct VengModuleHost
    {
        ApplicationRegistry& App;    // the module hands the host its Application factory
        TypeRegistry&        Types;  // the module registers its component/type descriptors
        EditorRegistry*      Editor; // non-null ONLY when loaded by the editor; null otherwise
    };
}

extern "C"
{
    // Exported by every game / editor module. The host dlsym()s this exact name,
    // calls it once after load, and the module registers what it provides into
    // the registries the host passes in. C ABI so the symbol resolves robustly
    // and a stale module fails loudly at load; the VengModuleHost payload is rich
    // C++.
    VE_MODULE_EXPORT void VengModuleRegister(Veng::VengModuleHost* host);
}

// Bumped whenever VengModuleHost's layout or the entry contract changes. Host
// and module each bake in the value from the header they compiled against; the
// loader compares them before calling VengModuleRegister. Guarded with #ifndef
// so a target can force a wrong value with a -D define.
#ifndef VENG_MODULE_ABI_VERSION
#define VENG_MODULE_ABI_VERSION 2u
#endif

// A module drops this in exactly one translation unit to export the version it
// was built against, beside its VengModuleRegister. The loader resolves and
// checks it.
#define VE_EXPORT_MODULE_ABI() \
    extern "C" VE_MODULE_EXPORT Veng::u32 VengModuleAbiVersion() { return VENG_MODULE_ABI_VERSION; }
