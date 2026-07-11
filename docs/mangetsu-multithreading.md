# Mangetsu multithreading architecture

Status: design baseline, 2026-07-11

This document covers the architecture analysis, proposed design, and incremental
implementation plan for adding internal event-level parallelism to Mangetsu. It
is intentionally written before the implementation. The current implementation
branch is derived from libass, but has substantial Mangetsu-specific rendering
paths, especially multi-border, distortion, furigana, image fills, gradients,
and RGBA output. Those paths are part of the design rather than follow-up work.

The principal prior art is libass PR #793, current head `09283f57d303af9a71ed80c83fe6242e4f035486`:

https://github.com/libass/libass/pull/793

The local `upstream/pthreads` reference is an older four-commit experiment and
must not be treated as the current proposal.

## 1. Existing architecture

### 1.1 Public ownership model

An `ASS_Library` owns configuration shared by tracks and renderers, including
the message callback and embedded font data. An `ASS_Track` owns parsed styles,
events, parser state, and per-event collision history. An `ASS_Renderer` owns
render settings, a FreeType library, font selection, caches, the current and
previous image lists, and one reusable `RenderContext`.

The API is not renderer-reentrant: an application must not call a render entry
point, mutate renderer settings, replace tag images or fonts, or destroy the
renderer concurrently on the same `ASS_Renderer`. Internal worker threads are
an implementation detail beneath one caller-owned render operation. Different
renderer instances remain independent except for an explicitly shared
`ASS_Library` callback and application-owned track access.

The track is read-only during the parallel portion. Lazy track initialization,
embedded-font updates, event-private collision-state allocation, cache eviction,
collision resolution, event pruning, and final image-list publication remain on
the calling thread.

### 1.2 Frame pipeline

The current alpha frame path is:

1. `ass_start_frame` validates the renderer and track, installs frame-global
   geometry and time, performs lazy track/font updates, rotates current and
   previous image lists, checks cache limits, and initializes per-event
   collision metadata.
2. The renderer scans the track in source order and selects active events.
3. Each active event is rendered serially with the renderer's reusable
   `RenderContext`.
4. Event results are sorted by layer.
5. Collision placement is resolved, grouped by layer, on mutable per-event
   collision history.
6. Event image lists are concatenated, transparent tiles are removed, the frame
   is reference-counted, change detection is performed, the prior frame is
   released, and old streaming events may be pruned.

The RGBA entry point has the same broad structure, but an event can emit both
coverage-mask images and premultiplied RGBA tiles. It then clips and concatenates
both result types and may convert the alpha list to RGBA when no Mangetsu feature
requires native RGBA rendering.

The natural concurrency boundary is an active event. Sorting, collision
resolution, publication, prior-frame release, and pruning depend on complete
event results and remain serial.

### 1.3 Event pipeline

`ass_render_event` performs these stages:

1. Reset event-local state and select the event style.
2. Parse override tags and text into glyph records. Mangetsu extensions add
   furigana groups, column state, image fills, multi-border state, distortion,
   jitter, fade colours, and gradient state.
3. Split style runs, resolve bidi direction, shape with HarfBuzz and FriBidi,
   and retrieve font glyphs.
4. Prepare furigana, measure, wrap, apply karaoke, reorder, align, and place
   columns and furigana.
5. Apply distortion, baseline transforms, event positioning, clipping, and
   rotation.
6. Retrieve or construct outlines, glyph bitmaps, border bitmaps, and composite
   bitmaps; run rasterization, blur, decoration, and shadow work.
7. Compute gradient rectangles and generate alpha and, when requested, RGBA
   image nodes.
8. Release event-temporary references and allocations while retaining reusable
   context buffers.

The expensive work is concentrated in shaping and fallback font lookup on cold
text, FreeType glyph loading and outline construction, outline transforms,
rasterization, blur, bitmap combination, vector clipping, distortion, and RGBA
painting. Cache hits make simple events cheap; complex drawings, large blur
radii, many borders, and cold glyph sets remain expensive.

