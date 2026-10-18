// The particle chain's records, mirroring the renderer's ParticleGather.h byte for byte (std430):
// the persistent particle state, the per-frame instance, emitter and spawn-range tables, and the
// per-particle draw record the update writes and the draw reads. Bindings are declared by each
// stage itself (the compute stages read-write, the graphics stages read-only), at the set-3 slots
// named here.

#pragma once

#include "Veng/sprite_shading_core.slang"

// The set-3 binding of each buffer, shared by every particle pipeline.
#define PARTICLE_BINDING_PARTICLES 0
#define PARTICLE_BINDING_DEAD_LIST 1
#define PARTICLE_BINDING_LIVE_LIST 2
#define PARTICLE_BINDING_ADDITIVE_LIST 3
#define PARTICLE_BINDING_ALPHA_KEYS 4
#define PARTICLE_BINDING_DRAWS 5
#define PARTICLE_BINDING_COUNTERS 6
#define PARTICLE_BINDING_ARGS 7
#define PARTICLE_BINDING_INSTANCES 8
#define PARTICLE_BINDING_EMITTERS 9
#define PARTICLE_BINDING_SPAWN_RANGES 10
#define PARTICLE_BINDING_STATS 11
#define PARTICLE_BINDING_FRAME_SCALES 12

// Byte offsets of the counters buffer's words.
static const uint ParticleCounterDead = 0;
static const uint ParticleCounterLive0 = 4;
static const uint ParticleCounterAlpha = 12;
static const uint ParticleCounterAdditive = 16;
static const uint ParticleCounterSpawned = 20;
static const uint ParticleCounterDropped = 24;

// Byte offsets of the argument buffer's records.
static const uint ParticleArgsUpdate = 0;
static const uint ParticleArgsAlphaDraw = 16;
static const uint ParticleArgsAdditiveDraw = 32;

// GpuParticleEmitter::Mode.w bits.
static const uint ParticleFlagSixWay = 1u << 0;
static const uint ParticleFlagBlendShift = 1u;
static const uint ParticleFlagBlendMask = 3u;
static const uint ParticleFlagSqrtTemperature = 1u << 3;
static const uint ParticleFlagTemperatureRed = 1u << 4;
static const uint ParticleFlagEmits = 1u << 5;
static const uint ParticleFlagLoop = 1u << 6;
static const uint ParticleFlagRandomStart = 1u << 7;

// The frame blends, as FlipbookFrameBlend.
static const uint ParticleBlendStep = 0u;
static const uint ParticleBlendCrossfade = 1u;
static const uint ParticleBlendMotion = 2u;

// GpuParticleEmitter::Mode.x.
static const uint ParticleRenderBillboard = 0u;
static const uint ParticleRenderStreak = 1u;

// GpuParticleInstance::Generation's bit marking an instance whose particles live but do not draw.
static const uint ParticleInstanceHidden = 0x80000000u;
// GpuParticleEmitter::Counts.y for a rate that runs until the instance stops emitting.
static const uint ParticleUnboundedRate = 0xFFFFFFFFu;
// GpuParticleEmitter::Counts.w for an emitter whose flipbook records no per-frame scales.
static const uint ParticleNoFrameScales = 0xFFFFFFFFu;
// The samples each over-life table holds.
static const uint ParticleLutSamples = 32;

// One particle's spawn state (48 bytes): written once, at spawn.
struct GpuParticle
{
    float3 Offset;   // from the instance's anchor at spawn, world units
    float SpawnTime; // seconds of the instance's playback
    float3 Velocity; // at spawn, world units per second
    float Lifetime;  // seconds
    uint Instance;   // instance slot in the low 16 bits, its generation in the high 16
    uint Emitter;    // the effect-relative record of the emitter's flipbook this particle plays
    uint Random;     // the particle's key, every further random draw's root
    float Spin;      // roll rate, radians per second
};

