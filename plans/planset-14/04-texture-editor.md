# Plan 04 — texture editor

**Goal:** deliver the first end-to-end asset editor: a docked `TextureEditorPanel` that
shows a decoded texture image in a live preview, exposes `.tex.json` settings (sRGB,
sampler min/mag/wrap/mip filter), triggers a live recook on any setting change (debounced),
and round-trips the JSON source on save. This is the template for all later asset editors in
area 6.

## `TextureEditorPanel`

Opened when the user double-clicks a texture asset in the asset browser. `EditorHost` calls
the registered `AssetEditorFactory` for `AssetType::Texture`, which creates a new
`TextureEditorPanel` and adds it to the open panel list. Multiple panels for different
texture assets are independent.

The panel owns:
- The source `.tex.json` path.
- The current in-memory settings (sRGB, sampler fields) parsed from the JSON on open.
- A `Ref<ImGuiTexture>` for the preview (created via `ImGuiLayer::CreateTexture`).
- The current `MountHandle` for the last cooked blob (null until first cook; replaced on
  each recook).
- A `bool m_CookPending` and a debounce timer: settings changes set `m_CookPending`; the
  cook fires after 300ms of no further changes (so rapid slider drags don't issue a cook
  per frame).
- An `optional<string> m_CookError` for inline error display.

## Preview render target

The texture preview uses an existing `AssetHandle<Texture>` loaded from the mounted archive
(after the first cook completes). The handle's `Ref<ImageView>` is passed to
`ImGuiLayer::CreateTexture` to get a `Ref<ImGuiTexture>`. `ImGui::Image(textureId, size)`
draws it in the panel. On recook the old `ImGuiTexture` is destroyed via
`ImGuiLayer::DestroyTexture`, the `MountHandle` is replaced (retiring the old cooked bytes),
a new `AssetManager::Load<Texture>` is issued, and the new `Ref<ImageView>` produces a new
`ImGuiTexture` once the load completes.

The panel shows a spinner / "Cooking…" overlay while a cook is in flight.

## Settings UI

```
┌─ Texture Editor: brick.tex.json ─────────────────────────────────────────┐
│  [preview area — zoom/pan with scroll and right-drag]                     │
│                                                                            │
│  Channel:  [RGB ▾]   Zoom: [1.0x ▾]                                       │
│  ─────────────────────────────────────────────────────────────────────    │
│  sRGB:         [x]                                                         │
│  Min filter:   [Linear ▾]                                                  │
│  Mag filter:   [Linear ▾]                                                  │
│  Wrap U:       [Repeat ▾]                                                  │
│  Wrap V:       [Repeat ▾]                                                  │
│  Mip filter:   [Linear ▾]                                                  │
│  ─────────────────────────────────────────────────────────────────────    │
│  [Save]    [Revert]    [status / error]                                    │
└────────────────────────────────────────────────────────────────────────────┘
```

Each setting maps to a field in `.tex.json`. Changing any field:
1. Updates the in-memory settings struct.
2. Sets `m_CookPending = true`, resets the debounce timer.
3. After the debounce expires, calls `EditorHost::RequestCook` with the modified source (a
   temp JSON written from the in-memory settings) and the texture's `AssetId`.

## JSON round-trip

On **Save**, the in-memory settings are serialized back to the source `.tex.json` using the
same JSON library the cooker uses (`nlohmann::json`). The save round-trip:
1. Reads the existing `.tex.json` into a `json` object (preserving any top-level keys the
   editor does not know about — this is the "don't destroy hand-authored structure" contract).
2. Patches only the keys corresponding to the edited settings.
3. Writes the patched object back to disk with 4-space indentation.

On **Revert**, the in-memory settings are re-parsed from the on-disk `.tex.json` and a
recook is triggered from the original source.

## Registering the texture editor in `libveng_editor`

`libveng_editor` registers a built-in `TextureEditorFactory` into the `EditorRegistry` for
`AssetType::Texture` during `EditorHost::OnInitialize`. Game modules can override this with
their own factory by registering a new one for the same type (last-write-wins, or first-
write-wins — document the policy; first-write-wins is simpler and matches the planset-5
convention for importers).

## Registering in `hello_triangle_editor`

`libhello_triangle_editor`'s `VengModuleRegister` does not need to register a texture
editor — the built-in one handles `brick.tex.json` already. It can leave the asset editor
registry untouched. The manual verification uses the built-in.

## Tests

- **Manual end-to-end:** launch `hello_triangle-editor`, open the brick texture from the
  asset browser, verify the preview shows the brick image, toggle sRGB off, verify the
  preview recooks and updates (colors shift), revert, verify the original appears. Save with
  a modified wrap mode, verify the `.tex.json` on disk reflects the change.
- **Cooker unit test (plan 03's test extended):** verify that a full
  `CookSession::Cook(brick.tex.json) → MountMemory → Load<Texture>` cycle produces a
  resident handle. GPU-free (the importer writes raw bytes; `Load` with `LoadSync` on a
  test context uploads — gate under the `gpu` label).
- `ctest --output-on-failure` green; smoke PPM unchanged; `include_hygiene` green.

## Acceptance

Clean build; `hello_triangle-editor` opens the texture editor for the brick texture;
preview renders the decoded image; toggling sRGB triggers a live recook and the preview
updates; Save round-trips the JSON without destroying existing keys; Revert restores the
original; `ctest` green; smoke PPM unchanged. Commit: `Plan 04: TextureEditorPanel —
preview RT, .tex.json settings editing, live recook on change, JSON round-trip`.
