#pragma once

#include <Veng/Renderer/SceneRendererSettings.h>
#include <Veng/Scene/Components.h>
#include <Veng/Veng.h>

#include "SkySourceKind.h"

namespace Veng::Renderer
{
    /// @brief How much of the depth-of-field chain a frame wires.
    ///
    /// The gate is a tri-state rather than a boolean because the two arms that reach it declare
    /// different pass sets: the debug arm inspects the circle of confusion without touching the
    /// HDR tail, while a fully active chain also gathers, fills, and composites — and re-routes
    /// the downstream source id through the composite.
    enum class DofStages : u8
    {
        /// @brief The chain is absent; nothing declares its resources.
        None,
        /// @brief Only the CoC/prefilter and tile-dilation stages run, feeding the debug blit.
        CocOnly,
        /// @brief The whole chain runs, including the composite that re-routes the HDR tail.
        Full,
    };

    /// @brief Whether a g-buffer colour channel's contents outlive the g-buffer pass.
    ///
    /// Every channel is allocated and written by the g-buffer pass whatever its state — the opaque
    /// material contract is five render targets unconditionally. The state decides only the pass's
    /// store op: on a tile-based GPU the store at the end of the pass is what spends memory
    /// bandwidth, so a channel nothing reads afterwards ends its life in tile memory.
    enum class GBufferChannelState : u8
    {
        /// @brief The pass stores the channel; a later pass reads it.
        Stored,
        /// @brief The pass writes the channel and discards it (StoreOp::DontCare); nothing reads it.
        Discarded,
    };

    /// @brief The store state of each of the five g-buffer colour channels.
    ///
    /// Depth is not listed: it has readers after the g-buffer pass in every configuration and is
    /// always stored.
    struct GBufferChannelStates
    {
        /// @brief G0, the albedo channel.
        GBufferChannelState Albedo = GBufferChannelState::Stored;
        /// @brief G1, the world-normal channel.
        GBufferChannelState Normal = GBufferChannelState::Stored;
        /// @brief G2, the packed occlusion/roughness/metallic channel.
        GBufferChannelState Orm = GBufferChannelState::Stored;
        /// @brief G3, the per-object motion-vector channel.
        GBufferChannelState Velocity = GBufferChannelState::Stored;
        /// @brief G4, the HDR emissive channel.
        GBufferChannelState Emissive = GBufferChannelState::Stored;

        /// @brief Compares every channel's state.
        /// @return True when both decide the same store for every channel.
        [[nodiscard]] bool operator==(const GBufferChannelStates&) const = default;
    };

    /// @brief The resolved sky facts the frame topology decides from.
    ///
    /// Plain data rather than the sky-resolve state machine itself, so the decision is a pure
    /// function of its arguments and constructible without a device.
    struct SkyTopologyInput
    {
        /// @brief The resolved sky source kind.
        SkySourceKind Kind = SkySourceKind::None;
        /// @brief The lighting tier the resolved sky was authored with.
        SkyLighting Lighting = SkyLighting::None;
        /// @brief Whether the resolved material/atmosphere source bakes to a radiance cube.
        bool IsBaked = false;
        /// @brief Whether a lighting cube other than the sky's own feeds the IBL tier.
        ///
        /// A scene-lighting capture or a Sky::LightingSource: image-based lighting is then wired
        /// whatever the source kind, with no sky source at all included.
        bool LightingCube = false;
    };

    /// @brief Which passes a frame's render graph wires, decided from the settings plus the sky.
    ///
    /// Every field is a topology decision the graph build reads back — never scene content, which
    /// is resolved per frame elsewhere. The struct is a value: the renderer holds the latest one
    /// and the later frame phases (the import bindings, the per-frame Execute work) consult it
    /// rather than a scatter of flags set as a side effect of building the graph.
    struct FrameTopology
    {
        /// @brief The frame runs the lean geometry path: a depth + world-normal prepass and nothing else.
        ///
        /// Set when SceneRendererSettings::Path is GeometryDepthNormal. It is exclusive with every
        /// other field here — a lean topology wires only the prepass and allocates none of the colour
        /// g-buffer / lighting / translucent / tonemap resources, so the resolve early-returns with
        /// this set and all else default.
        bool GeometryDepthNormal = false;

        /// @brief The frame composites the full lit scene before the HDR tail.
        bool SceneComposited = false;

        /// @brief The frame visualizes the accumulated bloom pyramid.
        bool DebugBloom = false;
        /// @brief The frame visualizes the directional shadow atlas.
        bool DebugShadow = false;
        /// @brief The frame visualizes the SSAO target.
        bool DebugAo = false;
        /// @brief The frame tints each fragment by the shadow cascade it selects.
        bool DebugCascades = false;
        /// @brief The frame visualizes the punctual shadow atlas.
        bool DebugPunctual = false;
        /// @brief The frame visualizes the raw SSR reflection target.
        bool DebugReflections = false;
        /// @brief The frame visualizes the per-pixel IBL ambient contribution (runs the lighting pass).
        bool DebugIblContribution = false;