### 1.4 Parser state

Track parsing (`ass_process_data`, codec-private parsing, and event pruning) owns
and mutates `ASS_ParserPriv` and track arrays. It is not part of the worker
region. Render-time tag parsing in `ass_parse.c` mutates only the supplied
`RenderContext` and its glyph/text arrays, while reading the selected event,
style, renderer settings, and track metadata.

FriBidi releases before 0.19.7 were not thread-safe by default. Thread-enabled
builds therefore require FriBidi 0.19.7 or newer, following PR #793. HarfBuzz
objects that carry mutable shaping state remain context-owned. Shared FreeType
faces require explicit locking.

### 1.5 Memory allocation and lifetime

Each `RenderContext` owns reusable text, line, break, combined-bitmap, furigana,
column, shaper, and rasterizer storage. Capacity grows with encountered content
and is retained until the context is destroyed. Event-specific linked glyph
records, drawings, distortion buffers, and temporary bitmap arrays are released
at event cleanup.

Caches allocate one header, value, and key block per entry. Cache values may own
aligned bitmap buffers, outlines, fonts, or references to other cache values.
The cache itself owns one reference; rendered images and nested cache values may
hold additional references. Eviction currently occurs at a frame boundary.

Alpha result nodes either reference cached bitmap data or own a transformed
buffer. RGBA nodes own aligned buffers and are currently charged against a
renderer-global output limit. A worker context multiplies retained context and
shaper storage, so workers are created lazily and only when at least two events
can execute concurrently.

### 1.6 Shared mutable state audit

The following data cannot be used concurrently without changes:

- Cache hash chains, LRU links, sizes, promotion state, and reference counts.
- `ASS_FontSelector` lookup/provider state.
- `ASS_Font` face arrays and every operation that changes or observes mutable
  `FT_Face`, `FT_Size`, glyph-slot, or charmap state.
- The application log callback when messages originate on workers.
- The renderer-global `rgba_output_size` and one-shot limit warning.
- The renderer-global Mangetsu gradient debug accumulator.
- The single renderer `RenderContext`.

The following state is deliberately caller-thread-only:

- Renderer settings and reconfiguration.
- `track`, frame time, frame dimensions, and current/previous frame roots.
- Event selection and `eimg` allocation.
- Per-event collision-history allocation and mutation.
- Cache promotion/eviction and cache destruction.
- Sorting, collision placement, concatenation, change detection, and pruning.

The following state can be shared read-only during the worker region:

- Track event/style arrays and event text.
- Renderer settings, frame geometry, bitmap-engine function table, tag-image
  entries, and font-provider configuration.
- Published cache keys and values after construction completes.

Each worker owns a stable `RenderContext`, including two Mangetsu shapers,
rasterizer scratch data, text arrays, and a cache client used for deferred LRU
promotion.

## 2. Assessment of libass PR #793

### 2.1 Problems solved by the current PR

The current 21-commit PR is a substantial redesign of the older pthread branch.
It provides:

- Autotools and Meson detection for atomics, POSIX threads, and native Windows
  threading, with a portability wrapper for atomics and condition variables.
- A TSAN CI job.
- Serialized log-callback invocation, with an opt-out contract.
- Font-selector locking and per-font locking around shared `FT_Face` access in
  both glyph generation and shaping.
- A `CacheClient` abstraction so each render context can record cache
  promotions without contending on the LRU queue on every hit.
- Concurrent cache lookup and placeholder construction, so one thread builds a
  missing value and peers wait for publication.
- Persistent per-renderer workers and atomic event claiming.
- Per-event rendering into preassigned result slots, followed by the existing
  serial sort/collision/concatenation stages.
- `ass_set_threads`, automatic logical-core detection, and a serial fallback.

These decisions correctly preserve the existing event boundary, avoid a project
rewrite, isolate mutable shaping/rasterizer state per worker, and keep collision
semantics out of the parallel region.

### 2.2 Important rationale to retain

