// Particle Update: one thread per live particle, dispatched indirectly from the count Prepare
// wrote. A particle whose instance slot changed generation, or whose age has reached its lifetime,
// returns its slot to the dead list. A survivor joins the other half of the live list and, unless
// its instance is hidden this frame, writes its draw record: the closed-form position and velocity
// at its age, its over-life curves read from the emitter's tables, its flipbook frame, and — for a
// six-way flipbook — the scene's light gathered once at its centre, from the lights whose forward
// tile masks the particle's drawn extent covers (ForwardSphereLightMask). Its draw index then joins
// the additive list, or the alpha keys with its view distance for the sort.
//
// A survivor that may draw alpha — its emitter's record is alpha, or its instance is hidden this
// frame, whose records are not read — is counted for the CPU's bound on the next frames' sort.
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(3, 0)]] ByteAddressBuffer g_Buffers[];

#include "particle_compute.slang"
#include "particle_math.slang"
#include "Veng/sprite_shading.slang"
#include "Veng/light_tiles.slang"

// One tap of an emitter's over-life tables at normalised age u: the first table in the result's
// xyzw, the tint in tint.
float4 SampleLut(GpuParticleEmitter emitter, float u, out float3 tint)
{
    float x = saturate(u) * float(ParticleLutSamples - 1u);
    uint i0 = min(uint(x), ParticleLutSamples - 1u);
    uint i1 = min(i0 + 1u, ParticleLutSamples - 1u);
    float f = x - float(i0);
    tint = lerp(emitter.Lut[ParticleLutSamples + i0].rgb, emitter.Lut[ParticleLutSamples + i1].rgb,
                f);
    return lerp(emitter.Lut[i0], emitter.Lut[i1], f);
}

