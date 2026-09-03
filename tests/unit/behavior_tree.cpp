// The behaviour runtime: the composites and decorators of a BehaviorTree, the seeded per-agent
// slots two agents on one tree keep apart, and the AI arm of the control pipeline end to end — a
// BehaviorAgent whose leaf writes Intent driving a pawn through the real MovementSystem, identically
// to a raw Intent write. Pure CPU — no Context, no Vulkan — in the control_movement.cpp mould: a
// real Scene over RegisterBuiltinTypes and a headless SystemContext.

#include <doctest/doctest.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>

#include <Veng/Behavior/BehaviorAgent.h>
#include <Veng/Behavior/BehaviorSystem.h>
#include <Veng/Behavior/BehaviorTree.h>
#include <Veng/Input.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Movement.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>

using namespace Veng;

namespace
{
    bool VecApprox(const vec3& a, const vec3& b, const f32 eps = 1e-4f)
    {
        return glm::all(glm::lessThan(glm::abs(a - b), vec3(eps)));
    }

    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);
        return registry;
    }

    // A SystemContext over a real headless Input (all-zeros) and never-dereferenced asset storage —
    // the behaviour tick reads only Delta/Tick/authority/Debug from it.
    struct ContextStorage
    {
        Input HeadlessInput{nullptr};
        alignas(16) unsigned char AssetsBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = HeadlessInput,
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
                .Audio = *reinterpret_cast<Audio::AudioEngine*>(TasksBytes),
            };
        }
    };

    // A leaf that returns a fixed status and tallies its enter/tick/exit calls, so a composite's
    // routing over it is observable.
    struct CountingTask final : BehaviorTask
    {
        Status Result = Status::Success;
        int Enters = 0;
        int Ticks = 0;
        int Exits = 0;
        Status LastExit = Status::Running;

        void OnEnter(BehaviorContext&) override { ++Enters; }
        Status Tick(BehaviorContext&) override
        {
            ++Ticks;
            return Result;
        }
        void OnExit(BehaviorContext&, const Status status) override
        {
            ++Exits;
            LastExit = status;
        }
    };

    // A leaf returning a scripted sequence of statuses (holding the last once exhausted), so a
    // running-then-finishing child can be staged tick by tick.
    struct ScriptTask final : BehaviorTask
    {
        vector<Status> Script;
        usize Index = 0;
        int Ticks = 0;

        Status Tick(BehaviorContext&) override
        {
            const Status status = Script[std::min(Index, Script.size() - 1)];
            ++Index;
            ++Ticks;
            return status;
        }
    };

    // The AI-producer leaf: writes the pawn's Intent each tick and stays Running, so the pawn moves
    // through the MovementSystem exactly as a player-driven one does.
    struct SetMoveTask final : BehaviorTask
    {
        vec3 Move{0.0f};

        Status Tick(BehaviorContext& context) override
        {
            context.Scene.Get<Intent>(context.Pawn).Move = Move;
            return Status::Running;
        }
    };

    // Ticks a tree once against a caller-owned slot vector (sized on first use), over a headless
    // context whose Agent and Pawn are one throwaway entity.
    struct Harness
    {
        TypeRegistry Registry = MakeRegistry();
        Unique<Scene> World = Scene::Create(Registry);
        Entity Self = World->CreateEntity();
        ContextStorage Storage;

        Status Tick(const Ref<BehaviorTree>& tree, vector<NodeSlot>& slots, const u64 seed,
                    const f32 delta)
        {
            if (slots.size() != tree->NodeCount())
            {
                slots.assign(tree->NodeCount(), NodeSlot{});
            }
            Rng random(seed);
            const SystemContext system = Storage.Make();
            const BehaviorContext context{
                .Scene = *World,
                .Agent = Self,
                .Pawn = Self,
                .Delta = delta,
                .Tick = 0,
                .Random = random,
                .System = system,
            };
            return tree->Tick(slots, seed, context);
        }
    };
}

