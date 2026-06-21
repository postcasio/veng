# API conventions

The API is consistent in a few ways that make it easier to predict.

## Type aliases

veng uses its own aliases rather than the raw `std`/glm names, and the public API
is written in them:

| Category | Names |
| --- | --- |
| Containers | `string`, `vector<T>`, `map`, `optional`, `path`, `function` |
| Scalars | `u8` `u32` `u64`, `f32`, `usize` |
| Math (glm) | `vec3`, `mat4`, `uvec2`, `quat` |
| Smart pointers | `Ref<T>` (`shared_ptr`), `Unique<T>` (`unique_ptr`) |

## Naming

- Getters are `GetFoo()`, setters `SetFoo()`. A boolean query starts with `Is`,
  like `IsLoaded()` or `IsKeyPressed()`.
- Components are named for what they are, not suffixed with "Component":
  `Transform`, `Light`, `Camera`, not `TransformComponent`.

## Factories and `Info` structs

A resource is created through a static factory taking an `Info` struct, written
with designated initializers:

```cpp
auto sampler = Sampler::Create(Context, {
    .Name      = "linear-clamp",
    .MinFilter = Filter::Linear,
    .MagFilter = Filter::Linear,
    .AddressU  = AddressMode::ClampToEdge,
});
```

The verb on the factory tells you what it does and whether it blocks:

| Verb | Makes | Runs |
| --- | --- | --- |
| `Create(Info)` | a low-level GPU resource | immediately |
| `Build<T>` / `BuildSync<T>` | an asset, from data | async / blocking |
| `Load<T>` / `LoadSync<T>` | an asset, from a pack | async / blocking |
| `Upload` / `UploadSync` | uploads data into a resource | async / blocking |

See [Resource lifetime](resource-ownership.md) for when it's safe to destroy what
they return.
