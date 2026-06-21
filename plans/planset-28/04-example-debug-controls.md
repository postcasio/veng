# Plan 04 — example debug UI: exposure, bloom, and the settings audit

**Goal:** complete the hello-triangle "Scene" debug window's coverage of
`SceneRendererSettings` — add the tunable controls it lacks (most visibly **exposure** and
**bloom strength**), including this planset's new bloom knobs. Example-only, one file
(`examples/hello-triangle/main.cpp`); no engine change. Depends on Plan 00 (`Exposure` on
`SceneView`) and Plans 01–02 (the new `BloomRadius` / `Kernel` knobs must exist).

## The gap

The "Scene" window already drives most of `SceneRendererSettings` — `DebugView` (the View
combo), SSAO, shadows + cascade count/resolution/split-lambda, punctual shadows +
resolution, frustum cull, cull mode, GPU occlusion. An audit across the renderer's tunable
surface (`SceneRendererSettings` **and** the per-frame `SceneView` knobs the example sets in
its `view{}` initializer) leaves exactly these **unexposed**, all pre-existing or
this-planset engine knobs:

| Knob | Home | Kind | What | Why unexposed |
|---|---|---|---|---|
| `Exposure` | `SceneView` (moved by Plan 00) | per-frame, no recompile | tonemap exposure | pre-existing, never wired |
| `Bloom` | `SceneRendererSettings` | topology (`ReconfigureScene`) | bloom on/off | pre-existing, never wired |
| `BloomThreshold` | `SceneView` | per-frame | bright-pass knee | pre-existing, never wired |
| `BloomIntensity` | `SceneView` | per-frame | bloom strength | pre-existing, never wired |
| `BloomRadius` | `SceneView` | per-frame | upsample spread | new this planset (Plan 01) |
| `Kernel` | `SceneRendererSettings` | topology (`ReconfigureScene`) | COD/Kawase filter | new this planset (Plan 02) |

The per-frame knobs (`Exposure`/`BloomThreshold`/`BloomIntensity`/`BloomRadius`) live on the
`SceneView` the example **builds fresh every `OnRender`**
([main.cpp:186](../../examples/hello-triangle/main.cpp:186)) — today with hardcoded
`BloomThreshold = 0.5f` / `BloomIntensity = 1.5f`. To make them UI-tunable the example holds
them as **app-side members** and feeds those members into the per-frame `view{}`; the UI
edits the members, the renderer reads them next `Execute`, **no `Configure`**. The two
topology knobs (`Bloom`, `BloomKernel`) edit `m_SceneSettings` and call `ReconfigureScene()`
on change, exactly as the SSAO/shadows/cull controls do.

## What lands

App-side members holding the per-frame knobs, fed into the `view{}` initializer in place of
the literals:

```cpp
// members
f32 m_Exposure = 1.0f;
f32 m_BloomThreshold = 0.5f;
f32 m_BloomIntensity = 1.5f;
f32 m_BloomRadius = 1.0f;

// in OnRender's view construction (replacing the hardcoded literals):
const Renderer::SceneView view{
    // ...
    .Exposure = m_Exposure,
    .BloomThreshold = m_BloomThreshold,
    .BloomIntensity = m_BloomIntensity,
    .BloomRadius = m_BloomRadius,
};
```

A bloom/tonemap section in the "Scene" window, following the established panel idioms — the
per-frame `UI::Drag`s edit the members (no `Configure`); the topology knobs call
`ReconfigureScene()`:

```cpp
// Tonemap — per-frame member, no recompile.
(void)UI::Drag("Exposure", m_Exposure, {.Speed = 0.01f, .Min = 0.0f, .Max = 16.0f});

// Bloom — on/off + kernel are topology; threshold/intensity/radius are per-frame members.
if (UI::Checkbox("Bloom", m_SceneSettings.Bloom))
{
    ReconfigureScene();
}
if (auto bloom = UI::Disabled(!m_SceneSettings.Bloom))
{
    (void)UI::Drag("Bloom threshold", m_BloomThreshold, {.Speed = 0.01f, .Min = 0.0f, .Max = 8.0f});
    (void)UI::Drag("Bloom intensity", m_BloomIntensity, {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});
    (void)UI::Drag("Bloom radius", m_BloomRadius, {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});

    static constexpr std::array kernelNames{"COD (13-tap/tent)", "Dual Kawase"};
    i32 kernel = static_cast<i32>(m_SceneSettings.Kernel);
    if (UI::Combo("Bloom kernel", kernel, kernelNames))
    {
        m_SceneSettings.Kernel = static_cast<Renderer::SceneRendererSettings::BloomKernel>(kernel);
        ReconfigureScene();
    }
}
```

(The snippet is the intent, not a verbatim drop-in — exact widget/options follow the
surrounding panel. `UI::Disabled` greys the per-bloom knobs when bloom is off, matching the
cascade controls' relationship to the Shadows toggle.)

## Decisions

1. **Per-frame vs topology split is respected at the call site.** `Exposure`,
   `BloomThreshold`, `BloomIntensity`, `BloomRadius` are `SceneView` values, so the UI edits
   app-side members fed into the per-frame `view{}` with **no** `Configure`; `Bloom` on/off
   and `Kernel` are `SceneRendererSettings` topology, so they call `ReconfigureScene()`.
   This is the same discipline every existing control in the panel follows — a recompile only
   when topology actually changes — and it is why Plan 00 moved `Exposure` onto `SceneView`
   (a Settings exposure slider would recompile every drag-frame).

2. **Exposure + the bloom group, which the audit confirms are the only remaining gaps.**
   The user asked for exposure and bloom strength specifically; the audit table above shows
   those plus the rest of the bloom group are the *entire* remaining unexposed set, so this
   plan wires all of them rather than a partial pass that re-opens later.

3. **No new engine knobs.** The knobs all exist after Plans 01–02 (`Exposure` relocated,
   bloom group on `SceneView`/`Settings`); this plan is purely the example's debug-UI wiring
   plus the app-side member plumbing — `main.cpp` only.

4. **Bloom controls grey out when bloom is off.** A `UI::Disabled` scope keyed on
   `!m_SceneSettings.Bloom` keeps the threshold/intensity/radius/kernel knobs visible but
   inert when the effect is disabled, mirroring how the cascade knobs relate to Shadows.

## Files

| File | Change |
|---|---|
| `examples/hello-triangle/main.cpp` | Hold the per-frame knobs (`Exposure`/`BloomThreshold`/`BloomIntensity`/`BloomRadius`) as members fed into the `view{}` initializer (replacing the literals); add the Exposure control and the Bloom section (toggle, threshold, intensity, radius, kernel) to the "Scene" debug window, per the per-frame/topology split. |

## Verification

- Clean build; the example runs windowed and the new controls appear in the "Scene" window.
- Dragging **Exposure**, **Bloom threshold/intensity/radius** edits the app-side members fed
  into the per-frame `view{}` and changes the rendered image live with **no** recompile (no
  `Configure` on those paths); toggling **Bloom** or switching **Bloom kernel** triggers
  `ReconfigureScene()` and re-wires correctly.
- The bloom knobs grey out when **Bloom** is unchecked.
- `smoke_golden` does **not** move (smoke runs headless at fixed settings; the debug UI is
  windowed-only and the defaults are unchanged), and the launcher smoke still writes a
  correct-sized PPM and exits 0.
