# Running the sample

The `hello-triangle` example is a complete, working reference for an application,
its scene setup, its assets, and its build wiring, and the quickest way to confirm
your build works.

## Windowed

After building, run its launcher:

```sh
build/examples/hello-triangle/hello_triangle-launcher
```

A window opens showing lit, textured primitives rendered through the deferred
pipeline, with orbiting lights and a Dear ImGui overlay.

## It runs anywhere

The launcher, the game library (`libhello_triangle.*`), and the cooked asset pack
(`sample.vengpack`) are self-contained: the launcher finds the library and the pack
relative to its own location. Copy those three files into another directory and the
launcher runs there unchanged.

## Headless capture

Setting the `HT_SMOKE` environment variable renders a single frame to an image
file with no window, then exits:

```sh
HT_SMOKE=/tmp/frame.ppm build/examples/hello-triangle/hello_triangle-launcher
```

It writes a 1280×720 RGB PPM of a fixed pose, useful for automated checks.

## Next

[Write your first application](your-first-app.md) to understand the pieces the
sample is built from.