// A frame's extent as a fraction of the full frame, 1 for a flipbook recording none.
float FrameScaleOf(GpuParticleEmitter emitter, uint frame)
{
    if (emitter.Counts.w == ParticleNoFrameScales)
    {
        return 1.0;
    }
    return g_FrameScales[emitter.Counts.w + min(frame, emitter.Grid.z - 1u)];
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    uint cur = g_PC.Cur;
    if (id.x >= g_Counters.Load(LiveCounter(cur)))
    {
        return;
    }
    uint slot = g_LiveList[cur * g_PC.Capacity + id.x];
    GpuParticle particle = g_Particles[slot];
    GpuParticleInstance instance = g_Instances[particle.Instance & 0xFFFFu];
    float age = instance.Time - particle.SpawnTime;
    bool dead = (instance.Generation & 0xFFFFu) != (particle.Instance >> 16) ||
                age >= particle.Lifetime;
    if (dead)
    {
        uint free;
        g_Counters.InterlockedAdd(ParticleCounterDead, 1u, free);
        g_DeadList[free] = slot;
        return;
    }

    uint o;
    g_Counters.InterlockedAdd(LiveCounter(1u - cur), 1u, o);
    g_LiveList[(1u - cur) * g_PC.Capacity + o] = slot;
    if ((instance.Generation & ParticleInstanceHidden) != 0u)
    {
        g_Counters.InterlockedAdd(ParticleCounterAlphaLive, 1u);
        return;
    }

    uint emitterIndex = instance.EmitterBase + particle.Emitter;
    GpuParticleEmitter emitter = g_Emitters[emitterIndex];
    if (emitter.Mode.y == 0u)
    {
        g_Counters.InterlockedAdd(ParticleCounterAlphaLive, 1u);
    }
    uint key = particle.Random;
    float u = particle.Lifetime > 0.0 ? age / particle.Lifetime : 1.0;
    float3 tint;
    float4 curves = SampleLut(emitter, u, tint);
    float opacity = curves.y;
    if (opacity <= 0.0)
    {
        return;
    }

    float3 acceleration = emitter.Accel.xyz * instance.Scale;
    float drag = emitter.Accel.w;
    float3 offset = ParticleOffsetAt(particle.Offset, particle.Velocity, acceleration, drag, age);
    float3 velocity = ParticleVelocityAt(particle.Velocity, acceleration, drag, age);
    float3 centre = instance.AnchorEye + offset;

    float width = lerp(emitter.LifetimeSize.z, emitter.LifetimeSize.w,
                       ParticleRandom(key, ParticleDrawSize)) *
                  instance.Scale * curves.x;
    float rotation = lerp(emitter.Rotation.x, emitter.Rotation.y,
                          ParticleRandom(key, ParticleDrawRotation)) +
                     particle.Spin * age;
    float intensity = curves.z;

    GpuParticleDraw draw;
    draw.CentreWidth = float4(centre, width);
    draw.TailHeight = float4(0.0, 0.0, 0.0, width);
    draw.Rotation = rotation;
    draw.Emitter = emitterIndex;
    draw.FramePosition = 0.0;
    draw.TemperatureOffset = curves.w;
    draw.FrameScale = 1.0;
    draw.FrameScaleNext = 1.0;
    draw.SoftDistance = 0.0;
    SpriteLightGather gather = SpriteLightGatherNone();

    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);
    if (emitter.Mode.x == ParticleRenderStreak)
    {
        tint *= emitter.Streak.rgb;
        draw.TailHeight = float4(-velocity * emitter.Streak.w, width);
    }
    else
    {
        float height = width * emitter.Playback.w;
        draw.TailHeight.w = height;

        // The flipbook frame, as FlipbookFramePositionAt: stretched over the life or at a fixed
        // rate, offset by a random start frame, wrapped when looping and held on the last frame
        // otherwise.
        uint frames = emitter.Grid.z;
        float rate = emitter.Playback.x > 0.0 ? emitter.Playback.x
                                              : float(frames) / max(particle.Lifetime, 1e-6);
        float position = age * rate;
        if ((emitter.Mode.w & ParticleFlagRandomStart) != 0u)
        {
            position += floor(ParticleRandom(key, ParticleDrawStartFrame) * float(frames));
        }
        uint frame;
        float fraction = 0.0;
        if ((emitter.Mode.w & ParticleFlagLoop) != 0u)
        {
            float whole = floor(position);
            frame = uint(whole) % frames;
            fraction = position - whole;
        }
        else if (position >= float(frames - 1u))
        {
            frame = frames - 1u;
        }
        else
        {
            float whole = floor(position);
            frame = uint(whole);
            fraction = position - whole;
        }
        if (ParticleFrameBlendOf(emitter) == ParticleBlendStep)
        {
            fraction = 0.0;
        }
        draw.FramePosition = float(frame) + min(fraction, 0.99999);
        draw.FrameScale = FrameScaleOf(emitter, frame);
        draw.FrameScaleNext = FrameScaleOf(emitter, ParticleNextFrame(emitter, frame));

        float soft = emitter.Noise.z;
        draw.SoftDistance = soft < 0.0 ? 0.5 * width : soft * instance.Scale;

        if ((emitter.Mode.w & ParticleFlagSixWay) != 0u)
        {
            // The particle's frame is the camera's rolled right and up, Z = X × Y toward the
            // viewer, centred on the frame centre, as a sprite's.
            float s = sin(rotation);
            float c = cos(rotation);
            float3 right = float3(view.View[0][0], view.View[0][1], view.View[0][2]);
            float3 up = float3(view.View[1][0], view.View[1][1], view.View[1][2]);
            float3 axisX = c * right + s * up;
            float3 axisY = -s * right + c * up;
            float3 axisZ = cross(axisX, axisY);
            float extent = lerp(draw.FrameScale, draw.FrameScaleNext, fraction);
            float2 toCentre = (0.5 - emitter.Frame.xy) * float2(width, height);
            float3 world = centre + view.CameraPosition.xyz + toCentre.x * axisX +
                           toCentre.y * axisY;
            float radius = 0.5 * max(width, height) * extent;
            // The drawn quad lies within half its diagonal at the larger frame extent of world.
            float reach = 0.5 * length(float2(width, height)) *
                          max(draw.FrameScale, draw.FrameScaleNext);
            LightMask lights = ForwardSphereLightMask(view, world, reach);
            gather = EvaluateSpriteLighting(view, world, axisX, axisY, axisZ, radius, lights);
        }
    }

    float3 colour = tint * intensity;
    draw.TintOpacity = uint2(f32tof16(colour.r) | (f32tof16(colour.g) << 16),
                             f32tof16(colour.b) | (f32tof16(opacity) << 16));
    PackParticleLight(gather, draw.Light0, draw.Light1, draw.Light2);
    g_Draws[o] = draw;

    if (emitter.Mode.y != 0u)
    {
        uint index;
        g_Counters.InterlockedAdd(ParticleCounterAdditive, 1u, index);
        g_AdditiveList[index] = o;
    }
    else
    {
        uint index;
        g_Counters.InterlockedAdd(ParticleCounterAlpha, 1u, index);
        // The complement of a positive float's bits orders the farthest first, and never reaches
        // the sentinel 0xFFFFFFFF.
        float distance = max(length(centre), 1e-30);
        g_AlphaKeys[index] = uint2(~asuint(distance), o);
    }
}
