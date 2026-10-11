// The still-picture helpers behind every screenshot: the display encode a download takes by its
// format, the PNG it becomes, and the name a saved screenshot claims. Device-free — the GPU half (a
// real image read back, the capture sink) is tests/gpu/frame_capture.cpp's.

#include <doctest/doctest.h>

#include <Veng/Capture/Screenshot.h>
#include <Veng/Path.h>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>

using namespace Veng;

namespace
{
    /// @brief A fresh scratch directory under the system temp tree, removed on scope exit.
    class ScratchDir
    {
    public:
        explicit ScratchDir(const string& label)
            : m_Path(std::filesystem::temp_directory_path() / ("veng_screenshot_" + label))
        {
            std::filesystem::remove_all(m_Path);
            std::filesystem::create_directories(m_Path);
        }

        ~ScratchDir() { std::filesystem::remove_all(m_Path); }

        ScratchDir(const ScratchDir&) = delete;
        ScratchDir& operator=(const ScratchDir&) = delete;

        [[nodiscard]] const path& Get() const { return m_Path; }

    private:
        path m_Path;
    };

    /// @brief A fixed local moment: 2026-03-04 05:06:07.
    std::tm FixedMoment()
    {
        std::tm moment{};
        moment.tm_year = 2026 - 1900;
        moment.tm_mon = 2;
        moment.tm_mday = 4;
        moment.tm_hour = 5;
        moment.tm_min = 6;
        moment.tm_sec = 7;
        return moment;
    }
}

TEST_CASE("screenshot: a linear half-float download takes the sRGB transfer function")
{
    // Black, white, linear mid-grey, and an over-range value (an HDR highlight) that clamps.
    const std::array<f32, 4> linear{0.0f, 1.0f, 0.2140f, 4.0f};
    vector<u16> halves;
    for (const f32 value : linear)
    {
        const u16 half = glm::packHalf1x16(value);
        halves.insert(halves.end(), {half, half, half, glm::packHalf1x16(1.0f)});
    }
    const std::span<const u8> bytes(reinterpret_cast<const u8*>(halves.data()),
                                    halves.size() * sizeof(u16));

    const Result<Capture::RgbImage> image =
        Capture::ConvertToRgb8(bytes, uvec2(4, 1), Renderer::Format::RGBA16Sfloat);
    REQUIRE(image.has_value());
    REQUIRE(image->Pixels.size() == 4 * 3);
    CHECK(image->Pixels[0] == 0);
    CHECK(image->Pixels[3] == 255);
    // Linear 0.214 is sRGB ~0.5: a write skipping the transfer function would read ~55.
    CHECK(image->Pixels[6] >= 126);
    CHECK(image->Pixels[6] <= 129);
    CHECK(image->Pixels[9] == 255);
}

TEST_CASE("screenshot: 8-bit downloads keep their values and lose only alpha and channel order")
{
    const std::array<u8, 8> texels{10, 20, 30, 255, 40, 50, 60, 0};

    const Result<Capture::RgbImage> rgba =
        Capture::ConvertToRgb8(texels, uvec2(2, 1), Renderer::Format::RGBA8Unorm);
    REQUIRE(rgba.has_value());
    CHECK(rgba->Pixels == vector<u8>{10, 20, 30, 40, 50, 60});

    const Result<Capture::RgbImage> bgra =
        Capture::ConvertToRgb8(texels, uvec2(2, 1), Renderer::Format::BGRA8Srgb);
    REQUIRE(bgra.has_value());
    CHECK(bgra->Pixels == vector<u8>{30, 20, 10, 60, 50, 40});
}

TEST_CASE("screenshot: a short download and an unencodable format are refused with a reason")
{
    const std::array<u8, 4> oneTexel{1, 2, 3, 4};

    const Result<Capture::RgbImage> shortDownload =
        Capture::ConvertToRgb8(oneTexel, uvec2(2, 2), Renderer::Format::RGBA8Unorm);
    REQUIRE_FALSE(shortDownload.has_value());
    CHECK_FALSE(shortDownload.error().empty());

    const Result<Capture::RgbImage> depth =
        Capture::ConvertToRgb8(oneTexel, uvec2(1, 1), Renderer::Format::D32Sfloat);
    REQUIRE_FALSE(depth.has_value());
    CHECK_FALSE(depth.error().empty());
}

TEST_CASE("screenshot: a PNG carries the signature and the picture's dimensions")
{
    const Capture::RgbImage image{.Width = 3, .Height = 2, .Pixels = vector<u8>(3 * 2 * 3, 128)};
    const Result<vector<u8>> png = Capture::EncodePng(image);
    REQUIRE(png.has_value());
    REQUIRE(png->size() > 24);

    constexpr std::array<u8, 8> Signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    CHECK(std::equal(Signature.begin(), Signature.end(), png->begin()));

    // IHDR is the first chunk: its width and height are big-endian words at bytes 16 and 20.
    const auto word = [&png](const usize at)
    {
        return (u32((*png)[at]) << 24) | (u32((*png)[at + 1]) << 16) | (u32((*png)[at + 2]) << 8) |
               u32((*png)[at + 3]);
    };
    CHECK(word(16) == 3);
    CHECK(word(20) == 2);

    CHECK_FALSE(Capture::EncodePng(Capture::RgbImage{}).has_value());
}

TEST_CASE("screenshot: the name is the local moment, sortable and free of path separators")
{
    CHECK(Capture::ScreenshotStem(FixedMoment()) == "2026-03-04_05-06-07");
}

TEST_CASE("screenshot: claiming a name never reuses one, even within the same second")
{
    const ScratchDir scratch("claims");
    const std::tm moment = FixedMoment();

    // A file already there, as a second process would leave it.
    const path taken = scratch.Get() / "2026-03-04_05-06-07_2.png";
    std::FILE* const existing = std::fopen(taken.string().c_str(), "wb");
    REQUIRE(existing != nullptr);
    std::fputs("not a png", existing);
    std::fclose(existing);

    vector<path> claimed;
    for (int shot = 0; shot < 3; ++shot)
    {
        const Result<path> next = Capture::ReserveScreenshotPath(scratch.Get(), moment);
        REQUIRE(next.has_value());
        claimed.emplace_back(*next);
    }

    CHECK(claimed[0].filename() == "2026-03-04_05-06-07.png");
    CHECK(claimed[1].filename() == "2026-03-04_05-06-07_3.png");
    CHECK(claimed[2].filename() == "2026-03-04_05-06-07_4.png");
    for (const path& file : claimed)
    {
        CHECK(std::filesystem::exists(file));
        CHECK(file.parent_path() == scratch.Get());
    }
    // The existing file was skipped, not truncated.
    CHECK(std::filesystem::file_size(taken) == string("not a png").size());
}

TEST_CASE("screenshot: a written PNG lands in the claimed file")
{
    const ScratchDir scratch("write");
    const Result<path> file = Capture::ReserveScreenshotPath(scratch.Get(), FixedMoment());
    REQUIRE(file.has_value());

    const Capture::RgbImage image{.Width = 2, .Height = 2, .Pixels = vector<u8>(2 * 2 * 3, 7)};
    REQUIRE(Capture::WritePng(image, *file).has_value());

    const Result<vector<u8>> expected = Capture::EncodePng(image);
    REQUIRE(expected.has_value());
    CHECK(std::filesystem::file_size(*file) == expected->size());

    CHECK_FALSE(Capture::WritePng(image, scratch.Get() / "missing" / "shot.png").has_value());
}
