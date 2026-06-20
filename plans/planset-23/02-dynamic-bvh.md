# Plan 02 — The BVH

**Goal:** the spatial structure itself — a **bounding volume hierarchy** over world-space `AABB`s
with a `Build` from a leaf array and a frustum `Query`, beside `AABB` and `Frustum`. Pure,
device-free, glm-only. Its correctness is an equivalence property — `Query` returns exactly the
linear `Intersects` scan's result — plus a tree-validity invariant, provable with no ICD.
Independent of Plan 01.

## What lands

### `BVH` ([engine/include/Veng/Math/BVH.h](../../engine/include/Veng/Math/BVH.h), new + src)

A third bounds primitive beside `AABB.h` and `Frustum.h` — glm-only, no ownership rule, inside the
public/backend include-hygiene split (`include_hygiene` compiles it against glm/fmt only). The
broadphase rebuilds it from scratch whenever the scene's spatial version moves (Plan 03), so the
structure is **build + query**; per-object incremental maintenance is a named refinement.

```cpp
namespace Veng
{
    // A bounding volume hierarchy over world-space AABBs. Build() constructs the
    // tree from a leaf array (each leaf a tight box plus a caller u32 id);
    // Query() returns the ids of the leaves a frustum touches — exactly a linear
    // Intersects scan of the tight boxes, since the accept test is per-leaf and
    // tight. A node's box encloses its children's. Device-free value type;
    // rebuilt from scratch when the source set changes.
    class BVH
    {
    public:
        struct Leaf { AABB Box; u32 Id; }; // tight world box + caller payload

        // Build the tree over `leaves` (replacing any prior contents). O(N log N)
        // top-down: recursively split the set by the surface-area-minimizing
        // partition along the longest centroid axis. Empty input yields an empty
        // tree.
        void Build(std::span<const Leaf> leaves);

        // Append the ids of every leaf whose box passes Intersects(frustum,.) to
        // `out` (NOT cleared). Descends internal nodes by their enclosing box,
        // tests leaves by their tight box.
        void Query(const Frustum& frustum, vector<u32>& out) const;

        [[nodiscard]] u32  GetLeafCount() const { return m_LeafCount; }
        [[nodiscard]] u32  GetNodeCount() const { return static_cast<u32>(m_Nodes.size()); }
        [[nodiscard]] i32  GetHeight()    const; // root height; 0 when empty or one leaf
        [[nodiscard]] AABB GetRootBounds() const; // box of everything (Empty when no leaves)

    private:
        static constexpr i32 NullNode = -1;

        struct Node
        {
            AABB Box;            // enclosing (leaf: the tight box)
            i32  Child1, Child2; // internal; both NullNode on a leaf
            u32  Id;             // leaf payload (unused on internal)
            [[nodiscard]] bool IsLeaf() const { return Child1 == NullNode; }
        };

        i32 BuildRange(std::span<Leaf> leaves); // recursive; returns the subtree root

        vector<Node> m_Nodes;
        i32 m_Root      = NullNode;
        u32 m_LeafCount = 0;
    };
}
```

### Build — top-down surface-area split

`Build` copies the leaves into scratch and calls `BuildRange` over the whole span. `BuildRange`:
a single leaf becomes a leaf node; otherwise it computes the centroid bounds, splits the range along
its **longest axis** at the partition that minimizes the summed child surface area (a small bucket
SAH sweep, or the median as the degenerate fallback when centroids coincide), recurses on each half,
and creates an internal node whose `Box` is the union of the two child boxes. Nodes live in one
`vector<Node>` appended in build order — no per-node allocation, and child links are `vector`
indices. The result is a fresh, near-balanced tree every build.

### Query — descend by node box, accept by tight leaf box

`Query` walks the nodes from the root on a small explicit stack: an internal node is descended iff
`Intersects(frustum, node.Box)`, a leaf is appended iff `Intersects(frustum, node.Box)` (a leaf's
box *is* its tight box). Two-state (reject-or-descend); a wholly-inside early-accept is a named
refinement that yields the same set. The accept test is the same `Intersects` the linear scan uses,
so the result equals the tight scan exactly.

## Decisions

1. **Tight leaf boxes preserve byte-identical.** Every leaf stores the caller's tight world box and
   the accept test runs `Intersects` against it — identical to the linear scan's per-candidate test.
   A leaf whose tight box hits the frustum sits under ancestors whose union boxes also hit, so it is
   always reached and tight-tested; a reached leaf is included iff its box passes. Equality is exact
   (README decision 4). There are **no fat boxes** in this plan — fat boxes exist only to make an
   *incremental* `Update` a no-op within a margin, and this BVH is rebuilt, not updated; fat boxes
   return with incremental maintenance, the named refinement.

2. **Ids, not box copies.** `Query` returns the caller's `u32` leaf ids (the broadphase's candidate
   indices), not `AABB`/draw copies — `BVH` stays free of any scene/draw type.

3. **Pool of nodes in one `vector`, indices not pointers.** Nodes live in one contiguous
   `vector<Node>`; child links are indices. The cache-friendly layout the physics-engine trees use,
   and trivially copyable/clearable for a from-scratch rebuild.

4. **Build, not incremental insert/update/remove.** A from-scratch *O(N log N)* build over veng's
   candidate counts (tens to hundreds) is microseconds and yields a quality-optimal tree every
   frame the scene changes. Incremental insert/update/remove with fat-box no-op and rotation
   balancing (the `b2DynamicTree`/`btDbvt` mould) is the named refinement for when *N* grows into the
   thousands and per-frame rebuild cost begins to matter — it drops in behind this same `Build`/`Query`
   surface.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Math/BVH.h` (new) | The `BVH` class. |
| `engine/src/Math/BVH.cpp` (new) | `Build` (top-down split) + `Query` + the accessors. |
| `engine/CMakeLists.txt` | Add `src/Math/BVH.cpp`. |
| `tests/unit/bvh.cpp` (new) + the unit suite source list | Device-free equivalence + validity tests. |

## Verification

- Clean build; `include_hygiene` compiles `Veng/Math/BVH.h` (glm/fmt only — no backend leak).
- **`tests/unit/bvh.cpp`** (device-free, no ICD — the `frustum.cpp` / `aabb.cpp` pattern):
  - **Equivalence (the core property).** `Build` over many randomized leaf sets; for each, over many
    frustums (`FromViewProjection` over randomized cameras plus axis-aligned and degenerate ones),
    the **set** of ids `Query` returns equals the set a linear `for (leaf) if (Intersects(frustum,
    leaf.Box))` scan returns — an **exact** compare.
  - **Tree validity invariant** (a checker the test runs after each `Build`): every internal node's
    `Box` contains both children's boxes; every leaf is reachable from the root exactly once;
    `GetHeight()` equals `1 + max(child heights)`; `GetLeafCount()` matches the input leaf count;
    `GetNodeCount()` equals leaves + internal nodes.
  - **All-in / all-out / empty / single.** A frustum containing all leaves returns all ids; none
    returns empty; an empty `Build` yields an empty tree (`Query` returns empty, `GetRootBounds()`
    is `Empty()`); a single leaf matches the scan on the boundary either way.
  - **Balance.** After building large randomized sets, `GetHeight()` stays within a small factor of
    `log2(GetLeafCount())` (the split keeps the tree near-logarithmic).
- `smoke_golden` is **byte-identical** — Plan 02 adds no rendering.
- Full `ctest` green across the unit/death/cooker bands and the `gpu` band where present; validation
  gate clean under `VE_DEBUG`.
