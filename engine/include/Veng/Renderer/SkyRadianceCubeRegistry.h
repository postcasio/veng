#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class ImageView;

    /// @brief A published baked sky radiance cube: its sampled view and face edge length.
    struct SkyRadianceCube
    {
        /// @brief The cube's sampled image view (all six faces, mip 0), owned by the publisher.
        Ref<ImageView> View;
        /// @brief The cube's face edge length in texels; an adopter copies only a size-matching cube.
        u32 FaceSize = 0;
    };

    /// @brief A Context-owned map of content key to a published baked sky radiance cube.
    ///
    /// The sky the whole scene sees is one baked radiance cube, but every SceneRenderer that shows it
    /// — the main viewport and each canopy/mirror SceneCapture of the same world — owns its own
    /// SkyResolver and would otherwise each bake that cube (a heavy per-face dust/atmosphere march)
    /// independently. The bake is a pure function of a caller-supplied content key (see
    /// MaterialSky::BakeKey), so a renderer whose bake completes **publishes** its finished cube here
    /// under that key, and a renderer that needs the same key **adopts** it — a one-blit copy into
    /// its own cube instead of a fresh bake. A world swap that re-spawns a capture then costs a copy
    /// where it used to cost a full re-bake.
    ///
    /// Non-owning of lifetime beyond the stored Ref: the publisher owns the cube; it removes its
    /// entry when its key changes or it is destroyed, so an adopter only ever copies a cube whose
    /// publisher is alive and whose content still matches the key. The registry holds Refs, so a
    /// still-published entry keeps the view alive, but publishers are torn down (with the renderers
    /// that own them) before the Context, so the map is empty by Context teardown. Single render
    /// thread: no synchronization.
    class SkyRadianceCubeRegistry
    {
    public:
        /// @brief Publishes (or replaces) the finished cube for @p key.
        /// @param key      The content key the cube was baked for (nonzero).
        /// @param view     The cube's sampled view (all six faces, mip 0).
        /// @param faceSize The cube's face edge length in texels.
        void Publish(u64 key, Ref<ImageView> view, u32 faceSize)
        {
            if (key == 0)
            {
                return;
            }
            m_Cubes[key] = SkyRadianceCube{.View = std::move(view), .FaceSize = faceSize};
        }

        /// @brief Removes the entry for @p key, if any (a no-op when absent or zero).
        void Remove(u64 key) { m_Cubes.erase(key); }

        /// @brief The published cube for @p key, or nullptr when none is published.
        ///
        /// The returned pointer is valid only until the next Publish/Remove; a caller copies from the
        /// view on the spot rather than retaining it.
        /// @param key  The content key to look up.
        /// @return The published cube, or nullptr.
        [[nodiscard]] const SkyRadianceCube* Find(u64 key) const
        {
            if (key == 0)
            {
                return nullptr;
            }
            const auto it = m_Cubes.find(key);
            return it != m_Cubes.end() ? &it->second : nullptr;
        }

    private:
        /// @brief The content-key to published-cube map.
        unordered_map<u64, SkyRadianceCube> m_Cubes;
    };
}