TEST_CASE("Sequence stops at the first failure and never ticks past it")
{
    Harness harness;
    auto a = CreateRef<CountingTask>();
    auto b = CreateRef<CountingTask>();
    auto c = CreateRef<CountingTask>();
    a->Result = Status::Success;
    b->Result = Status::Failure;
    c->Result = Status::Success;

    const Ref<BehaviorTree> tree =
        BehaviorTreeBuilder().Sequence().Leaf(a).Leaf(b).Leaf(c).End().Build();

    vector<NodeSlot> slots;
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Failure);
    CHECK(a->Ticks == 1);
    CHECK(b->Ticks == 1);
    CHECK(c->Ticks == 0); // the failure stops the sequence before its last child
}

TEST_CASE("Sequence resumes a Running child at the same child next tick")
{
    Harness harness;
    auto a = CreateRef<CountingTask>();
    auto b = CreateRef<ScriptTask>();
    a->Result = Status::Success;
    b->Script = {Status::Running, Status::Running, Status::Success};

    const Ref<BehaviorTree> tree = BehaviorTreeBuilder().Sequence().Leaf(a).Leaf(b).End().Build();

    vector<NodeSlot> slots;
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Running);
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Running);
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Success);

    // The first child succeeded once and was not re-ticked while the second ran.
    CHECK(a->Ticks == 1);
    CHECK(b->Ticks == 3);
}

TEST_CASE("Selector stops at the first success and mirrors Sequence")
{
    Harness harness;
    auto a = CreateRef<CountingTask>();
    auto b = CreateRef<CountingTask>();
    auto c = CreateRef<CountingTask>();
    a->Result = Status::Failure;
    b->Result = Status::Success;
    c->Result = Status::Success;

    const Ref<BehaviorTree> tree =
        BehaviorTreeBuilder().Selector().Leaf(a).Leaf(b).Leaf(c).End().Build();

    vector<NodeSlot> slots;
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Success);
    CHECK(a->Ticks == 1);
    CHECK(b->Ticks == 1);
    CHECK(c->Ticks == 0); // the success stops the selector
}

TEST_CASE("Parallel ticks every child and succeeds on all, fails on any")
{
    Harness harness;

    SUBCASE("succeeds only once every child has")
    {
        auto a = CreateRef<CountingTask>();
        auto b = CreateRef<ScriptTask>();
        a->Result = Status::Success;
        b->Script = {Status::Running, Status::Running, Status::Success};

        const Ref<BehaviorTree> tree =
            BehaviorTreeBuilder().Parallel().Leaf(a).Leaf(b).End().Build();

        vector<NodeSlot> slots;
        CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Running);
        CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Running);
        CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Success);
        CHECK(a->Ticks == 3); // every child is ticked every tick
    }

    SUBCASE("fails as soon as any child fails")
    {
        auto a = CreateRef<CountingTask>();
        auto b = CreateRef<CountingTask>();
        a->Result = Status::Success;
        b->Result = Status::Failure;

        const Ref<BehaviorTree> tree =
            BehaviorTreeBuilder().Parallel().Leaf(a).Leaf(b).End().Build();

        vector<NodeSlot> slots;
        CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Failure);
    }
}

TEST_CASE("Inverter swaps Success and Failure and passes Running through")
{
    Harness harness;
    auto child = CreateRef<CountingTask>();
    const Ref<BehaviorTree> tree = BehaviorTreeBuilder().Inverter().Leaf(child).Build();

    vector<NodeSlot> slots;
    child->Result = Status::Success;
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Failure);
    child->Result = Status::Failure;
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Success);
    child->Result = Status::Running;
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Running);
}

TEST_CASE("Succeeder maps any finish to Success")
{
    Harness harness;
    auto child = CreateRef<CountingTask>();
    const Ref<BehaviorTree> tree = BehaviorTreeBuilder().Succeeder().Leaf(child).Build();

    vector<NodeSlot> slots;
    child->Result = Status::Failure;
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Success);
    child->Result = Status::Running;
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Running);
}

TEST_CASE("Repeat re-runs its child a fixed number of times, then succeeds")
{
    Harness harness;
    auto child = CreateRef<CountingTask>();
    child->Result = Status::Success;
    const Ref<BehaviorTree> tree = BehaviorTreeBuilder().Repeat(3).Leaf(child).Build();

    vector<NodeSlot> slots;
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Running);
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Running);
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Success);
    CHECK(child->Ticks == 3);
}

