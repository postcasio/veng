# Scene hierarchy — intrusive sibling-linked representation (DELIVERED)

> **Delivered.** The representation redesign described here has landed: the `Hierarchy`
> component (replacing the up-link `Parent`), the `SetParent`/`Detach`/`MoveBefore` +
> `GetParent`/`ForEachChild` operations, the `Parent`→`Hierarchy` migration across
> `WorldMatrix`/`ComputeWorldMatrices`/prefab spawn/every caller, the O(subtree) recursive
> destroy, and device-free unit + death coverage. An increment of **area 7 (scene / entity
> model)**. The cached depth-sorted propagation and the incremental broadphase it unlocks
> remain the named follow-ons below.

## The problem with the up-link `Parent`

The builtin hierarchy today is a single **up-link**: `Parent { Entity Value }`
([Components.h](../../engine/include/Veng/Scene/Components.h)) stored on the child, and
`WorldMatrix` / `ComputeWorldMatrices` ([Transforms.h](../../engine/include/Veng/Scene/Transforms.h))
walk the parent chain `parent.world * local`, recompute-on-demand, no cache. It is the
minimal thing that works, and it has five structural limits:

- **No down-traversal.** A parent does not know its children — finding them is an O(N) scan
  of the `Parent` pool. Recursive `DestroyEntity` pays exactly this (gather the subtree by
  repeated full scans); so would reparent, subtree-move, and any editor "select children".
- **No child order.** Siblings have no defined order, so iteration and serialization are not
  deterministic across runs, and an editor hierarchy panel has nothing stable to show or
  reorder.
- **O(N·depth) propagation.** Every `WorldMatrix` re-walks the chain to the root, and
  `ComputeWorldMatrices` recomputes the entire world each call — no cache, no "only what
  moved".
- **Unmanaged references.** A `Parent` field is a raw `Entity` handle with no integrity
  guarantee; it can dangle (the crash recursive destroy patches at the source) because no
  invariant ties a child's link to its parent's existence.
- **No mutation chokepoint.** Reparenting by writing a mutable `Parent&` is invisible — the
  same "no setter to hook" gap that forced the spatial-version change-tick. There is nowhere
  to maintain an invariant or emit a precise change signal.

## The design: an intrusive sibling-linked `Hierarchy`

Replace the up-link with one builtin component holding **four intrusive links**:

```cpp
// The scene hierarchy link for one entity. Parent is the up-edge (Null = root);
// FirstChild heads a doubly-linked sibling list (Null = leaf); PrevSibling /
// NextSibling thread that list. All four are maintained together by the Scene's
// SetParent / Detach operations — never written directly — so the structure stays
// consistent and every reparent emits the hierarchy change signal. Entity::Null is
// the sentinel throughout.
struct Hierarchy
{
    Entity Parent      = Entity::Null;
    Entity FirstChild  = Entity::Null;
    Entity PrevSibling = Entity::Null;
    Entity NextSibling = Entity::Null;
};
```

