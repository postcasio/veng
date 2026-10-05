// The input-map editor panel's load and save, exercised without a frame: the panel reads and writes
// its document through ReadInputMapDocument / WriteInputMapDocument, so a map round-tripped through
// them is exactly what opening and saving it in the panel does to the file.

#include <doctest/doctest.h>

#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

#include "panels/InputMappingEditorPanel.h"
#include "support/TempPath.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

using namespace Veng;
using namespace VengEditor;

namespace
{
    // Every top-level and binding field authored away from its default, plus a key the document
    // does not own, which a save must keep.
    const char* const MapJson = R"({
      "Note": "hand-authored, must survive a save",
      "RequiresGameplayFocus": true,
      "Actions": [
        { "Id": "0x00000000000000A1", "Name": "Throttle", "Kind": "Axis1D" },
        { "Id": "0x00000000000000B2", "Name": "Fire", "Kind": "Button" }
      ],
      "Bindings": [
        {
          "Source": { "Device": "GamepadAxis", "Control": 3 },
          "Action": "0x00000000000000A1",
          "Axis": "X",
          "Scale": -0.5,
          "Threshold": 0.25,
          "Exponent": 2.0,
          "Modifier": { "Device": "GamepadAxis", "Control": 4 },
          "ModifierThreshold": 0.75
        },
        {
          "Source": { "Device": "GamepadAxis", "Control": 5 },
          "Action": "0x00000000000000B2",
          "Axis": "Whole",
          "Scale": 1.0,
          "Threshold": 0.5,
          "Exponent": 1.0,
          "Modifier": { "Device": "None", "Control": 0 },
          "ModifierThreshold": 0.5
        }
      ]
    })";

    nlohmann::json ReadJson(const path& file)
    {
        const std::ifstream in(file, std::ios::binary);
        std::ostringstream contents;
        contents << in.rdbuf();
        return nlohmann::json::parse(contents.str(), nullptr, false);
    }
}

TEST_CASE("input map document: a load and save leaves every authored field unchanged")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);

    const path file = TestSupport::TempDir() / "roundtrip.inputmap.json";
    std::ofstream(file, std::ios::binary | std::ios::trunc) << MapJson;

    const Result<InputMapData> loaded = ReadInputMapDocument(file, types);
    REQUIRE_MESSAGE(loaded.has_value(), loaded.error());
    REQUIRE(loaded->Bindings.size() == 2);
    CHECK(loaded->Bindings[0].Threshold == doctest::Approx(0.25f));
    CHECK(loaded->Bindings[0].Exponent == doctest::Approx(2.0f));
    CHECK(loaded->Bindings[0].Modifier.Device == InputDeviceType::GamepadAxis);
    CHECK(loaded->Bindings[0].Modifier.Control == 4u);
    CHECK(loaded->Bindings[0].ModifierThreshold == doctest::Approx(0.75f));

    REQUIRE(WriteInputMapDocument(file, *loaded, types).has_value());
    CHECK(ReadJson(file) == nlohmann::json::parse(MapJson));
}

TEST_CASE("input map document: a binding authoring no shaping or modifier reads the defaults")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);

    const path file = TestSupport::TempDir() / "defaults.inputmap.json";
    std::ofstream(file, std::ios::binary | std::ios::trunc) << R"({
      "Actions": [ { "Id": "0x00000000000000A1", "Name": "Jump" } ],
      "Bindings": [ { "Source": { "Control": 32 }, "Action": "0x00000000000000A1" } ]
    })";

    const Result<InputMapData> loaded = ReadInputMapDocument(file, types);
    REQUIRE_MESSAGE(loaded.has_value(), loaded.error());
    REQUIRE(loaded->Bindings.size() == 1);
    const Binding& binding = loaded->Bindings[0];
    CHECK(binding.Source.Device == InputDeviceType::Keyboard);
    CHECK(binding.Scale == 1.0f);
    CHECK(binding.Threshold == 0.0f);
    CHECK(binding.Exponent == 1.0f);
    CHECK(binding.Modifier.Device == InputDeviceType::None);
    CHECK(binding.ModifierThreshold == 0.5f);
}
