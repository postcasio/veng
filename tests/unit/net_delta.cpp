// Delta component codec: the per-component wire body that opts down from a full self-describing
// record to a field-presence delta against a shared baseline, quantizing the Transform's spatial
// leaves. Round-trips full and delta forms against mutated baselines, checks the delta carries only
// changed fields, and that a delta with no baseline falls back cleanly. Device-free over a registry.

#include <doctest/doctest.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <Veng/Asset/Prefab.h>
#include <Veng/Net/DeltaCodec.h>
#include <Veng/Net/Replication.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    vector<u8> Serialize(const void* value, const TypeInfo& info, const TypeRegistry& registry)
    {
        vector<u8> bytes;
        WriteFields(bytes, value, info, registry);
        return bytes;
    }

    // Decodes a body against a baseline, then reads the resulting WriteFields bytes back into a value.
    template <class T>
    T RoundTrip(std::span<const u8> body, std::span<const u8> baseline, const TypeInfo& info,
                const TypeRegistry& registry, const QuantizationSettings& quant)
    {
        vector<u8> outBytes;
        const VoidResult decoded = DecodeComponentBody(
            body, baseline, info.Id, TypeIdOf<Transform>(), registry, quant, outBytes);
        REQUIRE(decoded.has_value());
        T value{};
        REQUIRE(ReadFields(outBytes, &value, info, registry).has_value());
        return value;
    }
}

TEST_CASE("Transform full body quantizes and round-trips within tolerance")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const TypeInfo& info = registry.Info(TypeIdOf<Transform>());
    const QuantizationSettings quant{.PositionQuantum = 0.001f, .PositionExtent = 4096.0f};

    Transform t;
    t.Position = vec3(3.5f, -2.25f, 100.125f);
    t.Rotation = glm::normalize(glm::quat(0.2f, 0.4f, -0.5f, 0.7f));
    t.Scale = vec3(2.0f, 2.0f, 2.0f);

    vector<u8> body;
    EncodeComponentBody(body, info.Id, Serialize(&t, info, registry), {}, /*forceFull=*/false,
                        TypeIdOf<Transform>(), registry, quant);
    CHECK(static_cast<ComponentEncoding>(body[0]) == ComponentEncoding::TransformQuant);

    const auto decoded = RoundTrip<Transform>(body, {}, info, registry, quant);
    CHECK(glm::length(decoded.Position - t.Position) < 0.002f);
    CHECK(std::abs(glm::dot(decoded.Rotation, t.Rotation)) > 0.999f);
    CHECK(glm::length(decoded.Scale - t.Scale) < 1e-4f);
}

TEST_CASE("Transform delta sends only the changed leaves and keeps the rest from the baseline")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const TypeInfo& info = registry.Info(TypeIdOf<Transform>());
    const QuantizationSettings quant{.PositionQuantum = 0.001f, .PositionExtent = 4096.0f};

    Transform baseline;
    baseline.Position = vec3(1.0f, 2.0f, 3.0f);
    baseline.Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    baseline.Scale = vec3(1.0f, 1.0f, 1.0f);
    const vector<u8> baseBytes = Serialize(&baseline, info, registry);

    // Only the position moves; rotation and scale are unchanged.
    Transform current = baseline;
    current.Position = vec3(1.5f, 2.0f, 3.0f);
    const vector<u8> curBytes = Serialize(&current, info, registry);

    vector<u8> full;
    EncodeComponentBody(full, info.Id, curBytes, {}, false, TypeIdOf<Transform>(), registry, quant);
    vector<u8> delta;
    EncodeComponentBody(delta, info.Id, curBytes, baseBytes, false, TypeIdOf<Transform>(), registry,
                        quant);

    // The delta (position only) is smaller than the full (all three leaves).
    CHECK(delta.size() < full.size());

    const auto decoded = RoundTrip<Transform>(delta, baseBytes, info, registry, quant);
    CHECK(glm::length(decoded.Position - current.Position) < 0.002f);
    CHECK(std::abs(glm::dot(decoded.Rotation, baseline.Rotation)) > 0.999f);
    CHECK(glm::length(decoded.Scale - baseline.Scale) < 1e-4f);
}

TEST_CASE("An unchanged Transform delta carries no spatial payload")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const TypeInfo& info = registry.Info(TypeIdOf<Transform>());
    const QuantizationSettings quant{};

    Transform t;
    t.Position = vec3(5.0f, 6.0f, 7.0f);
    const vector<u8> bytes = Serialize(&t, info, registry);

    vector<u8> delta;
    EncodeComponentBody(delta, info.Id, bytes, bytes, false, TypeIdOf<Transform>(), registry,
                        quant);

    // Tag + one presence byte, no leaf payload at all.
    CHECK(delta.size() <= 2u);
    const auto decoded = RoundTrip<Transform>(delta, bytes, info, registry, quant);
    CHECK(glm::length(decoded.Position - t.Position) < 0.002f);
}

