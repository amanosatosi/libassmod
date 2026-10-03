# VSFilterMod random path tags

`\rndN` sets X, Y and Z amplitudes independently to the same destination.
`\rndxN`, `\rndyN` and `\rndzN` change only their respective component.
`\rndsS` sets the hexadecimal seed. All five tags are animatable with `\t`.

```ass
{\rnd30\rndsABC}Randomized boundary
{\rndx1\rndy2\rndz3\t(0,1000,\rnd30\rnds100)}Animated deformation
```

The seed is parsed like Windows `wcstol(...,16)`, including optional signs
and `0x`, with signed 32-bit saturation. For example `10` means hexadecimal
16; `FFFFFFFF` saturates to `7FFFFFFF`, whereas `-1` seeds the local generator
with the unsigned bit pattern `FFFFFFFF`. Seed animation interpolates the
integer values and truncates toward zero, rather than interpolating RNG output.

For an amplitude, Mangetsu parses the tag value, multiplies by 8, interpolates
from the current stored integer, and truncates toward zero. Thus `0.124`
stores 0, `0.125` stores 1, and `-0.125` stores -1. Negative amplitudes are
retained in state but disable deformation on that axis. Bare `+` and `-`
are absolute signs. Mangetsu's explicit `~+N` / `~-N` extension adds or
subtracts in tag units. Empty tags reset the affected components to zero;
empty `\rnd` resets all three axes, and `\r` resets both amplitudes and seed.
Every frame reconstructs state from the event, so direct seeks and backwards
rendering do not inherit values or geometry from a previously rendered frame.

The behavior follows VSFilterMod's [RTS.cpp](https://github.com/sorayuki/VSFilterMod/blob/master/src/subtitles/RTS.cpp)
and signed random state in [STS.h](https://github.com/sorayuki/VSFilterMod/blob/master/src/subtitles/STS.h).
A local unsigned 32-bit generator reproduces the Microsoft CRT sequence:

```text
state = state * 214013 + 2531011  (32-bit wraparound)
output = (state >> 16) & 32767
amp = stored_amplitude * 100
offset = (amp - output % (amp * 2 + 1)) * 0.01
```

Offsets use the reference SSE2 float conversion and multiply. Enabled axes
consume numbers in X/Y/Z order within each lane. Four lanes are generated at
a time, and path points take them in reverse lane order. A partial group still
consumes all four lanes. Disabled axes consume no numbers. This sequence is
explicitly scalar and independent of CPU SIMD support or renderer threads;
neither platform `rand()` nor global mutable RNG state is used. Each randomized
outline begins with the effective seed, without event-order or glyph-index seeds.

The output is only 15 bits. Near `\rnd20.5`, the modulo divisor exceeds the
generator's range; larger amplitudes become increasingly biased. `\rnd30`
and `\rnd100` intentionally retain this historical asymmetric corruption.
It is compatibility behavior, with no normalization or 21-unit clamp.
Amplitude arithmetic is widened to avoid signed overflow. Nonfinite numeric
values are sanitized and geometry outside the renderer's safe outline bounds
is rejected before rasterization.

Randomization acts on the path/control points after `\distort`, before font
X/Y scaling, shear, `\frz`, `\frx`/`\fry`, and projection. Z participates in
the projection denominator even with zero X/Y rotation. With `\ortho1`, Z
can affect rotated X/Y but never causes a perspective divide. Mangetsu's `\z`
continues to provide its ordinary uniform depth offset.

The fill is rasterized from this boundary; ordinary, anisotropic and multiple
borders widen that same projected boundary, and shadow follows the resulting
geometry. Opaque boxes retain their ordinary box geometry. Advances, wrapping,
baselines, line height, alignment, collision metrics and karaoke timing use
the original logical glyph metrics. Randomized temporary geometry and bitmaps
are owned by the current render operation and bypass persistent bitmap caches.

Text pixel parity also depends on GDI/FreeType outline topology and the
renderer's ordinary rasterization. No visual calibration compensates for these
differences. The regression tests use fixed RNG/offset vectors and explicit
ASS drawings to separate deterministic path behavior from font differences.
