#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>

namespace Veng
{
    class Texture;

    /// @brief A flipbook's recommended compositing.
    ///
    /// Stored as the underlying integer in CookedFlipbookHeader::Blend.
    enum class FlipbookBlend : u32
    {
        /// @brief Composited over the scene by its coverage.
        Alpha = 0,
        /// @brief Added onto the scene, an emissive glow.
        Additive = 1,
    };

    /// @brief How a flipbook atlas's alpha channel relates to its colour.
    ///
    /// Stored as the underlying integer in CookedFlipbookHeader::AlphaMode.
    enum class FlipbookAlpha : u32
    {
        /// @brief Colour is already multiplied by coverage.
        Premultiplied = 0,
        /// @brief Alpha is straight coverage; colour is independent of it.
        Coverage = 1,
        /// @brief Alpha is derived from the colour's luminance; the stored alpha is ignored.
        Luminance = 2,
        /// @brief Every texel is fully covered; the stored alpha is ignored.
        Opaque = 3,
    };

    /// @brief A flipbook's playback timing: how many frames it has, how fast, and whether it loops.
    struct FlipbookClip
    {
        /// @brief Populated frames in the sequence (at least one).
        u32 FrameCount = 1;
        /// @brief The authored playback rate, in frames per second.
        f32 Fps = 30.0f;
        /// @brief Whether the sequence wraps back to its first frame after its last.
        bool Loop = false;
    };

    /// @brief Resolves the frame rate a use of a clip plays at.
    ///
    /// A baked sequence's authored timing belongs to the bake, not to every use of it, so a use
    /// may either scale the authored rate or fit the whole sequence to a duration of its own.
    /// @param clip              The clip's authored timing.
    /// @param playbackRate      Multiplies the authored rate; ignored when @p durationOverride > 0.
    /// @param durationOverride  When positive, the whole sequence plays over this many seconds.
    /// @return The frames advanced per second; never negative.
    [[nodiscard]] VE_API f32 ResolveFlipbookRate(const FlipbookClip& clip, f32 playbackRate,
                                                 f32 durationOverride);

    /// @brief Returns the frame a clip shows at a playback time.
    ///
    /// `floor(time · rate)`, wrapped by the frame count for a looping clip and clamped to the last
    /// frame otherwise. A negative time shows the first frame.
    /// @param clip  The clip's timing (its Fps is not read; @p rate replaces it).
    /// @param rate  Frames per second, from ResolveFlipbookRate.
    /// @param time  Seconds since playback began.
    /// @return The frame index, in [0, FrameCount).
    [[nodiscard]] VE_API u32 FlipbookFrameAt(const FlipbookClip& clip, f32 rate, f32 time);

    /// @brief Whether a clip has played through its last frame at a playback time.
    ///
    /// A looping clip never finishes. A one-shot finishes once its last frame has been shown for
    /// its full duration, `time · rate ≥ FrameCount`, and a clip playing at a rate of zero never
    /// advances and so never finishes.
    /// @param clip  The clip's timing.
    /// @param rate  Frames per second, from ResolveFlipbookRate.
    /// @param time  Seconds since playback began.
    /// @return True when a one-shot clip has ended.
    [[nodiscard]] VE_API bool IsFlipbookFinished(const FlipbookClip& clip, f32 rate, f32 time);

    /// @brief Construction parameters for a Flipbook.
    struct FlipbookInfo
    {
        /// @brief Debug name.
        string Name;
        /// @brief The atlas texture every frame is cut from.
        Ref<Texture> Atlas;
        /// @brief Tiles across the atlas.
        u32 Columns = 1;
        /// @brief Tiles down the atlas.
        u32 Rows = 1;
        /// @brief One tile's size in source pixels.
        uvec2 FrameSize{1, 1};
        /// @brief The playback timing.
        FlipbookClip Clip;
        /// @brief The recommended compositing.
        FlipbookBlend Blend = FlipbookBlend::Alpha;
        /// @brief How the atlas's alpha relates to its colour.
        FlipbookAlpha AlphaMode = FlipbookAlpha::Premultiplied;
        /// @brief World-space size one frame represents, when the source knows it.
        optional<vec3> WorldExtent;
        /// @brief The effect's anchor inside WorldExtent, in world units from its minimum corner.
        optional<vec3> Pivot;
    };