// One playing instance (64 bytes).
struct GpuParticleInstance
{
    float3 AnchorEye;       // the anchor's position relative to the eye, render-space axes
    float Time;             // seconds of playback
    float4 Orientation;     // the effect's frame, a quaternion (x, y, z, w)
    float3 InheritVelocity; // added to every v0, world units per second
    uint Seed;
    uint Generation;        // the slot's generation in the low 16 bits; ParticleInstanceHidden
    uint EmitterBase;       // the effect's first emitter in the emitter table
    uint EmitterCount;
    float Scale;            // multiplies every length the effect authors
};

// One emitter's record for one of its flipbooks, drawn this frame (1424 bytes).
struct GpuParticleEmitter
{
    uint4 Mode;            // x render, y 1 when additive, z shape, w ParticleFlag bits
    uint4 Textures;        // x the colour sheet or lightA, y lightB, z motion, w temperature
    uint4 Grid;            // x columns, y rows, z frame count, w the sampler
    uint4 Counts;          // x burst count, y rate total, z alpha mode, w frame-scale base
    uint4 Variants;        // x flipbooks the emitter picks from, y the effect-relative record of
                           // its second (the rest follow); read from its first record
    float4 Spawn;          // x burst time, y rate start, z rate per second
    float4 LifetimeSize;   // xy lifetime range, zw size range
    float4 Speed;          // xy speed range, z speed-by-radius exponent, w shape radius
    float4 Cone;           // xyz unit axis in the effect's frame, w cos(half-angle)
    float4 Noise;          // x amplitude, y frequency, z soft distance (<0 half the width),
                           // w emission intensity
    float4 Accel;          // xyz acceleration, w drag
    float4 Rotation;       // xy initial roll range, zw spin range, radians
    float4 Playback;       // x frames per second (0 stretches over the life), y motion scale,
                           // w height over width
    float4 Frame;          // xy anchor fraction, zw one tile's extent in UV
    float4 Inset;          // xy half a texel in UV
    float4 Streak;         // rgb linear HDR colour, w seconds of velocity the length spans
    SpriteEmissionRamp Ramp;
    float4 Lut[2 * ParticleLutSamples]; // (size, opacity, intensity, temperature), then (tint, 0)
};

// One run of particle indices to spawn this frame (16 bytes).
struct GpuParticleSpawnRange
{
    uint InstanceEmitter; // instance slot low 16 bits, effect-local emitter index high 16
    uint FirstIndex;
    uint Count;
    uint Prefix;          // the spawn threads every earlier range takes
};

// One live particle as the draw reads it (112 bytes).
struct GpuParticleDraw
{
    float4 CentreWidth;      // xyz eye-relative centre (a streak's head), w width
    float4 TailHeight;       // xyz a streak's tail relative to its head, w the quad's height
    uint2 TintOpacity;       // half4: rgb tint times intensity, a opacity
    float Rotation;          // screen-plane roll, radians
    uint Emitter;            // emitter-table index
    float FramePosition;     // frame N plus the fraction toward N + 1
    float TemperatureOffset; // added to the decoded six-way temperature
    float FrameScale;        // frame N's extent as a fraction of the full frame
    float FrameScaleNext;    // frame N + 1's
    uint4 Light0;            // the gathered light: six axis terms and the ambient, as halves
    uint4 Light1;
    uint3 Light2;
    float SoftDistance;      // world depth the particle fades over into geometry; 0 off
};

// Packs a light gather into the draw record's halves.
void PackParticleLight(SpriteLightGather g, out uint4 l0, out uint4 l1, out uint3 l2)
{
    l0 = uint4(f32tof16(g.In[0].x) | (f32tof16(g.In[0].y) << 16),
               f32tof16(g.In[0].z) | (f32tof16(g.In[1].x) << 16),
               f32tof16(g.In[1].y) | (f32tof16(g.In[1].z) << 16),
               f32tof16(g.In[2].x) | (f32tof16(g.In[2].y) << 16));
    l1 = uint4(f32tof16(g.In[2].z) | (f32tof16(g.In[3].x) << 16),
               f32tof16(g.In[3].y) | (f32tof16(g.In[3].z) << 16),
               f32tof16(g.In[4].x) | (f32tof16(g.In[4].y) << 16),
               f32tof16(g.In[4].z) | (f32tof16(g.In[5].x) << 16));
    l2 = uint3(f32tof16(g.In[5].y) | (f32tof16(g.In[5].z) << 16),
               f32tof16(g.Ambient.x) | (f32tof16(g.Ambient.y) << 16),
               f32tof16(g.Ambient.z));
}

