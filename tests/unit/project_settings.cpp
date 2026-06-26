// Project-settings data-model cases: the reflected ProjectSettings /
// BuildConfiguration round-trip through the generic serializer (exercising the
// new FieldClass::Array support), and the CompressionRole / CompressionFormat
// enum⇄name tables. Pure CPU — no Context, no Vulkan symbol touched, no nlohmann.

#include <doctest/doctest.h>

#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Project/CompressionFormat.h>
#include <Veng/Project/CompressionRole.h>
#include <Veng/Project/ProjectSettings.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>

using namespace Veng;

namespace
{
    // A macOS/ASTC and a Windows/BC7 configuration, the two-target shape the plan
    // names — enough to exercise the array case with more than one element and two
    // distinct codec policies.
    ProjectSettings MakeSettings()
    {
        BuildConfiguration macos;
        macos.Name = "macos";
        macos.Target = "macos-arm64";
        macos.Formats.Color = CompressionFormat::ASTC4x4Srgb;
        macos.Formats.Normal = CompressionFormat::ASTC4x4Unorm;
        macos.Formats.Mask = CompressionFormat::ASTC4x4Unorm;
        macos.Formats.HDR = CompressionFormat::RGBA16Sfloat;
        macos.Formats.UI = CompressionFormat::ASTC4x4Unorm;
        macos.CompressionLevel = 19;
        macos.OutputSuffix = "-macos";

        BuildConfiguration windows;
        windows.Name = "windows";
        windows.Target = "windows-x64";
        windows.Formats.Color = CompressionFormat::BC7Srgb;
        windows.Formats.Normal = CompressionFormat::BC7Unorm;
        windows.Formats.Mask = CompressionFormat::BC7Unorm;
        windows.Formats.HDR = CompressionFormat::RGBA16Sfloat;
        windows.Formats.UI = CompressionFormat::RGBA8Unorm;
        windows.CompressionLevel = 22;
        windows.OutputSuffix = "-windows";

        ProjectSettings settings;
        settings.Configurations = {macos, windows};
        settings.ActiveConfiguration = "macos";
        return settings;
    }

    bool SameConfig(const BuildConfiguration& a, const BuildConfiguration& b)
    {
        return a.Name == b.Name && a.Target == b.Target && a.Formats.Color == b.Formats.Color &&
               a.Formats.Normal == b.Formats.Normal && a.Formats.Mask == b.Formats.Mask &&
               a.Formats.HDR == b.Formats.HDR && a.Formats.UI == b.Formats.UI &&
               a.CompressionLevel == b.CompressionLevel && a.OutputSuffix == b.OutputSuffix;
    }
}

// ---- Enum⇄name tables ------------------------------------------------------

TEST_CASE("CompressionRole names round-trip through ToString/Parse")
{
    for (const CompressionRole role : CompressionRoles)
    {
        const std::string_view name = ToString(role);
        CHECK_FALSE(name.empty());
        const optional<CompressionRole> parsed = ParseCompressionRole(name);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == role);
    }

    CHECK(ToString(CompressionRole::Color) == "Color");
    CHECK(ToString(CompressionRole::HDR) == "HDR");
    CHECK_FALSE(ParseCompressionRole("Nonsense").has_value());
}

TEST_CASE("CompressionFormat names round-trip through ToString/Parse")
{
    for (const CompressionFormat format : CompressionFormats)
    {
        const std::string_view name = ToString(format);
        CHECK_FALSE(name.empty());
        const optional<CompressionFormat> parsed = ParseCompressionFormat(name);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == format);
    }

    CHECK(ToString(CompressionFormat::ASTC4x4Srgb) == "ASTC4x4Srgb");
    CHECK(ToString(CompressionFormat::BC7Unorm) == "BC7Unorm");
    CHECK_FALSE(ParseCompressionFormat("D32Sfloat").has_value());
}

TEST_CASE("CompressionFormat lowers to the engine format vocabulary")
{
    CHECK(ToRendererFormat(CompressionFormat::RGBA8Unorm) == Renderer::Format::RGBA8Unorm);
    CHECK(ToRendererFormat(CompressionFormat::BC7Srgb) == Renderer::Format::BC7Srgb);
    CHECK(ToRendererFormat(CompressionFormat::ASTC4x4Unorm) == Renderer::Format::ASTC4x4Unorm);
    CHECK(ToRendererFormat(CompressionFormat::RGBA16Sfloat) == Renderer::Format::RGBA16Sfloat);
}

