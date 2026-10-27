#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/SphereTerrain.h>
#include <Veng/Result.h>

namespace Veng::Renderer
{
    class Context;
    class DescriptorSetLayout;
    class PipelineLayout;

    /// @brief The descriptor set the terrain stages read the pool and the frame's tiles through.
    inline constexpr u32 SphereTerrainSet = 4;

    /// @brief The binding of the frame's tile records; channel n is at binding n below it.
    inline constexpr u32 SphereTerrainRecordsBinding = MaxSphereTerrainChannels;

    /// @brief The binding of the frame's per-view tile lists.
    inline constexpr u32 SphereTerrainListBinding = MaxSphereTerrainChannels + 1;

    /// @brief Creates the terrain's set-4 layout: every channel's array as a sampled image, the
    ///        records and the lists as storage buffers, all visible to the vertex and fragment
    ///        stages.
    ///
    /// Every terrain pipeline is built against a layout defined exactly this way, so one set
    /// written per frame binds under any terrain material's pipeline and the shadow stage's alike.
    [[nodiscard]] Ref<DescriptorSetLayout> CreateSphereTerrainSetLayout(Context& context);

    /// @brief A material's reflected pipeline layout with its set 4 replaced by the terrain's.
    ///
    /// Fails when the material's reflected set 4 declares a binding the terrain's layout does not
    /// carry, or when the reflection holds no set 4 at all (the material's stages are not the
    /// terrain's).
    /// @param context    Render context.
    /// @param reflected  The layout reflected from the material's stages.
    /// @param name       The new layout's debug name.
    [[nodiscard]] Result<Ref<PipelineLayout>>
    WithSphereTerrainSet(Context& context, const PipelineLayout& reflected, string name);
}