TEST_CASE("A non-Transform component deltas field-wise, exact")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const TypeInfo& info = registry.Info(TypeIdOf<Light>());
    const QuantizationSettings quant{};

    Light baseline;
    baseline.Intensity = 1.0f;
    baseline.Color = vec3(1.0f, 1.0f, 1.0f);
    const vector<u8> baseBytes = Serialize(&baseline, info, registry);

    Light current = baseline;
    current.Intensity = 5.0f; // one field changes
    const vector<u8> curBytes = Serialize(&current, info, registry);

    vector<u8> full;
    EncodeComponentBody(full, info.Id, curBytes, {}, false, TypeIdOf<Transform>(), registry, quant);
    CHECK(static_cast<ComponentEncoding>(full[0]) == ComponentEncoding::ReflectFull);

    vector<u8> delta;
    EncodeComponentBody(delta, info.Id, curBytes, baseBytes, false, TypeIdOf<Transform>(), registry,
                        quant);
    CHECK(static_cast<ComponentEncoding>(delta[0]) == ComponentEncoding::ReflectDelta);
    CHECK(delta.size() < full.size());

    const auto decoded = RoundTrip<Light>(delta, baseBytes, info, registry, quant);
    CHECK(decoded.Intensity == doctest::Approx(5.0f));
    CHECK(glm::length(decoded.Color - baseline.Color) < 1e-5f);
}

TEST_CASE("A reflect delta with no baseline is a decode error, never a crash")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const TypeInfo& info = registry.Info(TypeIdOf<Light>());
    const QuantizationSettings quant{};

    Light a;
    a.Intensity = 1.0f;
    Light b;
    b.Intensity = 2.0f;
    vector<u8> delta;
    EncodeComponentBody(delta, info.Id, Serialize(&b, info, registry),
                        Serialize(&a, info, registry), false, TypeIdOf<Transform>(), registry,
                        quant);

    vector<u8> outBytes;
    const VoidResult decoded =
        DecodeComponentBody(delta, {}, info.Id, TypeIdOf<Transform>(), registry, quant, outBytes);
    CHECK_FALSE(decoded.has_value());
}

// ---- Integration: the ack-keyed delta stream over a server/client pair (quantization on) ----------

namespace
{
    AssetManager& FakeAssets()
    {
        alignas(16) static unsigned char bytes[64]{};
        return *reinterpret_cast<AssetManager*>(bytes);
    }

    struct DeltaWorld
    {
        TypeRegistry ServerTypes;
        TypeRegistry ClientTypes;
        Unique<Scene> Server;
        Unique<Scene> Client;
        ReplicationServer ReplServer;
        ReplicationClient ReplClient;
        Entity Pawn = Entity::Null;
        NetId PawnId = InvalidNetId;

        explicit DeltaWorld(const ReplicationServer::Settings& settings)
            : ReplServer(settings), ReplClient([](AssetId) -> Ref<Prefab> { return nullptr; })
        {
            RegisterBuiltinTypes(ServerTypes);
            RegisterBuiltinTypes(ClientTypes);
            Server = Scene::Create(ServerTypes);
            Client = Scene::Create(ClientTypes);
            ReplClient.SetQuantization(settings.Quantization);
            ReplServer.AddConnection(1);

            Server->SetChangeTick(0);
            Pawn = Server->CreateEntity();
            Server->Add<Transform>(Pawn);
            NetIdAllocator allocator;
            AssignServerNetIds(*Server, allocator);
            PawnId = Server->Get<NetIdentity>(Pawn).Id;
        }

        // One tick: advance the server pawn, generate its stream, apply it, ack the applied tick.
        void Step(u64 tick, const vec3& position, bool ack)
        {
            Server->SetChangeTick(tick);
            Server->Get<Transform>(Pawn).Position = position;

            u64 appliedTick = 0;
            for (const ReplicationMessage& message : ReplServer.Generate(1, *Server, tick))
            {
                if (message.Channel == Net::Channel::ReliableOrdered)
                {
                    ReplClient.ApplyReliable(message.Bytes, *Client, FakeAssets());
                }
                else
                {
                    const SnapshotApplyResult applied =
                        ReplClient.ApplySnapshot(message.Bytes, *Client);
                    appliedTick = applied.ServerTick;
                }
            }
            if (ack && appliedTick != 0)
            {
                ReplServer.Acknowledge(1, appliedTick);
            }
        }

        vec3 ClientPosition() const
        {
            const Entity local = ReplClient.Map().Lookup(PawnId);
            const auto& interp = Client->Get<RemoteInterpolation>(local);
            return interp.Samples.back().Position;
        }
    };
}

