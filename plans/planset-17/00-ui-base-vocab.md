# Plan 00 — `Veng::UI` base widget vocabulary

**Goal:** land the base immediate-mode vocabulary in `libveng` — the overloaded free
functions, layout helpers, and item/hover queries, plus the options structs they take.
This is the foundation the scopes (plan 01) and every migration (plans 02–04) build on.

## What lands

Six headers under `engine/include/Veng/UI/` (the umbrella + this plan's five free-function
groups; `Scopes.h` is plan 01) and the implementation TUs under `engine/src/UI/`.

### `Veng/UI/UI.h` — umbrella

Pulls `Types.h`, `Widgets.h`, `Layout.h`, `Query.h`, `Scopes.h` so a consumer writes one
`#include <Veng/UI/UI.h>` and has the whole surface. (It forward-references `Scopes.h`,
which plan 01 adds; this plan includes only the four it lands and plan 01 extends the
umbrella.)

### `Veng/UI/Types.h` — vocabulary, no imgui

Designated-initializer options structs (the `XInfo` idiom). No imgui types appear.

```cpp
namespace Veng::UI
{
    struct DragOptions   { f32 Speed = 0.01f; optional<f32> Min, Max; const char* Format = "%.3f"; };
    struct SliderOptions { f32 Min = 0.0f, Max = 1.0f; const char* Format = "%.3f"; };
}
```

