# API reference

This section is the full public API, generated from the headers' Doxygen comments.
It covers the public surface of every veng library — the engine (`Veng/`), the
asset-pack format (`Veng/Asset/`), the cooker, and the editor (`VengEditor/`).

## Browsing

| Start from | When you want |
| --- | --- |
| [Class List](../veng/annotated.md) | To browse every documented type. |
| [Class Index](../veng/classes.md) | An alphabetical index of types. |
| [Class Hierarchy](../veng/hierarchy.md) | The inheritance tree. |
| [Namespaces](../veng/namespaces.md) | The `Veng::*` namespace structure. |
| [Files](../veng/files.md) | To browse by header. |
| [Functions](../veng/namespace_member_functions.md) | A free-function index. |

!!! note "Generated at build time"
    These pages are produced by [mkdoxy](https://mkdoxy.kubaandrysek.cz/), which
    runs Doxygen over the public headers when the site is built — there are no
    checked-in API Markdown files. Building the API reference locally requires
    `doxygen` on your `PATH`. The same comments also feed the standalone HTML
    reference produced by the CMake `docs` target.
