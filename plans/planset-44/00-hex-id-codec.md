# Plan 00 — the hex-id codec + zero-padded convention

**Goal:** one shared, JSON-free, reliably-implemented codec that turns a 64-bit id into the
canonical zero-padded hex string and back, plus the `generate-id` output and the C++-literal
convention tightened to the same spelling. No consumer converts its JSON yet — this plan is
the tool every later plan calls.

## What lands

### 1. `assetpack/include/Veng/Asset/HexId.h` + `assetpack/src/HexId.cpp`

The public header declares only `string`↔`u64` functions — no `fmt`, no `nlohmann`, so the
`include_hygiene` and JSON-free guarantees hold:

```cpp
namespace Veng
{
    /// @brief Formats a 64-bit id as the canonical "0x" + 16-uppercase-hex-digit string.
    string FormatHexId(u64 value);

    /// @brief Parses a hex id string (optional 0x/0X prefix, case-insensitive) to a u64.
    ///
    /// Returns nullopt on empty input, overflow past 64 bits, or any non-hex or trailing
    /// character.
    optional<u64> ParseHexId(string_view text);

    /// @brief Formats an AssetId as a canonical hex-id string.
    string FormatAssetId(AssetId id);

    /// @brief Parses a canonical hex-id string into an AssetId.
    optional<AssetId> ParseAssetId(string_view text);
}
```

The `.cpp`:

- **`FormatHexId`** = `fmt::format("0x{:016X}", value)`. fmt is already a PRIVATE dep of
  `libveng_assetpack` (used in `Archive.cpp`/`CookedProject.cpp`), so this adds no new
  dependency and no header leak. `{:016X}` yields the exact zero-padded 16-digit uppercase
  form with no manual padding.
- **`ParseHexId`** skips a leading `0x`/`0X` if present, then
  `std::from_chars(first, last, value, /*base=*/16)`. Returns `nullopt` unless
  `ec == std::errc{}` **and** `ptr == last` (no trailing characters) **and** the input past
  the prefix is non-empty. `from_chars` is case-insensitive for hex and rejects overflow via
  `errc::result_out_of_range`. `<charconv>` is the include.
- `FormatAssetId`/`ParseAssetId` are thin wrappers (`FormatHexId(id.Value)` /
  `ParseHexId(text).transform(...)`).

`ActionId` is `enum class ActionId : u64` (not a bare `u64`), as is `SystemId`. Neither gets a
typed wrapper: every call site spells the cast explicitly so the codec surface stays one shape.
Write: `FormatHexId(static_cast<u64>(id))`. Read:
`static_cast<ActionId>(ParseHexId(text).value_or(0))` (mirroring the existing decimal casts, e.g.
[InputMapImporter.cpp:107](../../cooker/src/Importers/InputMapImporter.cpp) does
`action.Id = static_cast<ActionId>(id);` today). Committing to no-wrapper up front keeps Plans 02
and 03 — which both touch `ActionId`/`SystemId` sites — from diverging into a half-wrapped surface.

### 2. `assetpack/CMakeLists.txt`

Add `src/HexId.cpp` to the target sources. No new link deps (fmt already PRIVATE).

### 3. `vengc generate-id` output

[cooker/tool/main.cpp:468](../../cooker/tool/main.cpp) currently prints
`0x{:X}` (C++) and decimal (JSON). Change to the zero-padded, string-for-JSON form:

```cpp
fmt::print("hex (C++):   0x{:016X}ULL\n", id.Value);
fmt::print("hex (JSON):  \"0x{:016X}\"\n", id.Value);
```

(Drop the now-obsolete "decimal (JSON)" line; the JSON form is the hex string.)

`vengc generate-type-id` ([main.cpp:525](../../cooker/tool/main.cpp)) keeps its own
`decimal (JSON)` line **unchanged and untouched by this planset**: a `TypeId` never appears in
JSON as a number (it serializes by name — see the README's `TypeId` decision), so there is no
hex-JSON form to print for it. This is a deliberate exclusion, not an oversight.

## Tests

Add `tests/unit/hex_id.cpp` (registered in the `unit` suite):

- **Round-trip:** `ParseHexId(FormatHexId(v)) == v` over a spread including `1`, a mid value,
  and the boundary `0xFFFFFFFFFFFFFFFF`.
- **Zero-padding:** `FormatHexId(0x3E9) == "0x00000000000003E9"` (exactly 16 digits after
  `0x`, uppercase).
- **Parse tolerance:** accepts `"0x00000000000003E9"`, `"0x3e9"` (lowercase, unpadded),
  `"3E9"` (no prefix). All parse to `0x3E9`.
- **Parse rejection (→ nullopt):** `""`, `"0x"`, `"0xG"`, `"0x12cat"` (trailing junk),
  `"0x1FFFFFFFFFFFFFFFF"` (17 digits, overflow), a decimal-looking `"12345678901234567890"`
  that overflows.
- **`0x0`:** parses to `0` — the caller (not the codec) decides that a zero id is the invalid
  sentinel, mirroring the existing `AssetId::IsValid()` split.
- **`FormatAssetId`/`ParseAssetId`** delegate correctly (one round-trip case).

## Verification

- `build-debug` clean; `ctest -R hex_id` green; full `ctest` still green (nothing else
  changed yet).
- `include_hygiene` builds — `HexId.h` pulls in no backend/JSON include.
- Manual: `vengc generate-id` prints a 16-digit padded C++ literal and a quoted hex JSON
  string.

## Out of scope

- Any consumer conversion (Plans 01–03) and any asset/fixture/doc migration or C++-literal
  reformat (Plan 04). This plan ships the tool and leaves every current caller untouched.
