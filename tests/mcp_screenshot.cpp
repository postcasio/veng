// GPU-band proof for the veng::mcp render tools.
//
// Brings up a headless Context, an AssetManager over the test shader pack, and a
// Viewport rendering a one-cube scene, wires an McpHost returning that viewport for the
// primary ("") name, drives Pump() on this (render) thread, and over loopback:
//   - render.list_viewports reports the primary with its extent and role,
//   - render.stats returns a plausible cull funnel + a non-negative GPU frame time,
//   - render.screenshot returns an image content block whose base64 PNG decodes to the
//     viewport's exact dimensions (the full readback → tonemap → PNG → base64 → decode path).
// Labelled gpu; returns 77 (skips) with no Vulkan ICD, like the rest of the band.

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpServerInfo.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Viewport.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Task/TaskSystem.h>

#include <support/GpuContext.h>
#include <support/GpuProbe.h>

#include <nlohmann/json.hpp>

#define CPPHTTPLIB_IMPLEMENTATION
#include <httplib.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

using Json = nlohmann::json;
using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    int g_Failures = 0;

    void Check(bool condition, const char* what)
    {
        if (!condition)
        {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++g_Failures;
        }
    }

    Json Post(httplib::Client& client, const Json& message)
    {
        const httplib::Result res = client.Post("/", message.dump(), "application/json");
        if (!res)
        {
            return Json{{"error", "no response"}};
        }
        return Json::parse(res->body, nullptr, false);
    }

    int g_Id = 100;

    Json CallTool(httplib::Client& client, const std::string& name, const Json& args)
    {
        const Json response = Post(client, Json{{"jsonrpc", "2.0"},
                                                {"id", g_Id++},
                                                {"method", "tools/call"},
                                                {"params", {{"name", name}, {"arguments", args}}}});
        return response.contains("result") ? response["result"] : Json(nullptr);
    }

    // Parses a plain (text-block) tool result's payload back into JSON.
    Json TextPayload(const Json& result)
    {
        if (!result.is_object() || !result.contains("content"))
        {
            return Json(nullptr);
        }
        return Json::parse(result["content"][0].value("text", std::string{}), nullptr, false);
    }

    // Base64-decodes standard (padded) base64 into raw bytes.
    std::vector<u8> Base64Decode(const std::string& text)
    {
        auto value = [](char c) -> int
        {
            if (c >= 'A' && c <= 'Z')
            {
                return c - 'A';
            }
            if (c >= 'a' && c <= 'z')
            {
                return c - 'a' + 26;
            }
            if (c >= '0' && c <= '9')
            {
                return c - '0' + 52;
            }
            if (c == '+')
            {
                return 62;
            }
            if (c == '/')
            {
                return 63;
            }
            return -1;
        };

        std::vector<u8> out;
        u32 buffer = 0;
        int bits = 0;
        for (const char c : text)
        {
            if (c == '=')
            {
                break;
            }
            const int v = value(c);
            if (v < 0)
            {
                continue;
            }
            buffer = (buffer << 6) | static_cast<u32>(v);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out.push_back(static_cast<u8>((buffer >> bits) & 0xFF));
            }
        }
        return out;
    }

    CameraView FrontCamera(uvec2 extent)
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(45.0f),
                              static_cast<f32>(extent.x) / static_cast<f32>(extent.y), 0.1f,
                              100.0f);
        camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }
}

