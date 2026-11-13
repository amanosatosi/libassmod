# Numeric expressions in ASS overrides

Use arithmetic in supported Mangetsu numeric override arguments to reuse
effects across dialogue events of different durations and sizes. This is a
read-only arithmetic evaluator. It does not implement the full Karaoke
Templater.

## Variables

| Variable | Value |
| --- | --- |
| `$start` | Event-local start, always 0 ms |
| `$mid` | Half the event duration in milliseconds |
| `$end`, `$dur` | Event duration in milliseconds |
| `$width`, `$height` | Width and height of the baseline layout rectangle |
| `$left`, `$right`, `$center` | Baseline horizontal edges and center |
| `$top`, `$bottom`, `$middle` | Baseline vertical edges and center |

Geometry uses absolute script coordinates with X increasing rightward and Y
downward. Timing is relative to the event, even when it starts later in the
subtitle timeline.

## Arithmetic and transform timing

Use decimal literals and the operators `+`, `-`, `*` and `/`.
Multiplication and division bind more tightly than addition and subtraction.
Operators of equal precedence associate left to right. Unary signs bind
before binary operators. Parentheses group subexpressions. Spaces and tabs
are optional. For example, `$width+20/2` adds 10, and
`($width+20)/2` divides the entire sum by two.

In the two time arguments of `\t`, a percentage literal denotes a
fraction of the **current event duration**. A 3,000 ms event gives 300 ms
for `10%`, 2,700 ms for `90%`, and 3,000 ms for `100%`.
A complete negative percentage literal such as `-10%` counts backward
from the event end. In arithmetic, unary minus retains its usual meaning:
`(-10%)` is −300 ms and `$end-10%` is 2,700 ms.
`$mid+10%` is 1,800 ms and `10%+500` is 800 ms.
Values outside 0–100% resolve arithmetically. Transform expressions must
fit signed 32-bit milliseconds and truncate fractional milliseconds toward
zero. An explicit expression ending at zero means zero. Legacy literal
`0` as an end time still means event duration. Existing transform
acceleration and ordering apply. An acceleration expression is unitless.

## Supported arguments

- `\t` times and acceleration. `\move` coordinates and times.
- `\pos`, `\org`, and rectangular `\clip`/`\iclip`.
- Scalar typography and effects such as `\fs`, `\fscx`, `\fscy`,
  `\fsp`, `\fsvp`, `\fshp`, `\bord`, `\xbord`,
  `\ybord`, `\shad`, `\xshad`, `\yshad`, `\blur`,
  `\xblur`, `\yblur`, `\be`, `\fr`/`\frz`,
  `\frx`, `\fry`, `\frs`, `\fax`, `\fay`, and `\z`.
- Numbered border sizes, blur and edge blur, box padding, ring sizes,
  corner radius, and all gradient-slide widths.
- Stop-gradient angles and positioned gradient coordinates, including
  `\pgrdalpha`. Stop offsets keep their existing trailing percent
  delimiter. For example, `($dur/60)%` places a stop at 50% on a 3-second event.

Use parentheses for scalar expressions, for example `\bord(2+2)`.
Plain signed literals keep their existing relative-number interpretation.
An expression is absolute unless it has the existing explicit relative
prefix `~+` or `~-` in an argument that supports relative numbers.
Numeric expressions leave color and alpha token grammars alone, including
`$white` and `$black`. A literal dollar sign in dialogue remains text.
Percentage-of-duration syntax is specific to `\t` times. Discrete modes,
karaoke times, vector drawing commands, image names, and ASS style-table
fields use their original grammar.

## Baseline layout and reusable styles

Timing-only effects resolve in the normal override pass. When an event
references layout variables, Mangetsu first measures it at event start
using static tags and inherited defaults, with transforms and numeric
expressions skipped. It uses the wrapped and aligned layout rectangle:
horizontal line advances and line metrics, or the completed column rectangle
for native vertical text. Drawings use the layout metrics of their paths.
Furigana contributes its positioned layout bounds. Supported curved text
uses its laid-out path bounds. Static positioning and script margins apply.
The rectangle precedes collision displacement, clipping, raster filters,
stroke and shadow expansion, and ordinary projected glyph rotation. Changes
to renderer layout settings can change measurement and rounding. Seeking or
animation progress alone does not change the baseline.

The renderer caches measured bounds and compiled expressions separately.
The same baseline is used even when an expression affects font size,
wrapping, or placement. If measurement is unavailable, geometry expressions
are ignored. Chat and scrolling layouts currently do not supply geometry
variables. Long events or unusual style contexts can bypass the bounds
cache. They use the same deterministic baseline policy.

In `mangetsu-style` comments, the global and per-target slide widths can
use expressions. A compiled expression is stored in style metadata and
evaluated with each event's own timing and bounds. An invalid result leaves
the last literal setting or the ordinary inherited default. Actor
Colorcoding and inline overrides keep their usual precedence. Bare and
named `\r` restore the applicable style defaults.

Malformed expressions, unknown variables, unavailable geometry, division
by zero, non-finite results, and values outside supported ranges leave
the affected numeric setting intact. Expressions are limited to 256
bytes, 64 instructions, and 16 levels of grouping or unary operators.
General override expression results are limited to a magnitude of
1,000,000 before tag-specific validation.

## Complete event examples

```ass
[Script Info]
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,42,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,2,0,2,20,20,20,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\grds($height/2)}
Dialogue: 0,0:00:01.00,0:00:04.00,Default,,0,0,0,,{\t($start+500,$end-500,\fscx150)}Timing variables
Dialogue: 0,0:00:04.00,0:00:07.00,Default,,0,0,0,,{\t(10%,-10%,\blur5)}Percentage timing
Dialogue: 0,0:00:07.00,0:00:10.00,Default,,0,0,0,,{\clip($left-10,$top-10,$right+10,$bottom+10)\t(0%,100%,\1grd(90,&HFF0000&,&H0000FF&))}A clipped wipe
```
