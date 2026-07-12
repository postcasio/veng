// Packed input codec: the context-keyed bit-packed PlayerInput wire form and its negotiated
// fallback. Round-trips each action shape, verifies the context-stack hash is order-sensitive, and
// that a hash mismatch (a mid-flight context switch) is a decode error the caller falls back on.

#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include <Veng/Asset/AssetId.h>
#include <Veng/Net/Replication.h>

#include <array>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    constexpr ActionId Move{0x10};
    constexpr ActionId Aim{0x11};
    constexpr ActionId Jump{0x12};

    const vector<PackedInputAction> Schema = {
        PackedInputAction{.Id = Move, .Kind = ActionKind::Axis2D},
        PackedInputAction{.Id = Aim, .Kind = ActionKind::Axis1D},
        PackedInputAction{.Id = Jump, .Kind = ActionKind::Button},
    };

    ActionState SampleState()
    {
        ActionState state;
        state.Actions = {
            ActionSample{.Id = Move, .Value = vec2(0.5f, -0.25f), .Phase = ActionPhase::Ongoing},
            ActionSample{.Id = Jump, .Value = vec2(1.0f, 0.0f), .Phase = ActionPhase::Started},
        };
        return state;
    }
}

TEST_CASE("Packed action state round-trips each shape within the 8-bit axis quantum")
{
    const ActionState state = SampleState();
    const vector<u8> packed = EncodePackedActionState(state, Schema);
    const ActionState decoded = DecodePackedActionState(packed, Schema);

    // Move (present, 2D) and Jump (present, button) come back; Aim (absent) stays absent.
    CHECK(decoded.Actions.size() == 2);
    CHECK(glm::length(decoded.GetValue(Move) - vec2(0.5f, -0.25f)) < 0.01f);
    CHECK(decoded.GetValue(Jump).x == doctest::Approx(1.0f));
    CHECK(decoded.WasTriggered(Jump));          // phase Started preserved
    CHECK(decoded.GetValue(Aim) == vec2(0.0f)); // absent → zero
}

TEST_CASE("A packed record is a handful of bytes for a three-action schema")
{
    // The reflection form carries name-keyed self-describing records per sample; the packed form is
    // presence + phase + value bits, so a full three-action state fits in a couple of bytes.
    const vector<u8> packed = EncodePackedActionState(SampleState(), Schema);
    CHECK(packed.size() <= 6u);
}

TEST_CASE("The context-stack hash is order-sensitive")
{
    const AssetId a{0xAAAA};
    const AssetId b{0xBBBB};
    const AssetId forward[] = {a, b};
    const AssetId reverse[] = {b, a};
    CHECK(HashContextStack(forward) != HashContextStack(reverse));
    CHECK(HashContextStack(forward) == HashContextStack(forward));
}

TEST_CASE("A packed input packet round-trips when the context hash matches")
{
    const AssetId contexts[] = {AssetId{0x1}, AssetId{0x2}};
    const u64 hash = HashContextStack(contexts);

    const ActionState records[] = {SampleState(), SampleState()};
    const vector<u8> packet = EncodePackedInputPacket(7, hash, 100, records, Schema);

    const Result<InputPacket> decoded = DecodePackedInputPacket(packet, hash, Schema);
    REQUIRE(decoded.has_value());
    CHECK(decoded->AckedServerTick == 7);
    REQUIRE(decoded->Inputs.size() == 2);
    CHECK(decoded->Inputs[0].ClientTick == 100);
    CHECK(decoded->Inputs[1].ClientTick == 101);
    CHECK(glm::length(decoded->Inputs[0].State.GetValue(Move) - vec2(0.5f, -0.25f)) < 0.01f);
}

TEST_CASE("A context-hash mismatch is a decode error — the reflection-form fallback")
{
    const u64 senderHash = HashContextStack(std::array{AssetId{0x1}, AssetId{0x2}});
    const u64 receiverHash = HashContextStack(std::array{AssetId{0x9}});

    const ActionState records[] = {SampleState()};
    const vector<u8> packet = EncodePackedInputPacket(1, senderHash, 5, records, Schema);

    const Result<InputPacket> decoded = DecodePackedInputPacket(packet, receiverHash, Schema);
    CHECK_FALSE(decoded.has_value());
}
