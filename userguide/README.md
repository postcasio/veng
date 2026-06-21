# veng user guide

The source for veng's user guide and API reference. It is a
[MkDocs](https://www.mkdocs.org/) site using the
[Material](https://squidfunk.github.io/mkdocs-material/) theme, with the public
API reference generated from the headers' Doxygen comments by
[mkdoxy](https://mkdoxy.kubaandrysek.cz/).

## Building locally

You need Python 3 and — for the API reference — `doxygen` on your `PATH`
(`brew install doxygen` on macOS).

```sh
cd userguide
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt

mkdocs serve     # live-reload preview at http://127.0.0.1:8000
mkdocs build     # render the static site into ./site
```

`mkdocs serve` rebuilds on every save, including a Doxygen re-run when a header
changes, so you can edit prose and headers and watch the result live.

## Layout

- `mkdocs.yml` — site config, navigation, theme, and the mkdoxy project that
  points Doxygen at the public include trees (`engine`, `assetpack`, `cooker`,
  `editor`).
- `docs/` — the hand-written guide pages (Markdown).
- The API reference under **API reference** in the nav is generated at build
  time from the headers; there are no checked-in API Markdown files.

## Writing guidelines

The guide is consumer-facing: it explains how to *use* veng, in the same house
vocabulary the public API uses. It is not a design history — keep it
present-tense and factual, the same rule the code comments follow (see the root
`CLAUDE.md`). Deep backend internals belong in the per-module `CLAUDE.md` files,
not here.
