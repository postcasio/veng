#pragma once

#include <unordered_map>

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VolumeMarch.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;
    class PipelineLayout;
    class DescriptorSet;
    class DescriptorSetLayout;
    class VolumeField;

    /// @brief Ray-marches the scene's live volume fields into the lit scene color, depth-aware.
    ///
    /// A fullscreen march per live field, drawn far-to-near by camera distance to bounds center and
    /// blended (srcColor = ONE, dstColor = SRC_ALPHA): the fragment integrates the field's
    /// emission+extinction along each view ray and returns float4(accumulated emission, surviving
    /// transmittance), so one blend adds the medium's glow and attenuates the background behind it.
    /// Occupies the scene-color slot after the sky composite and before the refraction grab /
    /// translucent pass — so the field attenuates the backdrop behind it, the refraction grab captures
    /// it, translucents blend over it, and TAA resolves the per-pixel march jitter.
    ///
    /// The g-buffer depth is sampled through set-0 bindless; each field's 3D texture + sampler bind
    /// through a dedicated per-pass set (set 1), never set-0 bindless — a Texture3D in set 0's Metal
    /// argument buffer is a MoltenVK risk the engine refuses (the IBL-cubemap precedent). The fields
    /// are borrowed from the renderer's resolved set (refilled each Execute); an empty set makes the
    /// pass a per-frame no-op, and the pass is inserted only while a live field exists, so a fieldless
    /// scene runs unchanged and the smoke golden is unaffected.
    class VolumeScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass, building the march pipeline.
        /// @param context       The render context.
        /// @param assets        Asset manager the march shader loads through (the core pack).
        /// @param fields        The renderer-owned resolved field set this Execute, refilled per frame.
        /// @param outputFormat  Color format of the lit scene-color target the pass composites into.
        /// @param samplerHandle Shared sampler bindless handle for the g-buffer depth sample.
        VolumeScenePass(Context& context, AssetManager& assets,
                        const vector<VolumeFieldInstance>* fields, Format outputFormat,
                        SamplerHandle samplerHandle);

        /// @brief Destroys the pass's owned GPU resources.
        ~VolumeScenePass() override;

        /// @brief Contributes the fullscreen volume-march pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Returns the dedicated set for a field, allocating and writing it on first use.
        ///
        /// The field's 3D view + sampler are immutable for its life, so the set is written once and
        /// bound across frames without a per-frame rewrite (unlike the point-field ring); pruned when
        /// the field is no longer resolved.
        /// @param field The built field the set binds.
        /// @return The field's set-1 descriptor set (binding 0 the 3D view, binding 1 the sampler).
        const Ref<DescriptorSet>& SetFor(const VolumeField* field);

        /// @brief Draws every resolved field far-to-near as one fullscreen march each.
        /// @param cmd The frame's command buffer.
        /// @param ctx The scene pass context (view + resolved resources).
        void DrawFields(CommandBuffer& cmd, const ScenePassContext& ctx);

        /// @brief The render context.
        Context& m_Context;
        /// @brief Borrowed pointer to the renderer's resolved field set, refilled each Execute.
        const vector<VolumeFieldInstance>* m_Fields;
        /// @brief Lit scene-color format the pipeline targets.
        Format m_OutputFormat;
        /// @brief Shared sampler bindless handle for the depth sample.
        SamplerHandle m_SamplerHandle;

        /// @brief The fullscreen march pipeline (set 0 bindless + set 1 volume, (ONE, SRC_ALPHA) blend).
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief Shared layout (set 0 bindless reserved + set 1 volume + push block).
        Ref<PipelineLayout> m_Layout;
        /// @brief Set-1 layout: binding 0 the 3D sampled image, binding 1 the sampler.
        Ref<DescriptorSetLayout> m_SetLayout;

        /// @brief The g-buffer depth bindless slot, resolved in Declare.
        u32 m_DepthTextureIndex = 0;
        /// @brief The shared sampler bindless slot, resolved in Declare.
        u32 m_SamplerIndex = 0;

        /// @brief Per-field dedicated sets, keyed by field pointer, pruned when a field goes.
        std::unordered_map<const VolumeField*, Ref<DescriptorSet>> m_FieldSets;

        /// @brief Monotonic frame counter feeding the shader's temporal jitter rotation.
        u32 m_FrameIndex = 0;
    };
}
