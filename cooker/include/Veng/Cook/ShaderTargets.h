#pragma once

#include <array>

#include <string_view>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Types.h>
#include <Veng/Cook/Export.h>

/// @file
/// @brief The GPU targets a shader compiles for, and the reflected interface a compile produces.
///
/// The cook compiles every shader through these, and a tool that compiles Slang at run time uses
/// the same calls: it asks for its backend's target (NativeShaderTarget), describes that target to
/// its Slang session (DescribeSlangTarget), and reflects the linked program into the same interface
/// form the cook stores (ReflectShaderInterface). The Slang types are only forward-declared here, so
/// including this header needs no Slang headers; calling the functions that take them does.

namespace slang
{
    struct IGlobalSession;
    struct ShaderReflection;
    using ProgramLayout = ShaderReflection;
    struct TargetDesc;
}

namespace Veng::Cook
{
    /// @brief One GPU code target a shader can be compiled for.
    enum class ShaderTarget : u32
    {
        /// @brief SPIR-V, consumed by the Vulkan backend.
        SpirV = 1u << 0,
        /// @brief Metal, consumed by the Metal backend (a compiled library, or source at run time).
        Metal = 1u << 1,
    };

    /// @brief A set of shader targets.
    struct ShaderTargetSet
    {
        /// @brief The member targets' ShaderTarget bits.
        u32 Bits = 0;

        /// @brief Whether @p target is a member.
        [[nodiscard]] constexpr bool Has(ShaderTarget target) const
        {
            return (Bits & static_cast<u32>(target)) != 0;
        }

        /// @brief This set with @p target added.
        [[nodiscard]] constexpr ShaderTargetSet With(ShaderTarget target) const
        {
            return ShaderTargetSet{.Bits = Bits | static_cast<u32>(target)};
        }

        /// @brief This set with @p target removed.
        [[nodiscard]] constexpr ShaderTargetSet Without(ShaderTarget target) const
        {
            return ShaderTargetSet{.Bits = Bits & ~static_cast<u32>(target)};
        }

        /// @brief Whether the set has no members.
        [[nodiscard]] constexpr bool IsEmpty() const { return Bits == 0; }

        /// @brief Whether two sets have the same members.
        [[nodiscard]] constexpr bool operator==(const ShaderTargetSet&) const = default;
    };

    /// @brief Resolves a shader-target setting against a GPU backend.
    ///
    /// The setting is the build's VE_SHADER_TARGETS: `Native` (the backend's own target) or `All`
    /// (SPIR-V and Metal), or an explicit comma-separated list of `spirv` and `metal`. The backend
    /// is a VE_GPU_BACKEND value, `Vulkan` or `Metal`. Names match case-insensitively.
    /// @param setting The target setting.
    /// @param backend The backend `Native` resolves against.
    /// @return The targets, or an error naming the unrecognized setting or backend.
    [[nodiscard]] VE_COOK_API Result<ShaderTargetSet>
    ResolveShaderTargets(std::string_view setting, std::string_view backend);

    /// @brief The GPU backend this cooker was built beside (its VE_GPU_BACKEND): `Vulkan` or `Metal`.
    [[nodiscard]] VE_COOK_API std::string_view NativeGpuBackend();

    /// @brief The target of the GPU backend this cooker was built beside.
    [[nodiscard]] VE_COOK_API ShaderTarget NativeShaderTarget();

    /// @brief The targets the build this cooker belongs to cooks by default (its VE_SHADER_TARGETS).
    [[nodiscard]] VE_COOK_API ShaderTargetSet DefaultShaderTargets();

    /// @brief Renders a target set as its comma-separated names ("spirv,metal").
    [[nodiscard]] VE_COOK_API string FormatShaderTargets(ShaderTargetSet targets);

    /// @brief The Metal compiler's version line, or nullopt when no Metal toolchain is installed.
    ///
    /// Runs `xcrun -sdk macosx metal --version` once per process and caches the answer. Always
    /// nullopt off Apple platforms.
    [[nodiscard]] VE_COOK_API const optional<string>& ProbeMetalToolchain();

    /// @brief Fills a Slang target description for @p target.
    ///
    /// SPIR-V is SPIR-V 1.5; Metal is Metal Shading Language source with fast math, which a cook
    /// then compiles to a library and a run-time compile hands to the device compiler.
    /// @param session The global session whose profiles the description names.
    /// @param target  The target to describe.
    /// @param desc    Receives the format, profile and floating-point mode.
    VE_COOK_API void DescribeSlangTarget(slang::IGlobalSession& session, ShaderTarget target,
                                         slang::TargetDesc& desc);

    /// @brief A linked shader's interface, in the form the cooked shader blob stores.
    struct ReflectedShaderInterface
    {
        /// @brief The entry point's stage; an underlying Renderer::ShaderStage integer.
        u32 StageMask = 0;
        /// @brief Every descriptor binding past the bindless registries' sets.
        vector<CookedDescriptorBinding> Bindings;
        /// @brief Every push-constant block.
        vector<CookedPushConstantBlock> PushConstants;
        /// @brief A compute entry point's threadgroup size along x, y and z; zero for every other
        ///        stage.
        std::array<u32, 3> ThreadgroupSize{};
    };

    /// @brief Reflects one entry point's interface, and checks its Metal layout against SPIR-V's.
    ///
    /// The SPIR-V layout supplies the bindings and push constants. When @p metal is given, every
    /// binding is found in it too, each binding's MetalBinding and MetalIndex are filled in, and
    /// the program is held to the Metal binding model: every set is a parameter block in the
    /// buffer slot its set index names, a resource outside one is a dynamic uniform buffer at a
    /// reserved slot, the push-constant block is at its reserved slot, and every member of every
    /// constant block and structured-buffer element sits at the same offset on both targets.
    /// @param spirv      The linked program's SPIR-V layout, holding exactly one entry point.
    /// @param metal      The linked program's Metal layout, or null when Metal is not compiled.
    /// @param shaderName The shader's name, which every error leads with.
    /// @return The interface, or an error naming the shader and the binding, struct or member.
    [[nodiscard]] VE_COOK_API Result<ReflectedShaderInterface>
    ReflectShaderInterface(slang::ProgramLayout& spirv, slang::ProgramLayout* metal,
                           std::string_view shaderName);
}