Per-event scheduling is preferable to parallelizing individual glyphs. Events
already have isolated output lists and most intermediate state is naturally
event-local. Glyph-level work would require much finer synchronization, disturb
cache locality, complicate shaping runs and word-level effects, and increase
overhead on ordinary subtitles.

Persistent workers avoid per-frame creation costs. A per-context cache client
avoids making LRU maintenance a global hit-path lock. Placeholder publication
prevents duplicate expensive glyph/bitmap construction. FreeType access must be
locked per `ASS_Font`, rather than with one renderer-global lock, so unrelated
fonts remain parallel. Cache eviction after the worker barrier makes reclamation
much easier to reason about than concurrent eviction.

### 2.3 Remaining limitations and unfinished work

The PR is open and not an upstream release. Its discussion still contains
unresolved review history. An older revision was reported crashing in
`ass_cache_cut`; the report was against commit `5631927`, not the current head,
but it identifies cache unlinking and reclamation as a high-risk area. A current
review also calls the worker failure state overly complex.

The current PR has these architectural limitations:

- It uses lock-free hash-chain insertion and atomic intrusive links. This gives
  a fast hit path but substantially raises proof and maintenance cost. The
  absence of concurrent eviction narrows the problem enough that sharded locks
  can provide simpler safety with limited contention.
- It starts one worker per requested thread and blocks the caller, creating one
  more thread than necessary for a requested concurrency level and missing the
  caller's available CPU during the event phase.
- It starts the pool before checking whether the frame has enough active events,
  causing needless worker creation for editor/single-event workloads.
- Automatic logical-core count is the default and is not capped by active event
  count when the pool is created. This can multiply retained context memory on
  high-core-count systems.
- One event remains single-threaded. Frames dominated by one complex drawing or
  one heavily blurred event receive little benefit.
- Dynamic atomic claiming balances event costs but does not address priority or
  deadlines. There is no cancellation when an editor supersedes a frame.
- Font locks serialize all operations on the same `ASS_Font`; a frame dominated
  by one face may scale poorly even when caches are cold.
- The PR does not cover Mangetsu's RGBA accounting or gradient debug state.
- Worker-originated logging changes callback thread affinity. Serializing calls
  prevents concurrent callback execution but cannot make a main-thread-only
  application callback safe.
- Frame image-list reference counts are not made atomic by the PR; this design
  does not broaden the public API to permit concurrent frame ref/unref unless a
  later, separately reviewed change does so.

### 2.4 Potential races to exclude in Mangetsu

- A cache entry must not be observed before its key and value are fully
  published. Waiters must not miss the completion wake-up.
- Cache eviction or emptying must never overlap worker lookup, insertion,
  deferred promotion, or construction.
- Cache reference decrements on detached entries must use atomic reference
  counts if application-owned image references can be released concurrently.
- Every access to mutable `FT_Face` state, including HarfBuzz callbacks and
  charmap fallback, must use the owning font lock.
- Font selection and face insertion must use a consistent lock order to avoid a
  font-selector/font deadlock.
- A worker must never retain a pointer into a resizable worker array.
- Event result slots must be allocated before workers start and must not move
  until all jobs complete.
- RGBA limit decisions must not depend on worker completion order.
- Gradient debug merging must not race or select a different first 64 segments
  depending on scheduling.
- The main thread must observe all event writes before sorting or concatenating,
  including on frames where the caller completes the final job itself.
- Pool resize, renderer reconfiguration, font replacement, tag-image changes,
  and destruction must happen only while no frame is active.

## 3. Proposed architecture

### 3.1 Compatibility and defaults

Add `ass_set_threads(ASS_Renderer *, unsigned)` without changing existing render
entry points or result types. A requested value of zero selects the logical CPU
count; one selects serial execution; values above one select the maximum total
event concurrency including the calling thread.

Mangetsu defaults to one thread. This differs from PR #793's automatic default,
but preserves existing resource use, callback thread affinity, and editor
responsiveness unless an application explicitly opts in. It is the safer choice
for a renderer with new RGBA and extension paths. Applications can opt in as:

- Subtitle editor: 1 by default, or 2 after measuring multi-event previews.
- Video player: a fixed small count such as 2-4 for stable resource use.
- Offline encoder: 0 for automatic CPU utilization, or an explicit job-budgeted
  count when the encoder also parallelizes frames.

Thread-count changes take effect on the next render call. Calling configuration
APIs concurrently with rendering remains unsupported.

### 3.2 Worker ownership and lifecycle

Workers are owned by one renderer and persist until a requested shrink or
renderer destruction. Each worker is a separately allocated stable object that
contains its thread handle and `RenderContext`; the renderer stores pointers to
workers so growing the pointer array cannot invalidate worker arguments.

The calling thread participates with the renderer's existing `RenderContext`.
For a requested concurrency of N, no more than N-1 internal workers are needed.
Workers are created lazily after active-event enumeration and only up to
`min(requested - 1, active_events - 1)`. A later busier frame may grow the pool.
A lower requested count stops and joins the pool at a frame boundary, then the
next frame creates only what it needs.

Render-context allocation is performed on the caller before `pthread_create`.
A context allocation or thread creation failure simply limits usable parallelism
for that renderer and logs once; existing workers and caller-thread rendering
continue. This avoids a multi-flag worker-startup handshake.

### 3.3 Scheduling and barrier

Active events are first copied into stable `EventImages` slots in source order.
An atomic monotonically increasing index assigns jobs dynamically, which handles
uneven event costs without a central queue lock. Every job writes only its own
slot. The caller and workers use the same job function.

The pool mutex protects frame activation, shutdown, and condition-variable
waits. Atomic `next_job` and `remaining_jobs` counters cover the hot path. The
last completing participant signals the frame-done condition while holding the
pool mutex. The caller always completes a mutex acquire/release after the
remaining count reaches zero, establishing a full worker-to-caller visibility
barrier even if the caller executed the numerically final job.

Workers cannot advance into a later frame without returning through the pool
mutex and waiting for a new frame activation. There is exactly one in-flight
frame per renderer.

### 3.4 Cache design

Retain PR #793's `CacheClient` and deferred per-context promotion model, but do
not initially import its lock-free intrusive hash chains. Mangetsu instead uses
fixed sharded cache locks:

- A cache keeps its existing bucket count and maps each bucket to one of a fixed
  number of lock shards.
- Lookup and hash-chain insertion hold only the corresponding shard mutex.
- On a miss, a placeholder with a complete immutable key is inserted under the
  shard lock. The creator releases the lock during expensive construction.
- A waiter sleeps on the shard condition variable until the placeholder is
  published. Publication occurs under the same mutex, preventing lost wake-ups.
- Cache values and keys are immutable after publication.
- Each context records first use in the current frame in its `CacheClient` list.
  The caller merges these lists into the existing LRU after the worker barrier.
- Eviction, emptying, and cache-client destruction occur only outside the worker
  region. This means hash-chain removal never races lookup or insertion.
- Entry reference counts use the existing atomic portability layer, since
  detached cached data can outlive the cache's ownership through frame images.

This is preferable here because the hit-path critical section is restricted to
one shard and excludes construction and LRU manipulation, while pointer
publication and reclamation remain directly auditable. A lock-free replacement
is a future benchmark-driven optimization, not a prerequisite for correctness.

### 3.5 Font and shaping synchronization

The font selector receives a mutex around provider lookup and face insertion.
Each `ASS_Font` receives a mutex and atomic face count. All shared `FT_Face`
operations in `ass_font.c`, cache constructors in `ass_render.c`, and HarfBuzz
callbacks in `ass_shaper.c` execute under the owning font lock.

Lock order is font-selector first, then font, only while adding a face. Ordinary
glyph and metric operations take only the font lock. Cache shard locks are not
held during value construction, so a constructor can safely acquire font locks
without a cache/font lock-order cycle.

Thread-enabled builds require FriBidi 0.19.7 or newer. Each context retains its
own shapers and HarfBuzz buffers.