This is the classic scene-graph-in-an-ECS layout (the intrusive form EnTT documents and
Godot's tree uses): O(1) attach/detach/reparent, ordered children, no per-node heap
allocation, and four `Entity` fields in one sparse-set pool.

**Topology is mutated through operations; values stay mutable refs.** `Transform` values
keep the immediate-mode `Get`/`View`/`Each` ergonomics — a write is still a bare field
poke. But the *topology* moves behind `Scene` methods, which are the chokepoint the up-link
lacked:

```cpp
void   SetParent(Entity child, Entity parent); // detach from old siblings, attach under new (append); O(1)
void   Detach(Entity child);                    // reparent to root (Parent = Null)
void   MoveBefore(Entity child, Entity sibling);// drag-reorder / insert-at; O(1)
[[nodiscard]] Entity GetParent(Entity) const;   // up-edge, or Null
void   ForEachChild(Entity, const function<void(Entity)>&) const; // FirstChild → NextSibling…, insertion order
```

`SetParent` unlinks the child from its current sibling list, links it into the new parent's
list, and fixes all four affected links in O(1); it validates that `parent` is not a
descendant of `child` (a cycle is misuse — fatal assert) and bumps the spatial version (and,
once cached propagation lands, marks the moved subtree dirty). `ForEachChild` is the
down-traversal the up-link never offered — a sibling walk, O(children), in insertion order;
the down-traversal ships as `ForEachChild` rather than a range type. `MoveBefore` is the
editor's drag-reorder / insert-at primitive.

### What it fixes, and what it enables

- **Recursive `DestroyEntity` becomes O(subtree), not O(N).** The teardown walks
  `FirstChild` → siblings → descendants directly instead of repeatedly scanning the `Parent`
  pool — the down-index the planset-23 recursive destroy explicitly named as its O(children)
  successor.
- **Integrity by construction.** Every structural change goes through `SetParent`/`Detach`,
  which maintain the links as a set; a child can no longer hold a link to a parent that does
  not link back, and recursive destroy removes a subtree whole. The `WorldMatrix` cycle /
  dead-parent assert stays purely as a misuse detector.
- **Ordered, deterministic children** for serialization stability and the editor hierarchy
  panel (which also gains the reorder/drag-reparent it needs from `SetParent` + a sibling
  insert-at).
- **A precise change signal.** `SetParent` (topology) and the spatial-version bump
  (Transform writes) together name exactly which subtrees moved — the feed a cached,
  dirty-tracked transform propagation and an *incremental* broadphase both want.

## Cached, depth-sorted propagation — the paired perf follow-on

The representation change is the prerequisite for the transform-propagation perf layer
(area 7's "dirty-flag transform propagation"): with ordered down-traversal, world matrices
can be **cached** per entity and recomputed **top-down in one O(N) sweep** (roots → leaves
via the child links, or a maintained `Depth` for a sort key), invalidating only the subtrees
a `SetParent` or a `Transform` write dirtied — replacing the O(N·depth) recompute-on-demand
walk. That dirty-subtree signal is also exactly what would let the
[scene-renderer](scene-renderer.md) broadphase move from version-gated **rebuild** to
**incremental** maintenance (update only the changed world bounds), the refinement that
planset's BVH named. The propagation layer is a separate increment behind this
representation; the representation is useful on its own (down-traversal, order, integrity).

## Decisions

1. **One `Hierarchy` component, four intrusive links, replacing `Parent`.** Co-locating the
   links in one sparse-set pool keeps a structural op a single lookup, versus four pools or a
   side table. `Parent` (the up-link) is subsumed; callers reading `Parent.Value` migrate to
   `Hierarchy.Parent` (or a `GetParent(Entity)` accessor).

2. **Topology behind operations, values stay mutable refs.** Reparenting is inherently a
   multi-link structural change — it cannot be a field poke that keeps the siblings
   consistent. Putting it behind `SetParent`/`Detach` is the setter the change-tick design
   wanted, without giving up the mutable-ref ergonomics for `Transform` *values*.

3. **Ordered siblings.** The sibling list preserves insertion order; an insert-at /
   move-sibling op serves the editor's reorder. Order is part of the model, not incidental.

4. **Recursive destroy walks the links.** With `FirstChild`/siblings, destroy is O(subtree)
   — the planset-23 recursive destroy's named O(children) successor; the O(N)-scan
   implementation is the interim.

5. **Cached depth-sorted propagation is a paired follow-on, not part of the representation.**
   The links make it possible; it lands as its own increment and is what unlocks an
   incremental broadphase.

6. **On-disk prefab format is unchanged.** A cooked prefab stores only the **parent edge**
   per entity (as today); `SpawnInto` rebuilds the sibling/child links by calling `SetParent`
   per entity during spawn, in entity order — so child order is the prefab's authored order
   and no sibling links are serialized. The reflection serializer treats `Hierarchy`'s
   non-`Parent` links as derived (not persisted), or `Hierarchy` is spawned via the op rather
   than `ReadFields`.

7. **Integrity is asserted, not recovered.** `SetParent` fatally asserts on a cycle (a
   descendant adopting an ancestor) — API misuse under the error policy, like the existing
   `WorldMatrix` cycle assert it complements.

## Interactions with delivered work

- **planset-23 (BVH broadphase).** The spatial-version change-tick gains a `SetParent` bump
  site; the recursive destroy gains its O(subtree) implementation; and the dirty-subtree
  signal is the path from version-gated rebuild to incremental tree maintenance.
- **Scene editor (area 6, sub-area D).** The hierarchy panel needs ordered down-traversal and
  drag-reparent/reorder — delivered directly by `Children` + `SetParent` + sibling insert-at.
- **Prefab spawn (area 10).** `SpawnInto`'s entity-reference remap already rewrites the parent
  edge to the freshly-spawned handles; it rebuilds the intrusive links through `SetParent`
  rather than copying raw link fields.

## Scope

A planset of its own when taken up: the `Hierarchy` component + `SetParent`/`Detach`/`Children`
operations, the `Parent`→`Hierarchy` migration across `WorldMatrix`/`ComputeWorldMatrices`/prefab
spawn/any caller, recursive destroy re-pointed at the links, and device-free unit tests over the
structural ops (attach/detach/reparent/destroy/cycle/order). The cached depth-sorted propagation,
and the incremental broadphase it unlocks, are named follow-ons behind it.
