#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Context;
    class MetalFxUpscale;

    /// @brief Writes the motion MetalFX's temporal scaler reconstructs from.
    ///
    /// The velocity channel where geometry stands and the camera's turn at the cleared depth, the
    /// background rule the engine's own resolve applies (metalfx_motion.frag). Runs over the
    /// rendered sub-rect ahead of MetalFxTemporalScenePass, which reads the target it writes.
    class MetalFxMotionScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param context        Renderer context for bindless access.
        /// @param upscale        The owner of the motion pipeline.
        /// @param depthId        The g-buffer depth.
        /// @param velocityId     The g-buffer velocity channel.
        /// @param motionId       The motion target.
        /// @param velocityHandle The velocity channel's bindless slot.
        MetalFxMotionScenePass(Context& context, const MetalFxUpscale& upscale, ResourceId depthId,
                               ResourceId velocityId, ResourceId motionId,
                               TextureHandle velocityHandle)
            : m_Context(context), m_Upscale(upscale), m_DepthId(depthId), m_VelocityId(velocityId),
              m_MotionId(motionId), m_VelocityHandle(velocityHandle)
        {
        }

        /// @brief Declares the motion write and its two reads.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief Renderer context for bindless access.
        Context& m_Context;
        /// @brief The owner of the motion pipeline.
        const MetalFxUpscale& m_Upscale;
        /// @brief The g-buffer depth.
        ResourceId m_DepthId;
        /// @brief The g-buffer velocity channel.
        ResourceId m_VelocityId;
        /// @brief The motion target.
        ResourceId m_MotionId;
        /// @brief The velocity channel's bindless slot.
        TextureHandle m_VelocityHandle;
    };

    /// @brief The temporal resolve as MetalFX's temporal scaler.
    ///
    /// Takes the TAA resolve's place under AntiAliasingMode::MetalFXTemporal: reads the jittered lit
    /// target, the g-buffer depth and the motion target, and reconstructs the scene-colour
    /// allocation into the target the HDR tail reads. The scaler keeps its own history, so there is
    /// no history copy. Its work runs outside Vulkan, between two submits of the frame
    /// (Context::SplitFrame); the declared accesses place the barriers either side of it.
    class MetalFxTemporalScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param upscale       The owner of the temporal scaler, read at record time.
        /// @param litId         The jittered lit target.
        /// @param depthId       The g-buffer depth.
        /// @param velocityId    The motion target MetalFxMotionScenePass writes.
        /// @param outputId      The scene colour the scaler reconstructs.
        /// @param historyReset  The renderer's history-reset gate, read at record time.
        MetalFxTemporalScenePass(const MetalFxUpscale& upscale, ResourceId litId,
                                 ResourceId depthId, ResourceId velocityId, ResourceId outputId,
                                 const bool* historyReset)
            : m_Upscale(upscale), m_LitId(litId), m_DepthId(depthId), m_VelocityId(velocityId),
              m_OutputId(outputId), m_HistoryReset(historyReset)
        {
        }

        /// @brief Declares the scaler's reads and its write, and encodes it at record time.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief The owner of the temporal scaler.
        const MetalFxUpscale& m_Upscale;
        /// @brief The jittered lit target.
        ResourceId m_LitId;
        /// @brief The g-buffer depth.
        ResourceId m_DepthId;
        /// @brief The motion target.
        ResourceId m_VelocityId;
        /// @brief The scene colour the scaler reconstructs.
        ResourceId m_OutputId;
        /// @brief The renderer's history-reset gate.
        const bool* m_HistoryReset;
    };

    /// @brief The scene-colour promotion as MetalFX's spatial scaler.
    ///
    /// Takes the bilinear promotion's place under UpscaleFilter::MetalFXSpatial: reads the scene
    /// colour's valid extent and writes the post-resolve allocation the tail reads. Its work runs
    /// between two submits of the frame, like the temporal pass.
    class MetalFxSpatialScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass.
        /// @param upscale   The owner of the spatial scaler, read at record time.
        /// @param sourceId  The scene colour at the tail anchor.
        /// @param outputId  The promoted, post-resolve-allocation scene colour.
        MetalFxSpatialScenePass(const MetalFxUpscale& upscale, ResourceId sourceId,
                                ResourceId outputId)
            : m_Upscale(upscale), m_SourceId(sourceId), m_OutputId(outputId)
        {
        }

        /// @brief Declares the scaler's read and its write, and encodes it at record time.
        void Declare(RenderGraph& graph, const PassIO& io) override;

    private:
        /// @brief The owner of the spatial scaler.
        const MetalFxUpscale& m_Upscale;
        /// @brief The scene colour at the tail anchor.
        ResourceId m_SourceId;
        /// @brief The promoted scene colour.
        ResourceId m_OutputId;
    };
}
