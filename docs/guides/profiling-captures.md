# Viewing a profiling capture in Perfetto or speedscope

The diagnostics profiler writes a run's timing to a compact **binary capture** — a
`.vtrace` file. That binary is the native form: it is what the observatory ingests and
what every decoder reads. To look at a capture in a general-purpose viewer, convert it
to **Chrome Trace Event JSON** with `vengtrace`, then drag the JSON into a browser-based
viewer. This one conversion buys **Perfetto** and **speedscope** — a full flamegraph,
timeline, and query surface — with nothing to install.

> The JSON is a **lossy, viewer-facing projection**, not a second source of truth.
> Nothing in veng or the observatory ever reads it: they decode the binary directly. Some
> things do not survive the trip (the exact clock resolution, the format version, anything
> Chrome Trace has no slot for), and that is fine — the JSON exists only for a human with a
> viewer. If a future need wants a field the JSON cannot carry, read the binary, not the
> projection.

## 1. Take a capture

Build with the profiler enabled (`VE_PROFILE`, on by default under `VE_DEBUG`) and start
a capture from code, a hotkey, or over MCP — see the diagnostics and capture-control
documentation for the API. Captures land in `<build-dir>/captures/` (gitignored and
disposable with the build tree), so a run leaves its `.vtrace` files there:

```
build-debug/captures/run.vtrace
```

A capture from a run that ended badly — a crash, a killed process — has **no trailer** and
is *truncated*. That is a first-class case, not an error: the converter recovers every
complete section and marks the result truncated.

## 2. Convert it

```sh
vengtrace convert build-debug/captures/run.vtrace --out run.json
```

Options:

- `--pretty` — indented, human-readable JSON. The default is compact, because a frame-rate
  capture is large.
- `--events complete|pair` — the scope-span event form. The default, `complete`, emits one
  event per scope with a duration (half the JSON, and the form both viewers prefer); `pair`
  emits a separate begin and end event, for the rare case a viewer wants them.

Exit codes: **0** on success (a truncated capture still converts and exits 0, with a warning
on stderr) · **1** a usage error · **2** an unreadable input · **3** an unknown format
version · **4** a write failure.

## 3. Open it

- **Perfetto** — open [ui.perfetto.dev](https://ui.perfetto.dev) and drag `run.json` onto
  the page (or **Open trace file**).
- **speedscope** — open [speedscope.app](https://speedscope.app) and drop `run.json` in.

Perfetto is the timeline-and-query view; speedscope is the flamegraph view. Both read the
same file.

## What each track means

The converter maps the capture's tracks onto one process with several threads:

- **Frames** — a dedicated ruler at the top: one span per frame, covering that frame's
  extent. Because a back-dated GPU pass is placed on the frame it *measured* (not the frame
  it was read in), the GPU pass sits under the correct frame's bar. On a truncated capture
  the final frame was still open at the cut, so it is drawn as an open span running to the
  end of the trace.
- **CPU threads** — one lane per recording thread (the main thread at the top, workers
  below it in order). Each scope is a span with its duration; nesting reads as stacked
  spans.
- **GPU** — the GPU passes on their own lane, running parallel to the CPU threads, so "the
  CPU finished early and waited" is legible. The spans carry real begin/end times and
  nesting, not a duration-only summary laid end to end.
- **Counters** — a queue depth, draw-call count, or GPU millisecond sample renders as a
  counter track against the timeline (Perfetto), beside the frame it happened in.
- **Instants** — a load completion or hitch marker sits on the track that raised it.

Every event also carries its frame index in `args.frame`, so you can filter and query by
frame in Perfetto without interpretation. If the capture lost events (a ring wrap) or was
truncated, that accounting travels into the JSON as process metadata and a process label,
so a viewer shows a lossy capture as lossy rather than quietly short.