TEST_CASE("Quantized delta stream converges the client pose within the quantum")
{
    ReplicationServer::Settings settings;
    settings.SnapshotInterval = 1;
    settings.QuantizeSpatial = true;
    settings.Quantization =
        QuantizationSettings{.PositionQuantum = 0.001f, .PositionExtent = 4096.0f};
    settings.KeyframeInterval = 8;

    DeltaWorld world(settings);

    // Drive the pawn along a ramp with acks flowing, so baselines advance and deltas apply.
    for (u64 tick = 1; tick <= 40; ++tick)
    {
        world.Step(tick, vec3(static_cast<f32>(tick) * 0.1f, 2.0f, -3.0f), /*ack=*/true);
    }

    const vec3 expected(4.0f, 2.0f, -3.0f);
    CHECK(glm::length(world.ClientPosition() - expected) < 0.002f);
}

TEST_CASE("The delta stream heals after a window of dropped acks")
{
    ReplicationServer::Settings settings;
    settings.SnapshotInterval = 1;
    settings.QuantizeSpatial = true;
    settings.Quantization =
        QuantizationSettings{.PositionQuantum = 0.001f, .PositionExtent = 4096.0f};
    settings.KeyframeInterval = 4;

    DeltaWorld world(settings);

    // A window with no acks at all: the server keeps sending against the last (empty) baseline —
    // full records / keyframes ride through — so the client still converges.
    for (u64 tick = 1; tick <= 20; ++tick)
    {
        world.Step(tick, vec3(static_cast<f32>(tick) * 0.05f, 1.0f, 0.0f), /*ack=*/false);
    }
    CHECK(glm::length(world.ClientPosition() - vec3(1.0f, 1.0f, 0.0f)) < 0.002f);

    // Acks resume and the pawn keeps moving: baselines advance, deltas apply, still converged.
    for (u64 tick = 21; tick <= 40; ++tick)
    {
        world.Step(tick, vec3(1.0f + static_cast<f32>(tick - 20) * 0.05f, 1.0f, 0.0f),
                   /*ack=*/true);
    }
    CHECK(glm::length(world.ClientPosition() - vec3(2.0f, 1.0f, 0.0f)) < 0.002f);
}

TEST_CASE("Delta + quantization shrink the steady-state stream well below the full-record baseline")
{
    TypeRegistry serverTypes;
    RegisterBuiltinTypes(serverTypes);
    Unique<Scene> server = Scene::Create(serverTypes);
    server->SetChangeTick(0);

    // A field of moving entities — the steady-state workload the compression targets.
    constexpr int Count = 12;
    vector<Entity> entities;
    for (int i = 0; i < Count; ++i)
    {
        const Entity e = server->CreateEntity();
        server->Add<Transform>(e);
        entities.push_back(e);
    }
    NetIdAllocator allocator;
    AssignServerNetIds(*server, allocator);

    TypeRegistry clientTypes;
    RegisterBuiltinTypes(clientTypes);
    Unique<Scene> client = Scene::Create(clientTypes);
    ReplicationClient replClient([](AssetId) -> Ref<Prefab> { return nullptr; });
    const QuantizationSettings quant{.PositionQuantum = 0.001f, .PositionExtent = 4096.0f};
    replClient.SetQuantization(quant);

    ReplicationServer replServer(ReplicationServer::Settings{.SnapshotInterval = 1,
                                                             .QuantizeSpatial = true,
                                                             .Quantization = quant,
                                                             .KeyframeInterval = 32});
    replServer.AddConnection(1);

    usize compressedBytes = 0;
    usize baselineBytes = 0;
    for (u64 tick = 1; tick <= 120; ++tick)
    {
        server->SetChangeTick(tick);
        for (int i = 0; i < Count; ++i)
        {
            server->Get<Transform>(entities[i]).Position =
                vec3(static_cast<f32>(tick) * 0.05f + static_cast<f32>(i), 1.0f, 0.0f);
        }

        // The full self-describing snapshot is the planset-54 baseline (every dirty field, name-keyed).
        baselineBytes += EncodeSnapshot(*server, tick, /*sinceTick=*/0).size();

        u64 appliedTick = 0;
        for (const ReplicationMessage& message : replServer.Generate(1, *server, tick))
        {
            if (message.Channel == Net::Channel::ReliableOrdered)
            {
                replClient.ApplyReliable(message.Bytes, *client, FakeAssets());
            }
            else
            {
                compressedBytes += message.Bytes.size();
                appliedTick = replClient.ApplySnapshot(message.Bytes, *client).ServerTick;
            }
        }
        if (appliedTick != 0)
        {
            replServer.Acknowledge(1, appliedTick);
        }
    }

    // The delta + quantized wire lands the steady-state stream at a small fraction of the baseline
    // (only the moving position, quantized, rides each tick — not a full name-keyed Transform record).
    CHECK(compressedBytes > 0);
    CHECK(compressedBytes < baselineBytes / 2);
}
