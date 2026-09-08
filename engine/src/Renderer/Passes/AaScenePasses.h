#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;

    /// @brief FXAA push block, matching fxaa.frag PushConstants.
    ///
    /// The tonemapped LDR input slot, the shared sampler, and the reciprocal of the output extent
    /// (UV per texel) the shader offsets its neighbourhood taps by.
    struct FxaaPush
    {
        /// @brief Bindless sampled-image index of the tonemapped LDR input.
        u32 Texture;
        /// @brief Bindless sampler index (the shared linear sampler).
        u32 Sampler;
        /// @brief 1 / output extent, in UV per texel.
        vec2 RcpFrame;
    };

    /// @brief CMAA2 apply push block, matching cmaa2_apply.frag PushConstants.
    ///
    /// The tonemapped LDR colour slot, the edge-map slot the edge pass wrote, the shared sampler,
    /// and the reciprocal extent. Pad0 keeps the trailing vec2 at its 8-byte-aligned offset, matching
    /// the SPIR-V push block layout (three preceding u32s would otherwise leave it at 12).
    struct Cmaa2ApplyPush
    {
        /// @brief Bindless sampled-image index of the tonemapped LDR colour.
        u32 ColorTexture;
        /// @brief Bindless sampled-image index of the RG8 edge map.
        u32 EdgeTexture;
        /// @brief Bindless sampler index (the shared linear sampler).
        u32 Sampler;
        /// @brief Padding so RcpFrame lands at an 8-byte-aligned offset.
        u32 Pad0;
        /// @brief 1 / extent, in UV per texel.
        vec2 RcpFrame;
    };

    /// @brief FXAA resolve pass, reused for the CMAA2 edge pass.
    ///
    /// Reads one bindless-sampled input and writes a target through the shared texture + sampler +
    /// reciprocal-extent push block. As the FXAA resolve it reads the tonemapped LDR intermediate
    /// (AaResolve owns it) and writes the anti-aliased output; as the CMAA2 edge pass it reads the
    /// same intermediate and writes the RG8 edge map. The declared .Sample on the input id drives the
    /// graph-derived attachment → shader-read transition. Spatial and history-free, so it needs no
    /// velocity and composes with dynamic resolution (it runs on the already-upscaled LDR).
    class FxaaScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context      Renderer context for bindless access.
        /// @param pipeline     The FXAA pipeline.
        /// @param inputId      The tonemapped LDR intermediate (the resolve's input).
        /// @param outputId     The output the resolve writes.
        /// @param inputHandle  Bindless slot for the input target.
        /// @param samplerHandle The shared linear sampler slot.
        /// @param extent       Initial render extent; updated via Resize.
        /// @param label        The graph/profiler pass name ("FXAA", or "CMAA2 Edges" when reused).
        FxaaScenePass(Context& context, Ref<GraphicsPipeline> pipeline, ResourceId inputId,
                      ResourceId outputId, TextureHandle inputHandle, SamplerHandle samplerHandle,
                      uvec2 extent, const char* label = "FXAA")
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_InputId(inputId),
              m_OutputId(outputId), m_InputHandle(inputHandle), m_SamplerHandle(samplerHandle),
              m_Extent(extent), m_Label(label)
        {
        }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the FXAA resolve pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The FXAA pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief The tonemapped LDR intermediate (the resolve's input).
        ResourceId m_InputId;
        /// @brief The output the resolve writes.
        ResourceId m_OutputId;
        /// @brief Bindless slot for the input target.
        TextureHandle m_InputHandle;
        /// @brief The shared linear sampler slot.
        SamplerHandle m_SamplerHandle;
        /// @brief Current render extent.
        uvec2 m_Extent;
        /// @brief The graph/profiler pass name.
        const char* m_Label;
    };

    /// @brief CMAA2 apply pass: the edge-directed morphological blend.
    ///
    /// Reads the tonemapped LDR colour and the RG8 edge map the edge pass wrote, follows each
    /// silhouette to its ends, and blends across the edge by the shape's morphological coverage,
    /// writing the output. Only edge pixels are touched; everything else passes through. Declares
    /// .Sample on both inputs for the graph-derived barriers.
    class Cmaa2ApplyScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context       Renderer context for bindless access.
        /// @param pipeline      The CMAA2 apply pipeline.
        /// @param colorId       The tonemapped LDR intermediate (the blend's colour input).
        /// @param edgeId        The RG8 edge map (the blend's edge input).
        /// @param outputId      The output the blend writes.
        /// @param colorHandle   Bindless slot for the colour input.
        /// @param edgeHandle    Bindless slot for the edge map.
        /// @param samplerHandle The shared linear sampler slot.
        /// @param extent        Initial render extent; updated via Resize.
        Cmaa2ApplyScenePass(Context& context, Ref<GraphicsPipeline> pipeline, ResourceId colorId,
                            ResourceId edgeId, ResourceId outputId, TextureHandle colorHandle,
                            TextureHandle edgeHandle, SamplerHandle samplerHandle, uvec2 extent)
            : m_Context(context), m_Pipeline(std::move(pipeline)), m_ColorId(colorId),
              m_EdgeId(edgeId), m_OutputId(outputId), m_ColorHandle(colorHandle),
              m_EdgeHandle(edgeHandle), m_SamplerHandle(samplerHandle), m_Extent(extent)
        {
        }

        /// @brief Updates the render extent.
        void Resize(uvec2 extent) override { m_Extent = extent; }
        /// @brief Contributes the CMAA2 apply pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The CMAA2 apply pipeline.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief The tonemapped LDR colour input.
        ResourceId m_ColorId;
        /// @brief The RG8 edge map input.
        ResourceId m_EdgeId;
        /// @brief The output the blend writes.
        ResourceId m_OutputId;
        /// @brief Bindless slot for the colour input.
        TextureHandle m_ColorHandle;
        /// @brief Bindless slot for the edge map.
        TextureHandle m_EdgeHandle;
        /// @brief The shared linear sampler slot.
        SamplerHandle m_SamplerHandle;
        /// @brief Current render extent.
        uvec2 m_Extent;
    };
}
