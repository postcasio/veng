# The particle-effect source

A particle-effect source is a JSON file describing layered emitters and light flashes played as one
effect. veng's `ParticleEffectImporter` cooks it as it stands: a pack entry of type
`ParticleEffect` names the file (`*.particles.json`) as its `source`. The runtime asset is
`ParticleEffect` (`Veng/Asset/ParticleEffect.h`); a `ParticleEffectInstance` plays one, and the
renderer simulates and draws its particles on the GPU.

An authoring tool that writes effects targets this document.

```json
{
  "type": "particle-effect",
  "version": 1,
  "emitters": [{
    "name": "core",
    "render": "billboard",
    "flipbook": "0x…",
    "blend": "asset",
    "spawn": { "burst": { "count": 48, "time": 0.0 },
               "rate":  { "perSecond": 0, "start": 0.0, "duration": 0.0 } },
    "lifetime": [1.0, 1.0],
    "shape": { "kind": "point", "radius": 0.0 },
    "velocity": { "speed": [0, 0], "cone": { "axis": [0, 1, 0], "angle": 180 },
                  "speedByRadius": 0.0, "noise": { "amplitude": 0.0, "frequency": 1.0 } },
    "acceleration": [0, 0, 0],
    "drag": 0.0,
    "size": [1.0, 1.0],
    "rotation": [0, 0],
    "spin": [0, 0],
    "playback": { "mode": "lifetime", "fps": 0, "randomStart": false },
    "softDistance": null,
    "emission": { "ramp": [[0.25, [0, 0, 0]], [1.0, [30, 12, 4]]], "ignition": 0.25 },
    "streak": { "color": [1, 1, 1], "lengthScale": 0.05 },
    "overLife": { "size": [[0, 1], [1, 1]], "opacity": [[0, 1], [1, 1]],
                  "tint": [[0, [1, 1, 1]]], "intensity": [[0, 1]], "temperature": [[0, 0]] }
  }],
  "flashes": [{ "time": 0.0, "duration": 0.2, "color": [1, 0.8, 0.6], "intensity": 1.0e6,
                "range": 200, "radius": 1, "curve": [[0, 1], [1, 0]] }]
}
```

Every field but `type`, `version` and `emitters` is optional and takes the default shown. Lengths
are world units and times seconds, at an instance `Scale` of 1: an instance's `Scale` multiplies
every length below (radii, sizes, speeds, accelerations, streak lengths, flash ranges and radii) and
leaves every time alone. Every key the cook does not name here is ignored.

## The effect

| Field | Type | Meaning |
|---|---|---|
| `type` | string | Always `"particle-effect"`. |
| `version` | int | Always `1`. |
| `emitters` | array | The emitters, at most 16. |
| `flashes` | array, optional | The light flashes, at most 4. |

## An emitter

