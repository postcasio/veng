# Error handling

veng does not use exceptions. Errors come back to you one of two ways.

## Misuse aborts

Programming errors — passing an out-of-range index, using a stale entity, an
unsupported format, a failed GPU allocation — are fatal. The engine logs the
problem, breaks into the debugger in a debug build, and aborts. These are bugs to
fix, not conditions to handle, so there's no error value to check — the call does
not return.

## Recoverable failures return a `Result`

A call that can legitimately fail at runtime — loading a file that may not exist,
say — returns a `Result<T>`. Check it before using the value:

```cpp
auto shader = LoadShader(path);
if (!shader) {
    Log::Error("shader load failed: {}", shader.error());
    return;
}
use(shader.value());
```

`Result<T>` holds either the value or an error string. (For functions that return
nothing on success, the type is `VoidResult`.)

Asset loads use a richer error type, `AssetResult<T>`, which carries a structured
error you can branch on rather than a string:

```cpp
auto material = GetAssetManager().LoadSync<Material>(BrickMaterial);
if (!material) {
    Log::Error("load failed: {}", material.error().Kind);
    return;
}
```

See [Loading at runtime](../assets/loading.md).
