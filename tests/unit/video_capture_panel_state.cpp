// The capture panel's two pure helpers — everything about the panel that is not a draw call. The
// panel itself is widget code and is not asserted on here (no layout, no draw); what is worth
// pinning is the seam between the widgets' buffers and the settings a Start actually receives,
// because that is where a typed name can go missing, and that the megabit figure the panel shows
// beside the quality control is the recorder's own derivation rather than a second one drifting
// beside it.

#include <doctest/doctest.h>

#include <Capture/RecorderCore.h>

#include <Veng/UI/VideoCapture.h>

using namespace Veng;
using namespace Veng::Capture;

TEST_CASE("capture panel state: the name field is what a capture is started with")
{
    UI::VideoCapturePanelState state;

    state.Name = "flyby-01";
    CHECK(UI::ResolveCaptureSettings(state).Name == "flyby-01");

    // Surrounding whitespace a field picks up is not part of the name, and a blank field leaves
    // the name empty — which is what the recorder resolves to its own default form.
    state.Name = "  flyby-01\t";
    CHECK(UI::ResolveCaptureSettings(state).Name == "flyby-01");

    state.Name = "   ";
    CHECK(UI::ResolveCaptureSettings(state).Name.empty());

    state.Name.clear();
    CHECK(UI::ResolveCaptureSettings(state).Name.empty());
}

TEST_CASE("capture panel state: the edited settings reach Start unchanged")
{
    UI::VideoCapturePanelState state;
    state.Settings.Encoding = CaptureEncoding::Hdr10;
    state.Settings.Codec = VideoCodec::ProRes4444;
    state.Settings.Lockstep = true;
    state.Settings.FrameRate = 30;
    state.Settings.FrameBudget = 120;
    state.Settings.Audio = AudioTrack::None;
    state.Settings.IncludeOverlay = true;
    state.Settings.BitsPerPixel = 0.4f;

    const VideoCaptureSettings resolved = UI::ResolveCaptureSettings(state);
    CHECK(resolved.Encoding == CaptureEncoding::Hdr10);
    CHECK(resolved.Codec == VideoCodec::ProRes4444);
    CHECK(resolved.Lockstep);
    CHECK(resolved.FrameRate == 30);
    CHECK(resolved.FrameBudget == 120);
    CHECK(resolved.Audio == AudioTrack::None);
    CHECK(resolved.IncludeOverlay);
    CHECK(resolved.BitsPerPixel == doctest::Approx(0.4f));
}

TEST_CASE("capture panel state: the megabit figure is the recorder's own derivation")
{
    VideoCaptureSettings settings;
    settings.FrameRate = 60;
    settings.BitsPerPixel = 0.15f;

    constexpr uvec2 Window{1280, 720};
    constexpr uvec2 Retina{3840, 2160};

    // The panel reports whole megabits of whatever the recorder would encode at, so the two move
    // together at every extent rather than the panel carrying a second formula.
    for (const uvec2 extent : {Window, Retina})
    {
        const u64 bits = DeriveBitsPerSecond(settings, extent);
        CHECK(UI::CaptureBitrateMbps(settings, extent) == (bits + 500000ULL) / 1000000ULL);
    }

    // An override is the bitrate outright, at any extent, and an unknown extent derives nothing.
    settings.BitrateMbps = 80;
    CHECK(UI::CaptureBitrateMbps(settings, Window) == 80);
    CHECK(UI::CaptureBitrateMbps(settings, Retina) == 80);

    settings.BitrateMbps = 0;
    CHECK(UI::CaptureBitrateMbps(settings, uvec2{0, 0}) == 0);
}
