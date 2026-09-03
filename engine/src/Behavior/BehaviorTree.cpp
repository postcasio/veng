#include <Veng/Behavior/BehaviorTree.h>

#include <Veng/Assert.h>
#include <Veng/Scene/SceneSystem.h>

#include <utility>

namespace Veng
{
    namespace
    {
        // A leaf's per-tick context differs from its parent's only in the random stream: every node
        // gets a fresh stream seeded from the agent seed and the node's index, so a draw is
        // reproducible on replay and identical across two agents sharing a seed, independent of how
        // many draws happened elsewhere in the tree.
        BehaviorContext LeafContext(const BehaviorContext& parent, Rng& stream)
        {
            return BehaviorContext{
                .Scene = parent.Scene,
                .Agent = parent.Agent,
                .Pawn = parent.Pawn,
                .Delta = parent.Delta,
                .Tick = parent.Tick,
                .Random = stream,
                .System = parent.System,
            };
        }
    }

    void BehaviorTree::ResetSubtree(const u32 index, vector<NodeSlot>& slots) const
    {
        slots[index] = NodeSlot{};
        for (const u32 child : m_Nodes[index].Children)
        {
            ResetSubtree(child, slots);
        }
    }

    Status BehaviorTree::TickNode(const u32 index, vector<NodeSlot>& slots, const u64 seed,
                                  const BehaviorContext& context) const
    {
        const Node& node = m_Nodes[index];
        NodeSlot& slot = slots[index];
        Status result = Status::Failure;

        switch (node.Kind)
        {
        case NodeKind::Sequence:
        {
            // Resume the running child on a re-tick; children before it already succeeded this run.
            const u32 start = slot.Active ? slot.Counter : 0;
            slot.Active = true;
            result = Status::Success;
            for (u32 i = start; i < node.Children.size(); ++i)
            {
                const Status child = TickNode(node.Children[i], slots, seed, context);
                if (child == Status::Running)
                {
                    slot.Counter = i;
                    result = Status::Running;
                    break;
                }
                if (child == Status::Failure)
                {
                    result = Status::Failure;
                    break;
                }
            }
            if (result != Status::Running)
            {
                slot.Active = false;
                slot.Counter = 0;
            }
            break;
        }
        case NodeKind::Selector:
        {
            // The mirror of Sequence: the first Success wins, a Failure advances to the next child.
            const u32 start = slot.Active ? slot.Counter : 0;
            slot.Active = true;
            result = Status::Failure;
            for (u32 i = start; i < node.Children.size(); ++i)
            {
                const Status child = TickNode(node.Children[i], slots, seed, context);
                if (child == Status::Running)
                {
                    slot.Counter = i;
                    result = Status::Running;
                    break;
                }
                if (child == Status::Success)
                {
                    result = Status::Success;
                    break;
                }
            }
            if (result != Status::Running)
            {
                slot.Active = false;
                slot.Counter = 0;
            }
            break;
        }
        case NodeKind::Parallel:
        {
            // Tick every child each tick; fail as soon as any child has failed, succeed only once
            // all have. A still-running child abandoned by a failing sibling is reset (no OnExit).
            bool anyRunning = false;
            bool anyFailure = false;
            for (const u32 child : node.Children)
            {
                const Status status = TickNode(child, slots, seed, context);
                if (status == Status::Failure)
                {
                    anyFailure = true;
                }
                else if (status == Status::Running)
                {
                    anyRunning = true;
                }
            }
            if (anyFailure)
            {
                result = Status::Failure;
                for (const u32 child : node.Children)
                {
                    ResetSubtree(child, slots);
                }
            }
            else
            {
                result = anyRunning ? Status::Running : Status::Success;
            }
            break;
        }
        case NodeKind::Inverter:
        {
            const Status child = TickNode(node.Children[0], slots, seed, context);
            result = child == Status::Running   ? Status::Running
                     : child == Status::Success ? Status::Failure
                                                : Status::Success;
            break;
        }
        case NodeKind::Succeeder:
        {
            const Status child = TickNode(node.Children[0], slots, seed, context);
            result = child == Status::Running ? Status::Running : Status::Success;
            break;
        }
        case NodeKind::Repeat:
        {
            const Status child = TickNode(node.Children[0], slots, seed, context);
            if (child == Status::Running)
            {
                result = Status::Running;
                break;
            }
            // The child completed one iteration, regardless of its result; count it and either
            // finish or restart the child subtree so it re-enters next tick.
            ++slot.Counter;
            if (node.Count != 0 && slot.Counter >= node.Count)
            {
                slot.Counter = 0;
                result = Status::Success;
            }
            else
            {
                ResetSubtree(node.Children[0], slots);
                result = Status::Running;
            }
            break;
        }
        case NodeKind::Until:
        {
            const Status child = TickNode(node.Children[0], slots, seed, context);
            if (child == Status::Running)
            {
                result = Status::Running;
            }
            else if (child == node.TargetStatus)
            {
                // Keep going while the child returns the watched status.
                ResetSubtree(node.Children[0], slots);
                result = Status::Running;
            }
            else
            {
                result = child;
            }
            break;
        }
        case NodeKind::Cooldown:
        {
            if (slot.Timer > 0.0f)
            {
                slot.Timer -= context.Delta;
                result = Status::Failure;
                break;
            }
            result = TickNode(node.Children[0], slots, seed, context);
            if (result == Status::Success)
            {
                slot.Timer = node.TimeA;
            }
            break;
        }
        case NodeKind::Wait:
        {
            if (!slot.Active)
            {
                slot.Timer = node.TimeA;
                slot.Active = true;
            }
            slot.Timer -= context.Delta;
            if (slot.Timer <= 0.0f)
            {
                slot.Active = false;
                result = Status::Success;
            }
            else
            {
                result = Status::Running;
            }
            break;
        }
        case NodeKind::WaitRandom:
        {
            if (!slot.Active)
            {
                Rng stream(HashCombine(seed, index));
                slot.Duration = stream.NextFloat(node.TimeA, node.TimeB);
                slot.Timer = slot.Duration;
                slot.Active = true;
            }
            slot.Timer -= context.Delta;
            if (slot.Timer <= 0.0f)
            {
                slot.Active = false;
                result = Status::Success;
            }
            else
            {
                result = Status::Running;
            }
            break;
        }
        case NodeKind::Condition:
        {
            Rng stream(HashCombine(seed, index));
            BehaviorContext leaf = LeafContext(context, stream);
            result = node.Predicate(leaf) ? Status::Success : Status::Failure;
            break;
        }
        case NodeKind::Leaf:
        {
            Rng stream(HashCombine(seed, index));
            BehaviorContext leaf = LeafContext(context, stream);
            if (!slot.Active)
            {
                node.Leaf->OnEnter(leaf);
                slot.Active = true;
            }
            result = node.Leaf->Tick(leaf);
            if (result != Status::Running)
            {
                node.Leaf->OnExit(leaf, result);
                slot.Active = false;
            }
            break;
        }
        }

        slot.Last = result;
        return result;
    }