    /// @brief A grid-packed sprite-sheet atlas with its playback timing — the flipbook asset.
    ///
    /// Cooked from a flipbook-atlas manifest (schema v1) and its image. Frame f is the tile at
    /// column `f % Columns`, row `f / Columns`, counted from the atlas's top-left; the populated
    /// frames run row-major. The asset is data a player reads — FlipbookSprite plays one in the
    /// world — and carries no playback state of its own.
    class Flipbook
    {
    public:
        /// @brief Builds a flipbook over an existing atlas texture.
        ///
        /// The runtime counterpart of a cooked flipbook, for a sheet generated or uploaded in code.
        /// @param info  The atlas, grid, timing, and compositing.
        /// @pre info.Atlas is non-null, the grid is at least 1x1, and the frame count fits it.
        /// @return The flipbook.
        [[nodiscard]] static Ref<Flipbook> Create(const FlipbookInfo& info);

        /// @brief Returns the debug name.
        [[nodiscard]] const string& GetName() const { return m_Info.Name; }

        /// @brief Returns the atlas texture.
        [[nodiscard]] const Ref<Texture>& GetAtlas() const { return m_Info.Atlas; }

        /// @brief Returns the number of tiles across the atlas.
        [[nodiscard]] u32 GetColumns() const { return m_Info.Columns; }

        /// @brief Returns the number of tiles down the atlas.
        [[nodiscard]] u32 GetRows() const { return m_Info.Rows; }

        /// @brief Returns one tile's size in source pixels.
        [[nodiscard]] uvec2 GetFrameSize() const { return m_Info.FrameSize; }

        /// @brief Returns the playback timing.
        [[nodiscard]] const FlipbookClip& GetClip() const { return m_Info.Clip; }

        /// @brief Returns the recommended compositing.
        [[nodiscard]] FlipbookBlend GetBlend() const { return m_Info.Blend; }

        /// @brief Returns how the atlas's alpha relates to its colour.
        [[nodiscard]] FlipbookAlpha GetAlphaMode() const { return m_Info.AlphaMode; }

        /// @brief Returns the world-space size one frame represents, when the source knows it.
        [[nodiscard]] const optional<vec3>& GetWorldExtent() const { return m_Info.WorldExtent; }

        /// @brief Returns the effect's anchor inside its world extent, when the source records one.
        [[nodiscard]] const optional<vec3>& GetPivot() const { return m_Info.Pivot; }

        /// @brief Returns a frame's height over its width.
        [[nodiscard]] f32 GetAspect() const;

        /// @brief Returns the atlas sub-rectangle a frame occupies, in texture coordinates.
        ///
        /// The rectangle is inset by half a source texel on each side so bilinear filtering at
        /// the tile's edge never reads its neighbour. A frame past the last is clamped to it.
        /// @param frame  The frame index.
        /// @return (u0, v0, u1, v1), with (u0, v0) the tile's top-left.
        [[nodiscard]] vec4 GetFrameRect(u32 frame) const;

        /// @brief Returns where the effect's anchor sits within a frame, as a fraction of it.
        ///
        /// x runs left to right and y bottom to top. The anchor is the authored pivot projected
        /// onto the frame's width (the extent's x) and height (its y); a source recording no pivot,
        /// or no extent to measure it against, anchors at the frame's centre.
        /// @return The anchor, (0.5, 0.5) for a centred one.
        [[nodiscard]] vec2 GetAnchor() const;

    private:
        /// @brief Constructs from validated parameters; use Create.
        explicit Flipbook(FlipbookInfo info) : m_Info(std::move(info)) {}

        /// @brief The atlas, grid, timing, and compositing.
        FlipbookInfo m_Info;
    };

    /// @brief AssetTypeTrait specialization mapping Flipbook to AssetTypes::Flipbook.
    template <>
    struct AssetTypeTrait<Flipbook>
    {
        /// @brief The asset type tag for Flipbook.
        static constexpr AssetTypeId Type = AssetTypes::Flipbook;
    };
}