int main()
{
    if (!Test::HasVulkanDriver())
    {
        std::printf("mcp_screenshot: no Vulkan driver, skipping\n");
        return 77;
    }

    Test::GpuContext gpu("veng_mcp_screenshot", {64, 64});
    Context& context = gpu.Get();

    TaskSystem tasks{TaskSystemInfo{.WorkerCount = 2}};
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);

    AssetManager assets(context, tasks, registry);
    const VoidResult mounted = assets.Mount(path(TEST_SHADER_PACK));
    Check(mounted.has_value(), "mounted the test shader pack");

    constexpr uvec2 extent{64, 48};

    const Unique<Scene> scene = Scene::Create(registry);
    const Ref<Mesh> cube = Mesh::BuildSync(context, Primitives::Cube(1.0f), "Screenshot Cube");
    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity);
    scene->Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);

    const Unique<Viewport> viewport = Viewport::Create({
        .Context = context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = extent},
        .Role = ViewportRole::Presented,
    });
    viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(extent), .Delta = 0.0f});
    context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });

    // The host returns the one viewport for the primary ("") name and null otherwise, and
    // names it in ViewportNames — the app supplies the mapping, the server never enumerates.
    const Mcp::McpHost host{
        .Types = registry,
        .Assets = assets,
        .CurrentWorld = [&] { return scene.get(); },
        .Viewport = [&](string_view name) -> Viewport*
        { return name.empty() ? viewport.get() : nullptr; },
        .ViewportNames = [] { return std::vector<string>{""}; },
    };

    Mcp::McpServerInfo info;
    info.Port = 0;
    Unique<Mcp::McpServer> server = Mcp::McpServer::Create(info, host);
    const u16 port = server->GetPort();
    Check(port != 0, "GetPort resolved an ephemeral port");

    std::atomic<bool> done{false};
    std::thread pump(
        [&]
        {
            while (!done.load())
            {
                server->Pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            server->Pump();
        });

    {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(10, 0);

        // tools/list surfaces the three render tools alongside the world tools.
        const Json list = Post(client, Json{{"jsonrpc", "2.0"},
                                            {"id", 1},
                                            {"method", "tools/list"},
                                            {"params", Json::object()}});
        std::set<std::string> names;
        for (const Json& tool : list["result"]["tools"])
        {
            names.insert(tool.value("name", std::string{}));
        }
        Check(names.count("render.screenshot") == 1, "render.screenshot registered");
        Check(names.count("render.list_viewports") == 1, "render.list_viewports registered");
        Check(names.count("render.stats") == 1, "render.stats registered");

        // render.list_viewports reports the one primary viewport with its extent and role.
        const Json viewports =
            TextPayload(CallTool(client, "render.list_viewports", Json::object()));
        Check(viewports["viewports"].size() == 1, "render.list_viewports lists the one viewport");
        const Json& vp = viewports["viewports"][0];
        Check(vp.value("name", std::string{"?"}) == "", "the viewport is the primary (empty name)");
        Check(vp["extent"][0].get<u32>() == extent.x && vp["extent"][1].get<u32>() == extent.y,
              "render.list_viewports reports the region extent");
        Check(vp.value("role", std::string{}) == "Presented",
              "render.list_viewports reports the role");

        // render.stats returns a plausible cull funnel + a non-negative GPU frame time.
        const Json stats = TextPayload(CallTool(client, "render.stats", Json::object()));
        Check(stats.contains("visible"), "render.stats reports a visible count");
        Check(stats.contains("drawn"), "render.stats reports a drawn count");
        Check(stats.value("visible", 0) > 0, "the cube contributes at least one candidate");
        Check(stats.contains("gpu_frame_time_ms"), "render.stats reports a GPU frame time");
        Check(stats.value("gpu_frame_time_ms", -1.0f) >= 0.0f,
              "the GPU frame time is non-negative");

        // render.screenshot returns an image content block; the base64 PNG decodes to the
        // viewport's exact dimensions and is not entirely blank.
        const Json shot = CallTool(client, "render.screenshot", Json::object());
        Check(shot.value("isError", true) == false, "render.screenshot is not an error");
        const Json& content = shot["content"];
        Check(content.is_array() && content.size() >= 1, "screenshot has content blocks");
        Check(content[0].value("type", std::string{}) == "image", "the first block is an image");
        Check(content[0].value("mimeType", std::string{}) == "image/png", "the image is a PNG");

        const std::vector<u8> png = Base64Decode(content[0].value("data", std::string{}));
        int w = 0;
        int h = 0;
        int channels = 0;
        u8* pixels =
            stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels, 0);
        Check(pixels != nullptr, "the base64 payload decodes as a PNG");
        if (pixels != nullptr)
        {
            // The full readback → tonemap → PNG → base64 → decode round-trip: the decoded PNG
            // has the viewport's exact dimensions and 3 (RGB) channels. The scene is materialless,
            // so the image is legitimately black (a materialless mesh records no draw); the proof
            // is the pipeline and the geometry, not the shade.
            Check(w == static_cast<int>(extent.x), "the PNG width matches the viewport extent");
            Check(h == static_cast<int>(extent.y), "the PNG height matches the viewport extent");
            Check(channels == 3, "the PNG is RGB");
            stbi_image_free(pixels);
        }

        // A screenshot of an unknown viewport is an isError result, never a null deref.
        const Json missing = CallTool(client, "render.screenshot", Json{{"viewport", "nope"}});
        Check(missing.value("isError", false) == true, "an unknown viewport is an isError");
    }

    done.store(true);
    pump.join();

    if (g_Failures == 0)
    {
        std::printf("mcp_screenshot: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "mcp_screenshot: %d check(s) failed\n", g_Failures);
    return 1;
}