// ---- Reflection shape ------------------------------------------------------

TEST_CASE("ProjectSettings reflects Configurations as a FieldClass::Array")
{
    const vector<FieldDescriptor> fields = VengReflect<ProjectSettings>::Fields();
    REQUIRE(fields.size() == 2);

    CHECK(fields[0].Name == "Configurations");
    CHECK(fields[0].Class == FieldClass::Array);
    CHECK(fields[0].ElementType == TypeIdOf<BuildConfiguration>());
    REQUIRE(fields[0].ArraySize != nullptr);
    REQUIRE(fields[0].ArrayElement != nullptr);
    REQUIRE(fields[0].ArrayResize != nullptr);

    CHECK(fields[1].Name == "ActiveConfiguration");
    CHECK(fields[1].Class == FieldClass::String);
}

TEST_CASE("Registering ProjectSettings auto-registers the array element schema")
{
    TypeRegistry registry;
    registry.Register<ProjectSettings>();

    CHECK(registry.IsRegistered(registry.IdOf<ProjectSettings>()));
    CHECK(registry.IsRegistered(registry.IdOf<BuildConfiguration>()));
    CHECK(registry.IsRegistered(registry.IdOf<RoleToFormat>()));
    CHECK(registry.IsRegistered(TypeIdOf<CompressionFormat>()));
    CHECK(registry.IsRegistered(TypeIdOf<string>()));
}

// ---- Generic round-trip ----------------------------------------------------

TEST_CASE("ProjectSettings round-trips two configurations through the array case")
{
    TypeRegistry registry;
    registry.Register<ProjectSettings>();

    const ProjectSettings src = MakeSettings();

    vector<u8> bytes;
    WriteFields(bytes, &src, registry.Info(registry.IdOf<ProjectSettings>()), registry);

    ProjectSettings dst; // fresh, empty Configurations
    const VoidResult read =
        ReadFields(bytes, &dst, registry.Info(registry.IdOf<ProjectSettings>()), registry);
    REQUIRE(read);

    CHECK(dst.ActiveConfiguration == "macos");
    REQUIRE(dst.Configurations.size() == src.Configurations.size());
    for (usize i = 0; i < src.Configurations.size(); ++i)
    {
        CHECK(SameConfig(dst.Configurations[i], src.Configurations[i]));
    }
}

TEST_CASE("An empty configuration array round-trips")
{
    TypeRegistry registry;
    registry.Register<ProjectSettings>();

    ProjectSettings src;
    src.ActiveConfiguration = "none";

    vector<u8> bytes;
    WriteFields(bytes, &src, registry.Info(registry.IdOf<ProjectSettings>()), registry);

    ProjectSettings dst;
    dst.Configurations.resize(3); // a non-empty start the read must shrink to empty
    const VoidResult read =
        ReadFields(bytes, &dst, registry.Info(registry.IdOf<ProjectSettings>()), registry);
    REQUIRE(read);

    CHECK(dst.Configurations.empty());
    CHECK(dst.ActiveConfiguration == "none");
}

TEST_CASE("BuildConfiguration round-trips its role table on its own")
{
    TypeRegistry registry;
    registry.Register<BuildConfiguration>();

    BuildConfiguration src;
    src.Name = "linux";
    src.Target = "linux-x64";
    src.Formats.Color = CompressionFormat::RGBA8Srgb;
    src.Formats.UI = CompressionFormat::RGBA8Unorm;
    src.CompressionLevel = 10;
    src.OutputSuffix = "-linux";

    vector<u8> bytes;
    WriteFields(bytes, &src, registry.Info(registry.IdOf<BuildConfiguration>()), registry);

    BuildConfiguration dst;
    const VoidResult read =
        ReadFields(bytes, &dst, registry.Info(registry.IdOf<BuildConfiguration>()), registry);
    REQUIRE(read);
    CHECK(SameConfig(dst, src));
}
