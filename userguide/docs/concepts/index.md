# Concepts

A few things to understand before writing much code against the engine.

- **[How an application is structured](architecture-overview.md)** — the module
  and launcher, and what `Application` provides.
- **[The threading model](threading-model.md)** — the single render thread, and
  how to do work off it.
- **[Resource lifetime](resource-ownership.md)** — when it's safe to destroy a
  resource, and where to release them.
- **[Error handling](error-handling.md)** — when the engine aborts, and when it
  returns an error you check.
- **[API conventions](conventions.md)** — the naming and the type aliases the API
  is written in.