| Field | Type | Default | Meaning |
|---|---|---|---|
| `name` | string | `""` | A label for diagnostics. |
| `render` | `"billboard"` \| `"streak"` | `"billboard"` | A billboard draws a flipbook, schema 1 or 2. A streak is a quad stretched along the particle's velocity in one HDR colour, for sparks. |
| `flipbook` | hex id string, or an array of 1 to 8 | — | A billboard's `Flipbook`, in this pack or a reference pack, or a list of them: each particle plays one, picked uniformly from its own random stream, so a burst mixes them. Required for a billboard; refused on a streak. |
| `blend` | `"asset"` \| `"alpha"` \| `"additive"` | `"asset"` | `asset` takes the flipbook's recommendation, so a streak must choose `alpha` or `additive`. Alpha particles sort back to front; additive ones do not. |
| `spawn.burst.count` | int ≥ 0 | 0 | Particles spawned at once. |
| `spawn.burst.time` | number ≥ 0 | 0 | When the burst spawns. |
| `spawn.rate.perSecond` | number ≥ 0 | 0 | The rate. Rate particle `j` spawns at `start + j / perSecond`. |
| `spawn.rate.start` | number ≥ 0 | 0 | When the rate starts. |
| `spawn.rate.duration` | number ≥ 0 | 0 | The rate spawns `ceil(duration · perSecond)` particles; 0 runs it until the instance stops emitting. |
| `spawn.fed` | bool | false | The emitter's particles are placed by the instance's owner (`ParticleFeed`), each with its own place, time and velocity, rather than scheduled; it takes no burst and no rate. The shape's offset and the cone's speed are added to each fed place and velocity. An emitter must burst, rate or be fed. |
| `lifetime` | `[min, max]` ≥ 0 | `[1, 1]` | Seconds each particle lives. |
| `shape.kind` | `"point"` \| `"sphere"` \| `"shell"` | `"point"` | Where particles spawn: the origin, uniformly through a ball, or on its surface. |
| `shape.radius` | number ≥ 0 | 0 | The shape's radius `R`. |
| `velocity.speed` | `[min, max]` | `[0, 0]` | The radial speed. |
| `velocity.cone.axis` | `[x, y, z]`, non-zero | `[0, 1, 0]` | The axis directions gather about, in the effect's own frame, which turns with the instance. |
| `velocity.cone.angle` | degrees in (0, 180] | 180 | The cone's half-angle; 180 is the whole sphere. |
| `velocity.speedByRadius` | number | 0 | The exponent `e` below; 1 is the self-similar expansion. |
| `velocity.noise.amplitude` | number ≥ 0 | 0 | The relative amplitude of a smooth speed noise over spawn directions. |
| `velocity.noise.frequency` | number ≥ 0 | 1 | Its spatial frequency. |
| `acceleration` | `[x, y, z]` | `[0, 0, 0]` | Constant acceleration in the stable frame, units per second squared. |
| `drag` | number ≥ 0 | 0 | Linear drag rate `k`, per second. |
| `size` | `[min, max]` ≥ 0 | `[1, 1]` | The quad's width; a billboard's height follows its flipbook's frame aspect. For a flipbook carrying `frameScale` it is the width of a frame at scale 1 — each frame draws at `size` times its scale. |
| `rotation` | `[min, max]` degrees | `[0, 0]` | The initial screen-plane roll. |
| `spin` | `[min, max]` degrees per second | `[0, 0]` | The roll rate. |
| `playback.mode` | `"lifetime"` \| `"fps"` | `"lifetime"` | `lifetime` stretches the whole flipbook over the particle's life; `fps` plays at `playback.fps`, wrapping when the flipbook loops and holding its last frame when it does not. |
| `playback.fps` | number ≥ 0 | 0 | The `fps` mode's rate; 0 takes the flipbook's own. |
| `playback.randomStart` | bool | false | Offsets the playback by a random whole frame. |
| `softDistance` | number ≥ 0 or null | null | The depth over which a billboard fades into opaque geometry. Null takes the default: half the quad's width for a six-way flipbook, off otherwise. Always off for a streak. |
| `emission.ramp` | `[[t, [r, g, b]], …]` | the flipbook's | Replaces a six-way flipbook's emission ramp: linear HDR colours at normalised temperatures, at most 8 keys. Billboard only. |
| `emission.ignition` | number | 0 | The temperature below which nothing emits, when `emission` is given. |
| `streak.color` | `[r, g, b]` | `[1, 1, 1]` | A streak's linear HDR colour. |
| `streak.lengthScale` | number ≥ 0 | 0.05 | Seconds of velocity a streak's length spans: its tail sits `velocity · lengthScale` behind its head. |
| `overLife.size` | scalar curve | constant 1 | Multiplies the size over normalised age. |
| `overLife.opacity` | scalar curve | constant 1 | Multiplies the opacity over normalised age. |
| `overLife.tint` | colour curve | constant white | The RGB tint over normalised age. |
| `overLife.intensity` | scalar curve | constant 1 | Multiplies the tint over normalised age. |
| `overLife.temperature` | scalar curve | constant 0 | Added to a six-way flipbook's normalised temperature over normalised age, so a cooling particle reads cooler. |