(The window/tree/style flag enums — `WindowFlags`, `TreeFlags`, `StyleColorId`,
`StyleVarId` — are added to `Types.h` by **plan 01**, where the scopes that consume them
land. This plan's widgets need none of them.)

### `Veng/UI/Widgets.h` — overloaded, immediate-mode

One `Drag` overloaded on the value type (decision 1); every edit returns
`[[nodiscard]] bool` "changed". Text is preformatted `string_view` (decision 3).

```cpp
namespace Veng::UI
{
    // edits — `Drag` absorbs DragFloat/DragFloat2/3/4 + DragInt
    bool Drag(string_view label, f32&  v, DragOptions = {});
    bool Drag(string_view label, vec2& v, DragOptions = {});
    bool Drag(string_view label, vec3& v, DragOptions = {});
    bool Drag(string_view label, vec4& v, DragOptions = {});
    bool Drag(string_view label, i32&  v, DragOptions = {});

    bool Slider(string_view label, f32& v, SliderOptions);
    bool Slider(string_view label, i32& v, i32 min, i32 max);

    bool Checkbox(string_view label, bool& v);
    bool InputText(string_view label, string& v);                  // commit-on-deactivate; see decision 5
    bool Combo(string_view label, i32& index, std::span<const string_view> items);

    // buttons / selection
    bool Button(string_view label);
    bool Selectable(string_view label, bool selected = false);

    // text — preformatted, no varargs
    void Text(string_view text);
    void TextDisabled(string_view text);
    void TextColored(vec4 color, string_view text);
    void Label(string_view label, string_view value);              // was LabelText

    // a registered ImGui texture (the 16 ImGui::Image sites)
    void Image(const Ref<ImGuiTexture>& tex, vec2 size);
}
```

### `Veng/UI/Layout.h` — the small remainder

```cpp
namespace Veng::UI
{
    void Separator();
    void SameLine();
    void Spacing();
    vec2 ContentRegionAvail();          // was GetContentRegionAvail
    void ScrollToHere();                // was SetScrollHereY
}
```

### `Veng/UI/Query.h` — item/hover queries + stats

```cpp
namespace Veng::UI
{
    bool ItemHovered();                 // was IsItemHovered
    bool ItemEdited();                  // was IsItemDeactivatedAfterEdit
    void Tooltip(string_view text);     // was the IsItemHovered + SetTooltip pair
    f32  FrameRate();                   // was ImGui::GetIO().Framerate — the stats readout
}
```

Keyboard/mouse queries are **not** added here (decision 5) — they converge with the
event/input area, not a parallel `Veng::UI` key-enum.

## Decisions

1. **imgui-free signatures, imgui-only `.cpp`s** (planset decision 2). The headers name
   only engine types: `f32`/`vec2`/`vec3`/`vec4`/`i32`, `string`/`string_view`, `vec4` for
   color, `Ref<ImGuiTexture>` (the engine's own wrapper). `engine/src/UI/Widgets.cpp`,
   `Layout.cpp`, `Query.cpp` are the only TUs that `#include <imgui.h>`, where each free
   function translates to the matching `ImGui::` call. A `Veng::UI` label `string_view` is
   passed to ImGui through its data/size (ImGui's `const char*` overloads take an explicit
   end pointer); the small `InputText` char-buffer dance is owned here once.

2. **`Combo` takes `std::span<const string_view>`.** Matches the existing `std::span`
   spelling in public headers (`Buffer.h`, `Image.h`, `ShaderModule.h`); no new `span`
   house alias is introduced. The `.cpp` adapts the span to ImGui's items-getter form.

3. **`Slider(f32&, SliderOptions)` has no default options** — a slider without a declared
   range is meaningless, so the options struct (carrying `Min`/`Max`) is required, matching
   the design sketch. `Slider(i32&, min, max)` takes the bounds positionally (the common
   integer case).

4. **`DragOptions::Min`/`Max` are `optional<f32>`** — `nullopt` means unclamped (ImGui's
   `0,0` range sentinel). The `.cpp` maps a present min/max to ImGui's clamp args and
   `AlwaysClamp` flag; absent → no clamp. `Format` stays a `const char*` printf spec (it is
   ImGui's own numeric format, not user text — the printf-varargs concern of decision 3 is
   about *text content*, not numeric formatting). The default `Format = "%.3f"` is a
   float spec; the `Drag(i32&, …)` overload's `.cpp` substitutes `"%d"` when `Format` is the
   default sentinel (a caller may still override it), so the shared `DragOptions` struct
   serves both the float and integer overloads.

5. **`InputText` commits on deactivate, owning the scratch buffer.** The signature binds the
   destination `string& v` directly, but the wrapper does **not** write `v` per keystroke. It
   keeps an internal scratch buffer (seeded from `v` on the frame the item gains focus),
   edits that, and writes `v` back **only** on Enter or deactivate-after-edit — preserving the
   `EnterReturnsTrue` + `IsItemDeactivatedAfterEdit` semantics the reflection inspector relies
   on today. The returned `bool` means **committed** (a new value was written to `v`), not
   "edited this frame". A consumer therefore does **not** pair it with `UI::ItemEdited()` —
   the commit gate lives inside `InputText`. The fixed-256 truncation of the current call site
   is removed (the scratch grows with the string) — a capability gain, not a behavior change
   to the commit point.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/UI/UI.h` | New — umbrella include. |
| `engine/include/Veng/UI/Types.h` | New — `DragOptions`, `SliderOptions`. |
| `engine/include/Veng/UI/Widgets.h` | New — the overloaded widget free functions. |
| `engine/include/Veng/UI/Layout.h` | New — layout helpers. |
| `engine/include/Veng/UI/Query.h` | New — item/hover queries + `FrameRate`. |
| `engine/src/UI/Widgets.cpp` | New — impl (`#include <imgui.h>`). |
| `engine/src/UI/Layout.cpp` | New — impl. |
| `engine/src/UI/Query.cpp` | New — impl. |
| `engine/CMakeLists.txt` | Add the three `src/UI/*.cpp` to the SOURCES list. |
| `tests/include_hygiene/*` | Add the new `Veng/UI/*.h` headers to the compiled-header set. |

`ImGuiTexture.h` is already a public engine header (`GetTextureId()` → `u64`); `Widgets.h`
includes it for the `Image` overload. No new public dependency.

## Verification

- Clean build (`cmake -B build -S . && cmake --build build -j 2`) — the new TUs compile and
  link into `libveng`.
- `include_hygiene` compiles the new headers while linking only veng's PUBLIC deps — proves
  the `Veng/UI/` signatures pull in no backend include (imgui appears only in the `.cpp`s,
  which are not part of that test).
- `ctest --test-dir build --output-on-failure` green across the existing bands — this plan
  adds API, changes no behavior, so the suites are unaffected.
- Smoke PPM correct size + exit 0 (no call site uses `Veng::UI` yet; the library merely
  exists).

No migration in this plan — it lands the vocabulary. Plans 02–04 consume it.