TEST_CASE("Until repeats while its child returns the watched status")
{
    Harness harness;
    auto child = CreateRef<ScriptTask>();
    child->Script = {Status::Failure, Status::Failure, Status::Success};
    // Repeat while Failure — a retry-until-success guard.
    const Ref<BehaviorTree> tree = BehaviorTreeBuilder().Until(Status::Failure).Leaf(child).Build();

    vector<NodeSlot> slots;
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Running);
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Running);
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Success);
}

TEST_CASE("Cooldown blocks its child for the cooldown after it succeeds")
{
    Harness harness;
    auto child = CreateRef<CountingTask>();
    child->Result = Status::Success;
    const Ref<BehaviorTree> tree = BehaviorTreeBuilder().Cooldown(1.0f).Leaf(child).Build();

    vector<NodeSlot> slots;
    CHECK(harness.Tick(tree, slots, 0, 0.5f) == Status::Success); // runs, then arms the cooldown
    CHECK(child->Ticks == 1);
    CHECK(harness.Tick(tree, slots, 0, 0.5f) == Status::Failure); // cooling down: child not ticked
    CHECK(child->Ticks == 1);
    CHECK(harness.Tick(tree, slots, 0, 0.5f) == Status::Failure); // still cooling
    CHECK(child->Ticks == 1);
    CHECK(harness.Tick(tree, slots, 0, 0.5f) == Status::Success); // cooldown elapsed: runs again
    CHECK(child->Ticks == 2);
}

TEST_CASE("Condition reports Success or Failure from a predicate over the blackboard")
{
    Harness harness;
    bool gate = false;
    const Ref<BehaviorTree> tree =
        BehaviorTreeBuilder().Condition([&gate](BehaviorContext&) { return gate; }).Build();

    vector<NodeSlot> slots;
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Failure);
    gate = true;
    CHECK(harness.Tick(tree, slots, 0, 0.016f) == Status::Success);
}

TEST_CASE("Wait returns Running for about two seconds at 60 Hz, then Success once")
{
    Harness harness;
    const Ref<BehaviorTree> tree = BehaviorTreeBuilder().Wait(2.0f).Build();

    vector<NodeSlot> slots;
    int running = 0;
    int successTick = -1;
    for (int i = 1; i <= 200; ++i)
    {
        const Status status = harness.Tick(tree, slots, 0, 1.0f / 60.0f);
        if (status == Status::Running)
        {
            ++running;
        }
        else
        {
            successTick = i;
            break;
        }
    }
    // 2 s at 60 Hz is 120 ticks; the exact tick is bounded, not pinned, against float accumulation.
    CHECK(successTick >= 119);
    CHECK(successTick <= 121);
    CHECK(running == successTick - 1);
}

TEST_CASE("WaitRandom draws inside its bounds, reproducibly per seed")
{
    Harness harness;
    const Ref<BehaviorTree> tree = BehaviorTreeBuilder().WaitRandom(1.0f, 3.0f).Build();

    vector<NodeSlot> agentA;
    vector<NodeSlot> agentB;
    vector<NodeSlot> agentC;

    // Enter each agent's WaitRandom once so it draws its dwell into the slot.
    harness.Tick(tree, agentA, 0x1234u, 0.016f);
    harness.Tick(tree, agentB, 0x1234u, 0.016f);
    harness.Tick(tree, agentC, 0x9999u, 0.016f);

    CHECK(agentA[0].Duration >= 1.0f);
    CHECK(agentA[0].Duration < 3.0f);

    // The same seed on the same tree draws the same dwell; a different seed draws a different one.
    CHECK(agentA[0].Duration == doctest::Approx(agentB[0].Duration));
    CHECK(agentA[0].Duration != doctest::Approx(agentC[0].Duration));
}

TEST_CASE("Two agents on one tree keep independent slots")
{
    Harness harness;
    auto child = CreateRef<CountingTask>();
    child->Result = Status::Success;
    // A forever Repeat counts one completed iteration per tick into its slot.
    const Ref<BehaviorTree> tree = BehaviorTreeBuilder().Repeat().Leaf(child).Build();

    vector<NodeSlot> agentA;
    vector<NodeSlot> agentB;
    for (int i = 0; i < 5; ++i)
    {
        harness.Tick(tree, agentA, 0, 0.016f);
    }
    for (int i = 0; i < 3; ++i)
    {
        harness.Tick(tree, agentB, 0, 0.016f);
    }

    // Each agent's Repeat count is its own — one's ticks never move the other's.
    CHECK(agentA[0].Counter == 5);
    CHECK(agentB[0].Counter == 3);
}