**How a particle moves.** Each particle draws one direction `d`, uniform over the cone, and a radius
`r` — 0 for a point, `R · ∛u` through a sphere, `R` on a shell. It is placed at `d · r` and moves
along `d` at

```
lerp(speed) · (r / R)^speedByRadius · max(0, 1 + amplitude · N(d · frequency))
```

where `N` is a smooth value noise in [−1, 1], seeded per instance, and the radius factor is 1 for a
point. Its path is the closed form of constant acceleration `a` and linear drag `k`:
`p₀ + v₀·t + ½·a·t²` with no drag, `p₀ + v₀·F + a·(t − F)/k` with `F = (1 − e^{−kt})/k` with drag.
With neither, which is the default, the path is exactly `p₀ + v₀·t`: a particle keeps the velocity
it was given. An instance's inherited velocity is added to every `v₀`, unscaled.

**Curves.** A scalar curve is `[[t, value], …]`, a colour curve `[[t, [r, g, b]], …]`. Keys ascend
strictly in `t`, every `t` lies in [0, 1], and a curve holds at most 16 keys. The curve is
piecewise linear between keys and constant past the end keys; one key is constant. The runtime bakes
each into 32 samples, the first and last exactly the end keys.

## A flash

A flash is a point light over a window of the effect. While any flash is live, the playing entity
carries one point `Light`: its colour the intensity-weighted mean of the live flashes' colours, its
intensity their summed lumens, its range and radius the largest among them.

| Field | Type | Default | Meaning |
|---|---|---|---|
| `time` | number ≥ 0 | 0 | When the flash begins. |
| `duration` | number ≥ 0 | 0.2 | How long it lasts; it is live over `[time, time + duration)`. |
| `color` | `[r, g, b]` | `[1, 1, 1]` | The light's linear colour. |
| `intensity` | number ≥ 0 | 0 | Peak lumens, as `Light::Intensity` for a point light. |
| `range` | number ≥ 0 | 0 | As `Light::Range`. |
| `radius` | number ≥ 0 | 0 | As `Light::Radius`. |
| `curve` | scalar curve | constant 1 | Multiplies the intensity over the flash's normalised life. |

## Refusals

Each is a located cook error naming the file and the field (`emitters[1].velocity.cone.angle`):

- a `type` other than `"particle-effect"`, or a `version` other than 1;
- an unknown `render`, `blend`, `shape.kind` or `playback.mode`;
- a billboard whose `flipbook` is missing, malformed, or resolves to anything but a `Flipbook`, or
  a list of them that is empty or longer than 8 (an entry is named by its index,
  `emitters[0].flipbook[1]`);
- a streak naming a `flipbook`, an `emission`, or `blend: "asset"`;
- a `[min, max]` with `min > max`; a negative lifetime, size, radius, drag, rate or time;
- a cone axis of zero, or a cone angle outside (0, 180];
- a curve key out of order or with `t` outside [0, 1], or a curve past its key limit;
- an emitter that spawns nothing (no burst count and no rate);
- more than 16 emitters or 4 flashes.

An effect whose particles can number more than a renderer's default capacity (65,536) at once —
the sum over its emitters of the burst count plus `ceil(rate · lifetime.max)` — cooks with a
warning: past the capacity a renderer drops spawns and counts them.

## The cooked blob

`CookedParticleEffectHeader`, then `CookedParticleEmitter[EmitterCount]`, then
`CookedParticleFlash[FlashCount]`, then the `u64` flipbook-id pool each billboard's
`{FlipbookOffset, FlipbookCount}` run indexes, then the `f32` key pool every curve indexes
(`Veng/Asset/CookedBlobs.h`). The format version is `CookedParticleEffectVersion`; a mismatch is refused at load.