The broken-font fallback that probes every charmap restores the face's original
charmap after obtaining a glyph index. Previously it left the last probed
charmap installed, so later glyph lookup depended on which event happened to
encounter the broken mapping first. Restoring it is a narrowly scoped behavior
change for malformed fonts and is required for thread-count-independent output.

### 3.6 RGBA determinism

RGBA memory accounting and its limit behavior remain renderer-global to preserve
existing semantics. In an RGBA frame, parsing, shaping, layout, outline lookup,
rasterization, and bitmap combination run in parallel. Before the final
`render_text`/box-output stage, a successful event waits for an event-index
ticket. Failed or empty events advance their ticket from the common job wrapper.

Only the event holding the ticket may perform RGBA allocations and update the
global budget. The wrapper advances the ticket and broadcasts after the event's
output stage. This preserves the same source-order allocation and tile-dropping
decisions as serial rendering. It deliberately serializes the final RGBA paint
stage; correctness and bounded memory are preferable to schedule-dependent
output. A later two-pass deterministic allocator may remove this limitation.

The alpha-only entry point does not use the ticket.

### 3.7 Mangetsu debug state

Gradient debug collection becomes event-local. An event allocates debug storage
only when it encounters an active gradient segment. After the worker barrier,
the caller merges event debug records in source order into the renderer's fixed
debug array, then frees event-local records. This preserves current counts and
the deterministic first-segment limit without inflating every `EventImages`
slot by a large fixed array.

### 3.8 Logging

The library log callback is serialized when internal threading is enabled. The
implementation does not adopt PR #793's `ass_set_threads` side effect that opts
out of log locking. Opting into more than one thread permits callbacks to
originate from a worker, which is documented on `ass_set_threads`. Applications
that require main-thread-only callbacks must use one thread or marshal callback
work themselves.

### 3.9 Deterministic output guarantees

For identical renderer settings, track data, font environment, timestamp, and
allocation success, output is independent of requested thread count and worker
completion order:

- Event result slots are assigned in source order before dispatch.
- Dynamic scheduling never determines concatenation order.
- Existing layer sort and collision algorithms run after the barrier.
- Cache values are pure functions of immutable keys; a single placeholder wins
  each key and peers observe its published result.
- RGBA allocation order is explicitly ticketed by event index.
- Gradient debug data is merged in source order.
- Cache LRU ordering may differ among independent keys used for the first time
  in the same frame, but eviction cannot occur until the frame completes and
  therefore cannot affect that frame's pixels. Promotion merge order will be
  fixed by event/context order where practical to stabilize later eviction.

Out-of-memory and external callback behavior are not claimed to be bitwise
identical across all schedules unless explicitly handled; all such failures must
remain memory-safe and return a valid partial/empty result rather than corrupt
state.

## 4. Incremental implementation roadmap

No milestone below depends on locally compiling the project. Validation is by
focused static review, `git diff --check`, symbol/call-site audits, and GitHub
Actions. CI must compile Autotools and Meson configurations and run tests. A
thread-sanitizer job is required before event dispatch is enabled.

### Milestone 0: architecture baseline

Commit this document. No runtime behavior changes.

Review: confirm the ownership model, default serial policy, sharded-cache choice,
RGBA ticketing, and API contract before relying on them in code.

### Milestone 1: threading portability and CI

Import and adapt the reviewed pthread/Win32/atomic portability layer from PR
#793. Add build-system thread detection, the FriBidi 0.19.7 minimum, and a TSAN
CI configuration. Do not create worker threads yet.

Review: all build feature combinations retain a serial fallback; no thread-only
type leaks into disabled builds; Windows and POSIX wrappers have matching
semantics; the old dependency image can either satisfy the new FriBidi minimum
or explicitly disable threading without losing serial support.

### Milestone 2: shared font and logging safety

Add library log serialization, font-selector locking, per-font locking, and lock
all shared FreeType access in glyph and shaping paths. Rendering remains serial.

Review: enumerate every `FT_Face` access, prove lock ordering, check every error
path unlock, and ensure font/cache destruction happens with no active frame.

