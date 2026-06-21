# Scene & ECS

veng's scene layer is an ECS world whose components are plain reflected structs.
The same reflection drives both the editor inspector and serialization, so a
component is authored as data, cooked into a prefab, and spawned at runtime — all
from one `VE_REFLECT` block next to the struct.

- **[Entities & components](ecs.md)** — the scene, entities, queries, and the
  hierarchy.
- **[Game systems](systems.md)** — where gameplay logic lives, and how it runs.
- **[Reflection & type registration](reflection.md)** — how a struct becomes a
  serializable component, and how types are registered.
- **[Prefabs & spawning](prefabs.md)** — cooked prefabs, `SpawnInto`, and recipe
  components that build their resources at spawn.
