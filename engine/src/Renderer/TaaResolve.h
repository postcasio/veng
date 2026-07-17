#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Veng.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class Image;
    class ImageView;
    class GraphicsPipeline;
    class PipelineLayout;

    /// @brief Owns the TAA resolve/history targets, their pipelines, and the history-reset gate.
    ///
    /// The temporal anti-aliasing vertical the renderer wires between lighting and the tonemap
    /// tail: the resolve + history-copy pipelines, the separate lit target the lighting pass
    /// writes under TAA, the persisted history the resolve reprojects against, both targets'
    /// bindless slots, and the history-reset flag TaaScenePass reads at record time. The passes
    /// themselves are TaaScenePass, which the renderer constructs from the pipelines/handles this
    /// exposes; the previous-frame sub-rect UV state stays on the renderer (packed into the shared
    /// view-constants block the resolve shader reads), so no history-validity state lives here
    /// beyond the reset gate.
    class TaaResolve
    {
    public:
        /// @brief Creates the resolve/history-copy pipelines (targets are built by Resize).
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager used to load the resolve/copy fragment shaders.
        /// @return A new TaaResolve.
        static Unique<TaaResolve> Create(Context& context, AssetManager& assets);

        /// @brief Releases the lit/history bindless slots; the images retire through the frame bin.
        ~TaaResolve();

        TaaResolve(const TaaResolve&) = delete;
        TaaResolve& operator=(const TaaResolve&) = delete;

        /// @brief Recreates the lit + history targets at @p extent, or releases them when TAA is off.
        ///
        /// Both are HdrFormat at the full allocation extent, registered into bindless when
        /// @p enabled; otherwise the targets are dropped so the memory is not held for an unused
        /// path. Recreating (or releasing) invalidates the persisted history, so this sets the
        /// reset gate — the next resolve ignores history until a frame repopulates it.
        /// @param extent  The allocation extent both targets are sized to.
        /// @param enabled Whether TAA is active (the targets are allocated only then).
        void Resize(uvec2 extent, bool enabled);

        /// @brief Forces the next resolve to ignore history (used after a recreate or a reset).
        void InvalidateHistory() { m_HistoryReset = true; }

        /// @brief Clears the reset gate after an Execute has consumed it for this frame.
        void ClearHistoryReset() { m_HistoryReset = false; }

        /// @brief The TAA resolve pipeline (reproject + neighborhood-clip + blend), writing HdrFormat.
        [[nodiscard]] const Ref<GraphicsPipeline>& GetResolvePipeline() const
        {
            return m_ResolvePipeline;
        }

        /// @brief The TAA history-copy pipeline (unclamped passthrough into the history).
        [[nodiscard]] const Ref<GraphicsPipeline>& GetCopyPipeline() const
        {
            return m_CopyPipeline;
        }

        /// @brief The lit target (the resolve's current-frame input); null when TAA is off.
        [[nodiscard]] const Ref<ImageView>& GetLitView() const { return m_LitView; }

        /// @brief Bindless slot for the lit target; the resolve samples the current frame through it.
        [[nodiscard]] TextureHandle GetLitHandle() const { return m_LitHandle; }

        /// @brief The persisted history target; null when TAA is off.
        [[nodiscard]] const Ref<ImageView>& GetHistoryView() const { return m_HistoryView; }

        /// @brief Bindless slot for the history target; the resolve samples it at the reprojected UV.
        [[nodiscard]] TextureHandle GetHistoryHandle() const { return m_HistoryHandle; }

        /// @brief Pointer to the history-reset flag TaaScenePass reads at record time.
        ///
        /// The graph executes before Execute clears the flag, so a record-time read reflects this
        /// frame's history validity.
        [[nodiscard]] const bool* GetHistoryResetPointer() const { return &m_HistoryReset; }

    private:
        TaaResolve(Context& context, AssetManager& assets);

        Context& m_Context;

        /// @brief The TAA resolve pipeline (writes the HDR target the rest of the chain reads).
        Ref<GraphicsPipeline> m_ResolvePipeline;
        /// @brief Layout for the resolve pipeline: the resolve push block (no extra sets).
        Ref<PipelineLayout> m_ResolveLayout;
        /// @brief The TAA history-copy pipeline (writes the persisted history target).
        Ref<GraphicsPipeline> m_CopyPipeline;
        /// @brief Layout for the copy pipeline: a texture + sampler push block.
        Ref<PipelineLayout> m_CopyLayout;

        /// @brief Lighting target when TAA is active (the resolve's current-frame input).
        Ref<Image> m_LitImage;
        /// @brief View over m_LitImage.
        Ref<ImageView> m_LitView;
        /// @brief Bindless slot for the lit view.
        TextureHandle m_LitHandle;

        /// @brief Persisted previous-frame resolved HDR the resolve reprojects against.
        Ref<Image> m_HistoryImage;
        /// @brief View over m_HistoryImage.
        Ref<ImageView> m_HistoryView;
        /// @brief Bindless slot for the history view.
        TextureHandle m_HistoryHandle;

        /// @brief Forces the next resolve to ignore history (frame 0 and after Resize/Configure).
        bool m_HistoryReset = true;
    };
}