### Milestone 3: concurrent cache clients

Add cache clients, sharded lookup locks, placeholder construction, atomic entry
references, deferred promotions, and caller-thread-only eviction. The main
renderer context is the first cache client; rendering remains serial.

Review: duplicate-key races, placeholder wake-up, construction failure values,
key ownership, nested cache references, promotion uniqueness, eviction unlinking,
empty/destroy ordering, and counter overflow. Add cache stress tests suitable for
TSAN.

### Milestone 4: worker pool and alpha event dispatch

Add `ass_set_threads`, lazy renderer-owned workers, caller participation, stable
preallocated event slots, dispatch for `ass_render_frame`, and serial fallback.
Default remains one thread.

Review: partial worker creation, pool growth/shrink/destruction, frame barrier,
event failures, zero/one event frames, cache promotion timing, collision state,
image ownership, and identical alpha output at thread counts 1, 2, and auto.

### Milestone 5: deterministic Mangetsu state and RGBA dispatch

Move gradient debug collection to per-event results and merge it on the caller.
Use the common dispatcher in `ass_render_frame_rgba` with source-order RGBA
tickets and deterministic global memory accounting.

Review: every early return advances the ticket, output allocation never occurs
outside the ticketed section, clipping/freeing keeps accounting balanced, debug
records are freed on every path, and native/converted RGBA output matches serial
rendering at multiple thread counts.

### Milestone 6: regression and concurrency tests

Add deterministic cross-thread-count render tests for existing selftest samples,
multi-border, gradients, distortion, furigana, columns, clipping, RGBA budget
exhaustion, and empty/invalid events. Add repeated cache-miss and renderer
create/destroy stress under TSAN. Keep the public default serial in all existing
tests and add explicit threaded variants.

Review: tests compare pixels, geometry, ordering, `detect_change`, debug state,
and failure cleanup rather than only checking for crashes.

### Milestone 7: CI performance characterization

Extend the profile utility to accept thread count and report cold/warm timing.
Record, but do not enforce as pass/fail, results for one-event, many-event,
cache-hot, cache-cold, alpha, and native-RGBA samples at thread counts 1, 2, 4,
and auto. Capture peak cache/output memory where available.

Review: separate initialization, first frame, warm frames, and cleanup; avoid
claims based on one synthetic sample; report regressions in the serial path.

## 5. Performance expectations and limits

Many simultaneously active, independent events with expensive cold glyph or
bitmap work should scale until cache-shard, same-font FreeType, memory-bandwidth,
or RGBA-ticket contention dominates. Warm simple subtitles may see little gain
and can regress if forced through a pool, which is why zero/one event frames stay
on the caller and the default is serial.

The serial fraction includes frame setup, event enumeration, pool coordination,
layer sorting, collision placement, concatenation, change detection, cache
promotion/eviction, pruning, and ticketed RGBA painting. One large event does not
scale. Offline encoders that already render multiple frames concurrently should
budget Mangetsu threads to avoid oversubscription.

No performance number is claimed until CI artifacts run on controlled samples.
The first production gate is pixel identity and TSAN cleanliness; throughput is
the second gate.

## 6. Future optimization candidates

- A two-pass RGBA size calculation and deterministic prefix allocation to allow
  parallel painting without schedule-dependent budget decisions.
- Per-face clones or immutable font-data-backed FreeType faces for workers, if
  profiling shows same-font locking dominates and memory cost is acceptable.
- Work stealing with estimated event costs or editor deadlines, while retaining
  source-indexed output.
- Parallel work inside a single event at the bitmap-combination or independent
  blur layer level, only after event-level scaling is exhausted.
- A benchmark-justified lock-free cache hit path, preserving frame-boundary
  reclamation and backed by model checking or equivalent stress coverage.
- Context memory trimming after unusually large events and a configurable worker
  idle timeout for long-lived editors.
- Host-provided executors for applications that already own a thread pool. This
  would require a separate API proposal and must not complicate the internal
  default path.