        /// @brief The bloom sweep is wired and its pyramid imports are declared.
        bool BloomActive = false;
        /// @brief The auto-exposure metering pass is wired and drives the adapted exposure.
        bool AutoExposureActive = false;
        /// @brief The TAA resolve and history copy are wired, routing lighting into a lit target.
        bool TaaActive = false;
        /// @brief The temporal resolve reconstructs the post-resolve allocation, promoting there.
        ///
        /// Set for a temporal-upscaling frame whose chain admits it (see
        /// ResolveTemporalUpscalePromotes); the scene-colour chain from the resolve onward is then
        /// allocation-resolution and no spatial promotion follows it.
        bool TaaUpscalePromotes = false;
        /// @brief The FXAA post-tonemap resolve is wired (reads the tonemapped LDR).
        bool FxaaActive = false;
        /// @brief The CMAA2 post-tonemap compute resolve is wired (reads the tonemapped LDR).
        bool Cmaa2Active = false;
        /// @brief The directional shadow pass is wired.
        bool ShadowActive = false;
        /// @brief The punctual shadow pass is wired.
        bool PunctualShadowActive = false;
        /// @brief The SSAO term is folded into the lighting pass, selecting its variant pipeline.
        bool SsaoFold = false;
        /// @brief The SSAO pass is wired, whether folded into lighting or only visualized.
        bool SsaoActive = false;
        /// @brief The SSR trace, blur, and composite are wired.
        bool SsrActive = false;
        /// @brief The pre-translucent scene-color copy is wired.
        bool RefractionActive = false;
        /// @brief The per-tile light cull runs ahead of the lighting pass, which then visits only
        ///        each tile's lights.
        ///
        /// Set where the lighting pass shades direct light into an image someone reads — the
        /// composited arms and the debug arms that force-wire lighting as an input — and never for
        /// the two lighting variants that discard it (the cascade tint and the IBL-only arm).
        bool LightTileCullActive = false;
        /// @brief How much of the depth-of-field chain is wired.
        DofStages Dof = DofStages::None;

        /// @brief Which g-buffer colour channels the g-buffer pass stores for a later reader.
        ///
        /// Velocity is the one channel with no reader on most frames: only the temporal resolve
        /// and the MotionVectors debug blit sample it, so it is discarded when neither is wired.
        GBufferChannelStates GBufferStores;

        /// @brief The sky source bakes to a cube this frame, so the bake's set backs the skybox.
        bool BakedSkyWanted = false;
        /// @brief The sky is backed by a radiance cube, whether an environment's or a bake's.
        bool CubeBacked = false;
        /// @brief The cubemap skybox pass is wired.
        bool SkyboxWanted = false;
        /// @brief The per-pixel procedural atmosphere pass is wired.
        bool AtmosphereWanted = false;
        /// @brief The per-pixel authored sky-material pass is wired.
        bool SkyMaterialWanted = false;
        /// @brief The lighting pass folds in the sky's spherical-harmonic ambient.
        bool SkylightWanted = false;
        /// @brief The lighting pass lights from the sky's split-sum image-based lighting maps.
        bool IblAllowed = false;

        /// @brief Compares every decided field.
        /// @return True when both topologies wire the same passes.
        [[nodiscard]] bool operator==(const FrameTopology&) const = default;

        /// @brief Whether any depth-of-field stage is wired.
        /// @return True unless the chain is absent entirely.
        [[nodiscard]] bool DofWired() const { return Dof != DofStages::None; }

        /// @brief Whether the depth-of-field composite is wired and re-routes the HDR tail.
        /// @return True only for the fully active chain.
        [[nodiscard]] bool DofComposited() const { return Dof == DofStages::Full; }

        /// @brief Whether a post-tonemap AA resolve (FXAA or CMAA2) is wired.
        ///
        /// The two spatial resolves share the wiring shape — the tonemap writes an intermediate LDR
        /// target and the resolve reads it and writes the output — so the intermediate-target
        /// decision keys on this rather than on either flag alone.
        /// @return True when FXAA or CMAA2 is wired.
        [[nodiscard]] bool PostTonemapAa() const { return FxaaActive || Cmaa2Active; }
    };

    /// @brief Decides which passes a frame wires from the topology settings and the resolved sky.
    ///
    /// Final is the full deferred chain; a debug mode terminates after the g-buffer with one blit.
    /// Debug arms force-wire their producing battery pass so the visualized target exists
    /// regardless of the corresponding Settings toggle.
    ///
    /// Pure: no device, no allocation, no I/O, and no dependence on anything but its arguments —
    /// the side effects the graph build performs around it (the auto-exposure enable edge, the
    /// skylight notification) are the caller's, precisely so this stays a function of its inputs.
    /// @param settings The topology and sizing knobs the frame renders under.
    /// @param sky      The resolved sky facts for the frame.
    /// @return The decided topology.
    [[nodiscard]] FrameTopology ResolveFrameTopology(const SceneRendererSettings& settings,
                                                     const SkyTopologyInput& sky);

    /// @brief Whether a temporal resolve reconstructs the post-resolve allocation directly.
    ///
    /// Temporal upscaling promotes at the temporal anchor, so every pass between that anchor and
    /// the HDR tail sees allocation-resolution scene colour beside render-resolution depth. The SSR
    /// composite and a composited depth-of-field chain sit there and read both through one extent,
    /// so a frame wiring either resolves at the render allocation instead and the spatial promotion
    /// carries the tail up — a spatial reconstruction rather than a temporal one, with the render
    /// scale still buying exactly what it buys in every other mode.
    ///
    /// A function of the settings alone (no sky input), so the renderer can size its scene-colour
    /// allocation from it before the frame topology is resolved.
    /// @param settings The topology and sizing knobs the frame renders under.
    /// @return True when the temporal resolve is the promotion.
    [[nodiscard]] bool ResolveTemporalUpscalePromotes(const SceneRendererSettings& settings);

    /// @brief Whether the frame wires the bloom pyramid sweep (and so the bloom mask beside it).
    ///
    /// A function of the settings alone, so the renderer can decide whether the mask needs its own
    /// allocation before the frame topology is resolved — the same reason
    /// ResolveTemporalUpscalePromotes stands apart from the resolve.
    /// @param settings The topology and sizing knobs the frame renders under.
    /// @return True for the Final arm with bloom on, and for the Bloom debug arm.
    [[nodiscard]] bool ResolveBloomActive(const SceneRendererSettings& settings);
}
