# Mangetsu perspective

The event-wide override is \mpersp(u0,v0,u1,v1,u2,v2,u3,v3). Corner
order is top-left, top-right, bottom-right, bottom-left. The numbers are
normalized coordinates in the event's current logical layout rectangle.
The identity values are (0,0,1,0,1,1,0,1). The last valid override in the
event applies to every visual line, including furigana. A style reset \r
keeps the event transform. Use \mpersp() to restore identity.

    {\an5\pos(640,340)\mpersp(0.12,0.02,1.05,0,0.76,0.9,0.23,1)}Editable text
    {\mpersp()\t(0,1600,\mpersp(0.15,0,0.92,0.1,1.1,1,-0.1,0.93))}Animated corners

Mangetsu uses a new tag name to avoid the drawing-mode prefix \p on
older ASS renderers. The parser accepts eight finite signed decimal values
or empty parentheses, including inside \t. Edits to text, font, spacing,
scaling and wrapping recompute the reference rectangle. The tag stores
only the pins.

Existing layout, baseline transforms, \distort, and ASS rotation run
before the corner pin. Borders use the same projected geometry. Ordinary
shadow displacement, blur and gradient sampling keep their screen-space
semantics. Frame-space clips still use frame coordinates. Furigana shares
the base text's event transform. Existing alignment and collision placement
use the original event layout.

Invalid or crossed quads, unsafe projections, and over-budget masks use
the ordinary event geometry for that frame. This also applies to intermediate
quads during animation. Chat, scrolling and outline randomization currently
use the existing renderer geometry even when the event has perspective.
Existing \distort operations keep their bilinear behavior.

Hosts may call ass_get_perspective_layout immediately after rendering to
read the reference rectangle and the evaluated pins in frame pixels. The
pins describe the authored target even when a frame falls back.