float2 UnpackHalf2(uint v)
{
    return float2(f16tof32(v & 0xFFFFu), f16tof32(v >> 16));
}

// Unpacks a draw record's light gather.
SpriteLightGather UnpackParticleLight(uint4 l0, uint4 l1, uint3 l2)
{
    float2 a = UnpackHalf2(l0.x);
    float2 b = UnpackHalf2(l0.y);
    float2 c = UnpackHalf2(l0.z);
    float2 d = UnpackHalf2(l0.w);
    float2 e = UnpackHalf2(l1.x);
    float2 f = UnpackHalf2(l1.y);
    float2 g = UnpackHalf2(l1.z);
    float2 h = UnpackHalf2(l1.w);
    float2 i = UnpackHalf2(l2.x);
    float2 j = UnpackHalf2(l2.y);
    float2 k = UnpackHalf2(l2.z);
    SpriteLightGather gather;
    gather.In[0] = float3(a.x, a.y, b.x);
    gather.In[1] = float3(b.y, c.x, c.y);
    gather.In[2] = float3(d.x, d.y, e.x);
    gather.In[3] = float3(e.y, f.x, f.y);
    gather.In[4] = float3(g.x, g.y, h.x);
    gather.In[5] = float3(h.y, i.x, i.y);
    gather.Ambient = float3(j.x, j.y, k.x);
    return gather;
}

// The frame blend an emitter resolved to.
uint ParticleFrameBlendOf(GpuParticleEmitter emitter)
{
    return (emitter.Mode.w >> ParticleFlagBlendShift) & ParticleFlagBlendMask;
}

// A frame's atlas rectangle, (u0, v0, u1, v1) inset half a texel, as Flipbook::GetFrameRect.
float4 ParticleFrameRect(GpuParticleEmitter emitter, uint frame)
{
    uint clamped = min(frame, emitter.Grid.z - 1u);
    float2 origin = float2(float(clamped % emitter.Grid.x), float(clamped / emitter.Grid.x)) *
                    emitter.Frame.zw;
    return float4(origin + emitter.Inset.xy, origin + emitter.Frame.zw - emitter.Inset.xy);
}

// The frame after frame: wrapped for a looping flipbook, held on a one-shot's last.
uint ParticleNextFrame(GpuParticleEmitter emitter, uint frame)
{
    uint count = emitter.Grid.z;
    if ((emitter.Mode.w & ParticleFlagLoop) != 0u)
    {
        return (frame + 1u) % count;
    }
    return min(frame + 1u, count - 1u);
}

// The two frames a draw record blends between.
SpriteFramePair ParticleFramePair(GpuParticleEmitter emitter, GpuParticleDraw draw)
{
    uint frame = uint(max(draw.FramePosition, 0.0));
    float fraction = draw.FramePosition - float(frame);
    bool blends = ParticleFrameBlendOf(emitter) != ParticleBlendStep;
    uint next = blends ? ParticleNextFrame(emitter, frame) : frame;
    SpriteFramePair pair;
    pair.RectN = ParticleFrameRect(emitter, frame);
    pair.RectN1 = ParticleFrameRect(emitter, next);
    pair.TileSize = emitter.Frame.zw;
    pair.ScaleN = draw.FrameScale;
    pair.ScaleN1 = draw.FrameScaleNext;
    pair.Fraction = blends ? fraction : 0.0;
    return pair;
}
