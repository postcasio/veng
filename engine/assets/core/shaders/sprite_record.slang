// The sprite pass's per-frame records, shared by its vertex and fragment stages: one GpuSprite per
// sprite (set 3 binding 0) and the emission ramps six-way sprites index (set 3 binding 1), both
// mirroring Passes/SpriteScenePass.h byte for byte, and the push block.

#pragma once

#include "Veng/sprite_shading_core.slang"

// GpuSprite::Flags bits, matching the renderer's SpriteFlag constants.
static const uint SpriteFlagSixWay = 1u << 0;
static const uint SpriteFlagBlendShift = 1u;
static const uint SpriteFlagBlendMask = 3u;
static const uint SpriteFlagSqrtTemperature = 1u << 3;
static const uint SpriteFlagTemperatureRed = 1u << 4;
static const uint SpriteFlagEmits = 1u << 5;

// The frame blend a sprite resolved to (bits 1-2 of Flags).
static const uint SpriteBlendStep = 0u;
static const uint SpriteBlendCrossfade = 1u;
static const uint SpriteBlendMotion = 2u;

// One sprite record (std430, 160 bytes).
struct GpuSprite
{
    float4 PositionWidth;   // xyz the world anchor, w the full frame's quad width
    float4 Color;           // rgb linear HDR tint (the lit term's, for six-way), a opacity
    float4 UvRect;          // frame N: u0 v0 u1 v1, (u0, v0) its top-left, inset half a texel
    float4 UvRectNext;      // frame N+1, inset the same way
    float2 Anchor;          // the anchor within the full frame, x right, y up
    float Height;           // the full frame's quad height
    float Rotation;         // screen-plane roll, radians counter-clockwise
    float2 TileSize;        // one frame's extent in atlas UV
    float FrameScale;       // frame N's extent as a fraction of the full frame
    float FrameScaleNext;   // frame N+1's
    uint Texture;           // the colour sheet, or lightA: bindless index
    uint Sampler;           // the sampler every texture of the sprite is read through
    uint AlphaMode;         // the colour sheet's FlipbookAlpha
    uint Flags;             // SpriteFlag* bits
    uint LightB;            // the -X/-Y/+Z lightmap's bindless index (six-way)
    uint Motion;            // the motion sheet's bindless index (motion blend)
    uint Temperature;       // the texture holding the temperature (six-way)
    uint Ramp;              // index into the emission-ramp table (six-way)
    float FrameFraction;    // how far playback is from frame N toward N+1
    float MotionScale;      // the motion sheet's decode scale
    float SoftDistance;     // world distance the soft edge fades over; 0 is off
    float EmissionIntensity; // multiplies the ramp
    float TemperatureOffset; // added to the decoded temperature
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

[[vk::binding(0, 3)]] StructuredBuffer<GpuSprite> g_Sprites;
[[vk::binding(1, 3)]] StructuredBuffer<SpriteEmissionRamp> g_SpriteRamps;

struct SpritePushConstants
{
    uint ViewConstantsIndex;
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

[[vk::push_constant]] ConstantBuffer<SpritePushConstants> g_PC;

// The frame blend a sprite resolved to.
uint SpriteFrameBlendOf(GpuSprite sprite)
{
    return (sprite.Flags >> SpriteFlagBlendShift) & SpriteFlagBlendMask;
}

// The two frames a sprite blends between.
SpriteFramePair SpriteFramePairOf(GpuSprite sprite)
{
    SpriteFramePair pair;
    pair.RectN = sprite.UvRect;
    pair.RectN1 = sprite.UvRectNext;
    pair.TileSize = sprite.TileSize;
    pair.ScaleN = sprite.FrameScale;
    pair.ScaleN1 = sprite.FrameScaleNext;
    pair.Fraction = SpriteFrameBlendOf(sprite) == SpriteBlendStep ? 0.0 : sprite.FrameFraction;
    return pair;
}