    Status BehaviorTree::Tick(vector<NodeSlot>& slots, const u64 seed,
                              const BehaviorContext& context) const
    {
        if (m_Nodes.empty())
        {
            return Status::Success;
        }
        return TickNode(m_Root, slots, seed, context);
    }

    BehaviorTreeBuilder& BehaviorTreeBuilder::OpenComposite(const BehaviorTree::NodeKind kind)
    {
        BehaviorTree::Node node;
        node.Kind = kind;
        const u32 index = AddNode(std::move(node));
        m_Stack.push_back(Frame{.Index = index, .IsDecorator = false, .HasChild = false});
        return *this;
    }

    BehaviorTreeBuilder& BehaviorTreeBuilder::OpenDecorator(const BehaviorTree::NodeKind kind)
    {
        BehaviorTree::Node node;
        node.Kind = kind;
        const u32 index = AddNode(std::move(node));
        m_Stack.push_back(Frame{.Index = index, .IsDecorator = true, .HasChild = false});
        return *this;
    }

    u32 BehaviorTreeBuilder::AddNode(BehaviorTree::Node node)
    {
        const auto index = static_cast<u32>(m_Nodes.size());
        m_Nodes.push_back(std::move(node));
        Attach(index);
        return index;
    }

    void BehaviorTreeBuilder::Attach(const u32 index)
    {
        if (m_Stack.empty())
        {
            VE_ASSERT(!m_HasRoot, "BehaviorTreeBuilder: a second root node with no open parent");
            m_Root = index;
            m_HasRoot = true;
            return;
        }
        Frame& top = m_Stack.back();
        VE_ASSERT(!(top.IsDecorator && top.HasChild),
                  "BehaviorTreeBuilder: a decorator was given more than one child");
        m_Nodes[top.Index].Children.push_back(index);
        top.HasChild = true;
    }

