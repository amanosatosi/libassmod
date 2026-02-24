# VSFilterMod `\ortho` override tag

This fork implements VSFilterMod-style orthogonal projection control via
`\ortho0` and `\ortho1`.

## User-facing behavior

- **Name:** Orthogonal projection
- **Syntax:** `\ortho<0|1>`
- **Modes:**
  - `\ortho0`: perspective projection (default)
  - `\ortho1`: orthogonal projection
- **Default/reset:** If the tag is not present, projection is perspective.
  `\r` resets back to perspective.
- **Invalid/empty argument:** Falls back to default perspective behavior.

In `\ortho1`, text still responds to `\frx`, `\fry`, `\frz`, `\fax`, `\fay`,
and `\z`, but without perspective depth scaling (no z-based divide).

## Examples

- `{\ortho1\frx45\fry45}ORTHO1`
- `{\ortho0\frx45\fry45}ORTHO0`
- `{\ortho1\frx45\fry45\t(0,5000,\fry405)}ORTHO1_ANIM`

## Notes

- `\ortho` is a VSFilterMod-specific extension and is not part of standard ASS.
- The tag is a discrete projection toggle, not a continuous interpolation value.
