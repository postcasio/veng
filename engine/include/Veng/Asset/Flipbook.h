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

    /// @brief What a flipbook's textures hold.
    ///
    /// Stored as the underlying integer in CookedFlipbookHeader::Shading.
    enum class FlipbookShading : u32
    {
        /// @brief One finished, premultiplied colour sheet.
        Unlit = 0,
        /// @brief Two six-way lightmap sheets and a temperature, shaded by the renderer.
        ///
        /// The atlas holds the light received from +X, +Y and -Z in RGB and coverage in alpha; the
        /// second lightmap holds the light received from -X, -Y and +Z. The axes are the sprite's
        /// own frame: X right, Y up, Z toward the viewer.
        SixWay = 1,
    };

    /// @brief How a player is recommended to blend between consecutive frames.
    ///
    /// Stored as the underlying integer in CookedFlipbookHeader::FrameBlend. The cook derives it
    /// from the source rather than reading it.
    enum class FlipbookFrameBlend : u32
    {
        /// @brief Show one frame at a time.
        Step = 0,
        /// @brief Fade from one frame to the next.
        Crossfade = 1,
        /// @brief Warp both frames toward each other along the motion vectors, then fade.
        Motion = 2,
    };

    /// @brief The transfer a flipbook's stored temperature is under.
    ///
    /// Stored as the underlying integer in CookedFlipbookHeader::TemperatureEncoding.
    enum class FlipbookTemperatureEncoding : u32
    {
        /// @brief The stored value is the normalised temperature t.
        Linear = 0,
        /// @brief The stored value is sqrt(t), spending precision on the hot end.
        Sqrt = 1,
    };

    /// @brief One key of a flipbook's emission ramp.
    struct FlipbookEmissionKey
    {
        /// @brief The normalised temperature the key sits at, in [0, 1].
        f32 T = 0.0f;
        /// @brief Linear HDR emission in the renderer's exposure-scaled units.
        vec3 Color{0.0f};
    };

    /// @brief The most keys a flipbook's emission ramp holds.
    inline constexpr u32 MaxFlipbookEmissionKeys = 8;

    /// @brief A flipbook's default emission: the colour each temperature glows, and where it starts.
    struct FlipbookEmission
    {
        /// @brief The ramp's keys, ascending in T; empty when the flipbook emits nothing by default.
        vector<FlipbookEmissionKey> Ramp;
        /// @brief The normalised temperature below which nothing emits.
        f32 Ignition = 0.0f;
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
        /// @brief The primary texture every frame is cut from.
        ///
        /// The colour sheet of an unlit flipbook, or the +X/+Y/-Z lightmap (with coverage in
        /// alpha) of a six-way one.
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
        /// @brief What the textures hold.
        FlipbookShading Shading = FlipbookShading::Unlit;
        /// @brief The -X/-Y/+Z lightmap of a six-way flipbook, its temperature in alpha unless
        ///        Temperature is set; null for an unlit one.
        Ref<Texture> LightB;
        /// @brief The motion-vector sheet, or null.
        ///
        /// RG hold the offset carrying a texel of frame N to its place in frame N+1, in units of
        /// the frame's world extent with v running down the frame, stored as `0.5 + m / (2s)` for
        /// a MotionScale of s.
        Ref<Texture> Motion;
        /// @brief A six-way flipbook's temperature as a texture of its own, read from red; null
        ///        when it rides in LightB's alpha.
        Ref<Texture> Temperature;
        /// @brief The largest motion offset the motion sheet represents; positive exactly when
        ///        Motion is set.
        f32 MotionScale = 0.0f;
        /// @brief The transfer the stored temperature is under.
        FlipbookTemperatureEncoding TemperatureEncoding = FlipbookTemperatureEncoding::Sqrt;
        /// @brief The default emission ramp.
        FlipbookEmission Emission;
        /// @brief How a player is recommended to blend between frames.
        FlipbookFrameBlend FrameBlend = FlipbookFrameBlend::Step;
        /// @brief Each frame's extent as a fraction of WorldExtent, centred on the full frame;
        ///        empty when every frame is full size.
        vector<f32> FrameScale;
    };

    /// @brief A grid-packed sprite-sheet atlas with its playback timing — the flipbook asset.
    ///
    /// Cooked from a flipbook-atlas manifest and its images (the format is documented in the
    /// cooker's `FLIPBOOK_ATLAS_SCHEMA.md`). Every texture shares one grid: frame f is the tile at
    /// column `f % Columns`, row `f / Columns`, counted from the top-left; the populated frames run
    /// row-major. An unlit flipbook is one finished colour sheet. A six-way flipbook carries data
    /// for the renderer to shade — two lightmaps, a temperature and a default emission ramp —
    /// and either kind may carry a motion-vector sheet. The asset is data a player reads —
    /// FlipbookSprite plays one in the world — and carries no playback state of its own.
    class VE_API Flipbook
    {
    public:
        /// @brief Builds a flipbook over an existing atlas texture.
        ///
        /// The runtime counterpart of a cooked flipbook, for a sheet generated or uploaded in code.
        /// @param info  The textures, grid, timing, and compositing.
        /// @pre info.Atlas is non-null, the grid is at least 1x1, and the frame count fits it. A
        ///      six-way flipbook has LightB and an unlit one has neither LightB nor Temperature;
        ///      Motion comes with a positive MotionScale; every texture has the atlas's extent; the
        ///      ramp holds at most MaxFlipbookEmissionKeys ascending keys; FrameScale is empty or
        ///      holds one entry in (0, 1] per frame.
        /// @return The flipbook.
        [[nodiscard]] static Ref<Flipbook> Create(const FlipbookInfo& info);

        /// @brief Returns the debug name.
        [[nodiscard]] const string& GetName() const { return m_Info.Name; }

        /// @brief Returns the primary texture: the colour sheet, or a six-way flipbook's first
        ///        lightmap.
        [[nodiscard]] const Ref<Texture>& GetAtlas() const { return m_Info.Atlas; }

        /// @brief Returns what the textures hold.
        [[nodiscard]] FlipbookShading GetShading() const { return m_Info.Shading; }

        /// @brief Returns a six-way flipbook's -X/-Y/+Z lightmap; null for an unlit one.
        [[nodiscard]] const Ref<Texture>& GetLightB() const { return m_Info.LightB; }

        /// @brief Returns the motion-vector sheet, or null.
        [[nodiscard]] const Ref<Texture>& GetMotion() const { return m_Info.Motion; }

        /// @brief Returns the largest motion offset the motion sheet represents; 0 without one.
        [[nodiscard]] f32 GetMotionScale() const { return m_Info.MotionScale; }

        /// @brief Returns the default emission ramp.
        [[nodiscard]] const FlipbookEmission& GetEmission() const { return m_Info.Emission; }

        /// @brief Returns how a player is recommended to blend between frames.
        [[nodiscard]] FlipbookFrameBlend GetFrameBlend() const { return m_Info.FrameBlend; }

        /// @brief Returns the transfer the stored temperature is under.
        [[nodiscard]] FlipbookTemperatureEncoding GetTemperatureEncoding() const
        {
            return m_Info.TemperatureEncoding;
        }

        /// @brief Returns the texture holding the temperature: its own texture when it has one,
        ///        else the second lightmap; null for an unlit flipbook.
        [[nodiscard]] const Ref<Texture>& GetTemperatureTexture() const
        {
            return m_Info.Temperature != nullptr ? m_Info.Temperature : m_Info.LightB;
        }

        /// @brief Returns the channel of GetTemperatureTexture() the temperature is read from.
        /// @return 0 (red) for a texture of its own, 3 (alpha) for the second lightmap.
        [[nodiscard]] u32 GetTemperatureChannel() const
        {
            return m_Info.Temperature != nullptr ? 0u : 3u;
        }

        /// @brief Returns a frame's extent as a fraction of the world extent.
        /// @param frame  The frame index; one past the last is clamped to it.
        /// @return The frame's scale, 1 when the flipbook records none.
        [[nodiscard]] f32 GetFrameScale(u32 frame) const;

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