    void BehaviorTreeBuilder::CloseCompletedDecorators()
    {
        // A finished child satisfies a wrapping decorator, which may in turn satisfy its own.
        while (!m_Stack.empty() && m_Stack.back().IsDecorator && m_Stack.back().HasChild)
        {
            m_Stack.pop_back();
        }
    }

    BehaviorTreeBuilder& BehaviorTreeBuilder::End()
    {
        VE_ASSERT(!m_Stack.empty() && !m_Stack.back().IsDecorator,
                  "BehaviorTreeBuilder: End() with no open composite");
        m_Stack.pop_back();
        CloseCompletedDecorators();
        return *this;
    }

    BehaviorTreeBuilder& BehaviorTreeBuilder::Repeat(const u32 count)
    {
        OpenDecorator(BehaviorTree::NodeKind::Repeat);
        m_Nodes[m_Stack.back().Index].Count = count;
        return *this;
    }

    BehaviorTreeBuilder& BehaviorTreeBuilder::Until(const Status status)
    {
        OpenDecorator(BehaviorTree::NodeKind::Until);
        m_Nodes[m_Stack.back().Index].TargetStatus = status;
        return *this;
    }

    BehaviorTreeBuilder& BehaviorTreeBuilder::Cooldown(const f32 seconds)
    {
        OpenDecorator(BehaviorTree::NodeKind::Cooldown);
        m_Nodes[m_Stack.back().Index].TimeA = seconds;
        return *this;
    }

    BehaviorTreeBuilder& BehaviorTreeBuilder::Leaf(Ref<BehaviorTask> task)
    {
        VE_ASSERT(task != nullptr, "BehaviorTreeBuilder: Leaf() with a null task");
        BehaviorTree::Node node;
        node.Kind = BehaviorTree::NodeKind::Leaf;
        node.Leaf = std::move(task);
        AddNode(std::move(node));
        CloseCompletedDecorators();
        return *this;
    }

    BehaviorTreeBuilder& BehaviorTreeBuilder::Wait(const f32 seconds)
    {
        BehaviorTree::Node node;
        node.Kind = BehaviorTree::NodeKind::Wait;
        node.TimeA = seconds;
        AddNode(std::move(node));
        CloseCompletedDecorators();
        return *this;
    }

    BehaviorTreeBuilder& BehaviorTreeBuilder::WaitRandom(const f32 minSeconds, const f32 maxSeconds)
    {
        BehaviorTree::Node node;
        node.Kind = BehaviorTree::NodeKind::WaitRandom;
        node.TimeA = minSeconds;
        node.TimeB = maxSeconds;
        AddNode(std::move(node));
        CloseCompletedDecorators();
        return *this;
    }

    BehaviorTreeBuilder& BehaviorTreeBuilder::Condition(function<bool(BehaviorContext&)> predicate)
    {
        VE_ASSERT(predicate != nullptr, "BehaviorTreeBuilder: Condition() with a null predicate");
        BehaviorTree::Node node;
        node.Kind = BehaviorTree::NodeKind::Condition;
        node.Predicate = std::move(predicate);
        AddNode(std::move(node));
        CloseCompletedDecorators();
        return *this;
    }

    Ref<BehaviorTree> BehaviorTreeBuilder::Build()
    {
        VE_ASSERT(m_HasRoot, "BehaviorTreeBuilder: Build() on an empty tree");
        VE_ASSERT(m_Stack.empty(), "BehaviorTreeBuilder: Build() with an unclosed composite");
        return Ref<BehaviorTree>(new BehaviorTree(std::move(m_Nodes), m_Root));
    }
}