TEST_CASE("The AI arm: a BehaviorAgent's leaf drives its possessed pawn through MovementSystem")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // A pawn the agent possesses, and a second pawn driven by a raw Intent write — the two must
    // move identically, the claim control_movement.cpp makes for a hand-rolled AI, now against the
    // shipped runtime.
    const Entity pawn = scene->CreateEntity();
    scene->Add<Transform>(pawn, Transform{});
    scene->Add<Intent>(pawn, Intent{});
    scene->Add<Mover>(pawn, Mover{.MoveSpeed = 2.0f, .TurnSpeed = 1.0f});

    const Entity control = scene->CreateEntity();
    scene->Add<Transform>(control, Transform{});
    scene->Add<Intent>(control, Intent{.Move = vec3(0.0f, 0.0f, 1.0f)});
    scene->Add<Mover>(control, Mover{.MoveSpeed = 2.0f, .TurnSpeed = 1.0f});

    auto move = CreateRef<SetMoveTask>();
    move->Move = vec3(0.0f, 0.0f, 1.0f);
    const Ref<BehaviorTree> tree = BehaviorTreeBuilder().Leaf(move).Build();

    const Entity agent = scene->CreateEntity();
    scene->Add<Possesses>(agent, Possesses{.Pawn = pawn});
    scene->Add<BehaviorAgent>(agent, BehaviorAgent{.Tree = tree, .Seed = 7});

    BehaviorSystem behavior;
    MovementSystem movement;
    ContextStorage storage;

    behavior.OnUpdate(*scene, 1.0f, storage.Make());
    movement.OnUpdate(*scene, 1.0f, storage.Make());

    // The agent wrote the pawn's Intent; the movement system integrated it identically to the raw
    // producer: (0,0,1) at speed 2 over 1 s.
    CHECK(VecApprox(scene->Get<Transform>(pawn).Position, vec3(0.0f, 0.0f, 2.0f)));
    CHECK(VecApprox(scene->Get<Transform>(pawn).Position, scene->Get<Transform>(control).Position));
}

TEST_CASE("An agent with no Possesses acts on itself")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity agent = scene->CreateEntity();
    scene->Add<Transform>(agent, Transform{});
    scene->Add<Intent>(agent, Intent{});
    scene->Add<Mover>(agent, Mover{.MoveSpeed = 3.0f, .TurnSpeed = 1.0f});

    auto move = CreateRef<SetMoveTask>();
    move->Move = vec3(1.0f, 0.0f, 0.0f);
    const Ref<BehaviorTree> tree = BehaviorTreeBuilder().Leaf(move).Build();
    scene->Add<BehaviorAgent>(agent, BehaviorAgent{.Tree = tree, .Seed = 1});

    BehaviorSystem behavior;
    MovementSystem movement;
    ContextStorage storage;

    behavior.OnUpdate(*scene, 1.0f, storage.Make());
    movement.OnUpdate(*scene, 1.0f, storage.Make());

    // Pawn resolved to the agent itself: (1,0,0) at speed 3 over 1 s.
    CHECK(VecApprox(scene->Get<Transform>(agent).Position, vec3(3.0f, 0.0f, 0.0f)));
}

TEST_CASE("A Remote-tier agent is not ticked")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    const Entity agent = scene->CreateEntity();
    scene->Add<Transform>(agent, Transform{});
    scene->Add<Intent>(agent, Intent{});
    scene->Add<Authority>(agent, Authority{.Tier = Tier::Remote});

    auto move = CreateRef<SetMoveTask>();
    move->Move = vec3(0.0f, 0.0f, 1.0f);
    const Ref<BehaviorTree> tree = BehaviorTreeBuilder().Leaf(move).Build();
    scene->Add<BehaviorAgent>(agent, BehaviorAgent{.Tree = tree, .Seed = 1});

    BehaviorSystem behavior;
    ContextStorage storage;
    behavior.OnUpdate(*scene, 1.0f, storage.Make());

    // The authority filter skipped the agent, so its tree never ran and its Intent is untouched.
    CHECK(VecApprox(scene->Get<Intent>(agent).Move, vec3(0.0f)));
}
