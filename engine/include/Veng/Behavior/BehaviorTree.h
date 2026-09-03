#pragma once

#include <Veng/Veng.h>
#include <Veng/Math/Random.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Scene;
    struct SystemContext;

    /// @brief The result a behaviour node reports each tick.
    ///
    /// The three-valued outcome every composite, decorator, and leaf returns: still working, done
    /// well, or done badly. Composites and decorators route on it (a Sequence stops at the first
    /// Failure; a Selector at the first Success), and a leaf BehaviorTask produces it.
    enum class Status : u8
    {
        /// @brief The node is still working and wants another tick.
        Running,
        /// @brief The node finished and succeeded.
        Success,
        /// @brief The node finished and failed.
        Failure,
    };

    /// @brief Everything a behaviour node reads and writes during one tick.
    ///
    /// Handed to every leaf the engine ticks. The ECS is the blackboard: a BehaviorTask reads and writes
    /// components on @ref Agent and @ref Pawn through @ref Scene, so the runtime introduces no
    /// second data model. There is deliberately no Blackboard type — cross-tick memory a task needs
    /// lives in its own component on the agent entity, which already has reflection, an inspector,
    /// serialisation, and replication.
    ///
    /// @ref Random is the node's own reproducible stream, seeded from the agent's seed and the
    /// node's position, so a decision keyed off it replays identically. @ref System exposes the
    /// tick's authority, replay, and debug state; a leaf whose Tick has an *external* side effect (a
    /// spawn, an audio one-shot) is responsible for gating it on `System.IsReplay`, since the engine
    /// re-ticks the tree on a reconciliation replay to re-derive intent.
    struct BehaviorContext
    {
        /// @brief The scene the agent and its pawn live in — the blackboard a task reads and writes.
        Scene& Scene;
        /// @brief The entity carrying the BehaviorAgent being ticked.
        Entity Agent = Entity::Null;
        /// @brief The pawn the agent acts through: its Possesses target, or the agent itself.
        Entity Pawn = Entity::Null;
        /// @brief Seconds since the previous tick.
        f32 Delta = 0.0f;
        /// @brief The fixed simulation tick number this tick advances.
        u64 Tick = 0;
        /// @brief The node's own reproducible random stream (seeded from the agent seed and node index).
        Rng& Random;
        /// @brief The per-tick engine services — authority, replay, and debug state.
        const SystemContext& System;
    };

    /// @brief A consumer-supplied leaf: the unit of game logic in a behaviour tree.
    ///
    /// A BehaviorTask is where a tree meets the game. The engine owns the composites, decorators, and the
    /// per-agent running state; the consumer subclasses BehaviorTask and puts its decision or action in
    /// @ref Tick, reading and writing the ECS through the BehaviorContext. One BehaviorTask instance is
    /// shared by every agent running the tree, so a BehaviorTask holds no per-agent mutable state: anything
    /// that must persist across ticks for one agent is kept in a component on the agent entity.
    ///
    /// @ref OnEnter runs on the tick the leaf first becomes active, @ref OnExit on the tick it
    /// finishes (returns a non-Running status); a leaf abandoned mid-run because a Parallel sibling
    /// completed does not receive an OnExit.
    class BehaviorTask
    {
    public:
        /// @brief Virtual destructor; tasks are held through Ref<BehaviorTask>.
        virtual ~BehaviorTask() = default;

        /// @brief Called on the tick the leaf becomes active, before the first Tick.
        ///
        /// The default does nothing.
        /// @param context  The per-tick blackboard and services.
        virtual void OnEnter(BehaviorContext& context) {}

        /// @brief Advances the leaf one tick and reports its status.
        /// @param context  The per-tick blackboard and services.
        /// @return Running to be ticked again, or Success/Failure to finish.
        [[nodiscard]] virtual Status Tick(BehaviorContext& context) = 0;

        /// @brief Called on the tick the leaf finishes, with the status it returned.
        ///
        /// The default does nothing. Not called for a leaf abandoned mid-run by a completing Parallel.
        /// @param context  The per-tick blackboard and services.
        /// @param status   The finishing status (Success or Failure).
        virtual void OnExit(BehaviorContext& context, Status status) {}
    };

    /// @brief One node's per-agent running state, indexed by the node's position in its tree.
    ///
    /// Two agents running one shared tree each own a vector of these, so their running positions,
    /// timers, and counts never collide. It is plain copyable data — a component holding it stays
    /// poolable — and every field is reset to its default when the node's subtree is restarted.
    struct NodeSlot
    {
        /// @brief The status the node reported on its last tick.
        Status Last = Status::Running;
        /// @brief Whether the node is currently mid-run (between becoming active and finishing).
        ///
        /// A Sequence/Selector uses it to know whether to resume; a Wait/WaitRandom to know whether
        /// its timer is already seeded; a BehaviorTask's active state drives its OnEnter/OnExit pairing.
        bool Active = false;
        /// @brief A countdown timer in seconds: a Wait/WaitRandom's remaining dwell, a Cooldown's remaining block.
        f32 Timer = 0.0f;
        /// @brief A WaitRandom's drawn dwell duration, held so the same seed yields the same wait.
        f32 Duration = 0.0f;
        /// @brief A general counter: a Sequence/Selector's resumed child index, or a Repeat's completed-iteration count.
        u32 Counter = 0;
    };

    /// @brief An immutable behaviour tree, built once and shared by every agent that runs it.
    ///
    /// The tree is a flat array of nodes carrying only the structure and authored parameters — no
    /// running state, which lives per agent in a vector of NodeSlot. A BehaviorTreeBuilder produces
    /// it with the nesting spelled as calls; it is then held through a Ref<BehaviorTree> and ticked
    /// against an agent's slots by @ref Tick. Because it is stateless it is freely shared and never
    /// copied per agent.
    ///
    /// The node kinds are three families: **composites** (Sequence stops at the first Failure,
    /// Selector at the first Success, Parallel ticks all children and succeeds on all / fails on
    /// any), **decorators** wrapping one child (Inverter swaps Success and Failure, Succeeder maps
    /// any finish to Success, Repeat re-runs a child n times or forever, Until re-runs while the
    /// child returns a given status, Cooldown blocks a child for a time after it succeeds), and
    /// **leaves** (a consumer BehaviorTask, a Wait/WaitRandom dwell timer, a Condition predicate over the ECS).
    class BehaviorTree
    {
    public:
        /// @brief Returns the number of nodes, which is the size an agent's slot vector must have.
        /// @return The node count.
        [[nodiscard]] usize NodeCount() const { return m_Nodes.size(); }

        /// @brief Ticks the tree's root once against one agent's slots and returns its status.
        ///
        /// The slot vector must have @ref NodeCount entries (the caller sizes it once). @p seed is
        /// the agent's seed, mixed with each node's index to seed that node's reproducible stream.
        /// @param slots    The agent's per-node running state, sized to NodeCount.
        /// @param seed     The agent's seed for its random streams.
        /// @param context  The per-tick blackboard and services for the root.
        /// @return The root node's status this tick.
        Status Tick(vector<NodeSlot>& slots, u64 seed, const BehaviorContext& context) const;

    private:
        friend class BehaviorTreeBuilder;

        /// @brief How a node behaves; selects the branch in the tick walk.
        enum class NodeKind : u8
        {
            Sequence,
            Selector,
            Parallel,
            Inverter,
            Succeeder,
            Repeat,
            Until,
            Cooldown,
            Wait,
            WaitRandom,
            Condition,
            Leaf,
        };

        /// @brief One immutable node: its kind, its children, and its authored parameters.
        struct Node
        {
            /// @brief The node's behaviour.
            NodeKind Kind = NodeKind::Sequence;
            /// @brief Child node indices — many for a composite, one for a decorator, none for a leaf.
            vector<u32> Children;
            /// @brief Primary time parameter: a Wait's dwell, a WaitRandom's minimum, a Cooldown's block.
            f32 TimeA = 0.0f;
            /// @brief Secondary time parameter: a WaitRandom's maximum.
            f32 TimeB = 0.0f;
            /// @brief A Repeat's iteration count; zero means repeat forever.
            u32 Count = 0;
            /// @brief An Until's target status — the child result it repeats while it sees.
            Status TargetStatus = Status::Failure;
            /// @brief A Leaf's shared task instance.
            Ref<BehaviorTask> Leaf;
            /// @brief A Condition's predicate over the ECS.
            function<bool(BehaviorContext&)> Predicate;
        };

        /// @brief Constructs the tree from its finished node array and root index.
        BehaviorTree(vector<Node> nodes, u32 root) : m_Nodes(std::move(nodes)), m_Root(root) {}

        /// @brief Ticks one node (and, recursively, its subtree) against the agent's slots.
        [[nodiscard]] Status TickNode(u32 index, vector<NodeSlot>& slots, u64 seed,
                                      const BehaviorContext& context) const;

        /// @brief Resets a node's subtree to its default slots, so restarting it re-enters cleanly.
        void ResetSubtree(u32 index, vector<NodeSlot>& slots) const;

        /// @brief The flat node array; index 0 is not special, @ref m_Root names the entry point.
        vector<Node> m_Nodes;
        /// @brief The root node's index.
        u32 m_Root = 0;
    };

    /// @brief Builds a BehaviorTree with its nesting spelled as chained calls.
    ///
    /// Composites open a scope closed by @ref End; decorators wrap the single node that follows and
    /// close themselves once it is complete; leaves attach and close immediately. So a tree reads as
    /// its own shape:
    ///
    /// @code
    /// Ref<BehaviorTree> tree = BehaviorTreeBuilder()
    ///     .Repeat()                                   // forever
    ///         .Sequence()
    ///             .Leaf(CreateRef<MoveToTask>(pointA))
    ///             .Wait(2.0f)
    ///             .Leaf(CreateRef<MoveToTask>(pointB))
    ///             .Wait(2.0f)
    ///         .End()
    ///     .Build();
    /// @endcode
    ///
    /// A builder builds one tree: @ref Build consumes its node array, so it is not reused afterward.
    class BehaviorTreeBuilder
    {
    public:
        /// @brief Opens a Sequence: ticks children in order, stopping at the first Failure.
        /// @return This builder.
        BehaviorTreeBuilder& Sequence() { return OpenComposite(BehaviorTree::NodeKind::Sequence); }

        /// @brief Opens a Selector: ticks children in order, stopping at the first Success.
        /// @return This builder.
        BehaviorTreeBuilder& Selector() { return OpenComposite(BehaviorTree::NodeKind::Selector); }

        /// @brief Opens a Parallel: ticks every child each tick; succeeds on all, fails on any.
        /// @return This builder.
        BehaviorTreeBuilder& Parallel() { return OpenComposite(BehaviorTree::NodeKind::Parallel); }

        /// @brief Closes the innermost open composite.
        /// @return This builder.
        BehaviorTreeBuilder& End();

        /// @brief Wraps the next node in an Inverter: swaps its Success and Failure.
        /// @return This builder.
        BehaviorTreeBuilder& Inverter() { return OpenDecorator(BehaviorTree::NodeKind::Inverter); }

        /// @brief Wraps the next node in a Succeeder: maps any finish to Success.
        /// @return This builder.
        BehaviorTreeBuilder& Succeeder()
        {
            return OpenDecorator(BehaviorTree::NodeKind::Succeeder);
        }

        /// @brief Wraps the next node in a Repeat: re-runs it @p count times, or forever when 0.
        /// @param count  The number of iterations, or 0 to repeat forever.
        /// @return This builder.
        BehaviorTreeBuilder& Repeat(u32 count = 0);

        /// @brief Wraps the next node in an Until: re-runs it while it returns @p status.
        /// @param status  The child result the decorator repeats while it sees; any other finish ends it.
        /// @return This builder.
        BehaviorTreeBuilder& Until(Status status);

        /// @brief Wraps the next node in a Cooldown: blocks it (returns Failure) for @p seconds after it succeeds.
        /// @param seconds  The cooldown duration.
        /// @return This builder.
        BehaviorTreeBuilder& Cooldown(f32 seconds);

        /// @brief Adds a leaf running the given BehaviorTask.
        /// @param task  The shared task instance every agent on this tree runs.
        /// @return This builder.
        BehaviorTreeBuilder& Leaf(Ref<BehaviorTask> task);

        /// @brief Adds a leaf that returns Running for @p seconds, then Success once.
        /// @param seconds  The dwell duration.
        /// @return This builder.
        BehaviorTreeBuilder& Wait(f32 seconds);

        /// @brief Adds a leaf that waits a random duration in [@p minSeconds, @p maxSeconds), drawn from the node's seeded stream.
        /// @param minSeconds  Inclusive lower bound of the dwell.
        /// @param maxSeconds  Exclusive upper bound of the dwell.
        /// @return This builder.
        BehaviorTreeBuilder& WaitRandom(f32 minSeconds, f32 maxSeconds);

        /// @brief Adds a leaf that returns Success when @p predicate holds and Failure otherwise — a perception check over the ECS.
        /// @param predicate  The test, evaluated against the tick's blackboard.
        /// @return This builder.
        BehaviorTreeBuilder& Condition(function<bool(BehaviorContext&)> predicate);

        /// @brief Finalises and returns the tree; the builder is spent afterward.
        /// @return The immutable, shared tree.
        [[nodiscard]] Ref<BehaviorTree> Build();

    private:
        /// @brief A builder stack entry: an open composite, or a decorator awaiting its one child.
        struct Frame
        {
            u32 Index = 0;
            bool IsDecorator = false;
            bool HasChild = false;
        };

        BehaviorTreeBuilder& OpenComposite(BehaviorTree::NodeKind kind);
        BehaviorTreeBuilder& OpenDecorator(BehaviorTree::NodeKind kind);
        u32 AddNode(BehaviorTree::Node node);
        void Attach(u32 index);
        void CloseCompletedDecorators();

        vector<BehaviorTree::Node> m_Nodes;
        vector<Frame> m_Stack;
        u32 m_Root = 0;
        bool m_HasRoot = false;
    };
}
