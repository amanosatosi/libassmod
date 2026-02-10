# libassmod

⚠️ **Important notice**

This project is partially **vibecoded** and experimental.
Behavior may be incomplete, incorrect, or change without notice.

If you require exact VSFilterMod rendering accuracy, do **not** rely on
libassmod.

---

libassmod is a modified fork of **libass** that adds support for a selected
subset of VSFilterMod-style ASS extensions commonly used in advanced subtitle
typesetting.

It is intended as an **optional renderer**, not a replacement for libass.

---

## Why libassmod?

libass intentionally avoids supporting many non-standard ASS tags introduced by
VSFilterMod. While this keeps libass portable and maintainable, it also prevents
previewing or rendering modern anime typesetting that relies on those extensions.

libassmod exists to partially bridge that gap.

The goal is **practical usability**, not perfect compatibility.

---

## Scope and philosophy

- Renderer-side extensions only
- Support commonly used VSFilterMod-style tags
- Compatibility with standard ASS scripts is prioritized
- Graceful degradation for unsupported behavior
- **No attempt to fully replicate VSFilterMod**
- Accuracy may differ intentionally

This project favors **working previews** over exact matching.

---

## Supported VSFilterMod-style tags

### Typography & text attributes

- `\fsc<scale>`  
  Unified font scaling (equivalent to `\fscx` + `\fscy`)

- `\fsvp<spacing>`  
  Vertical spacing (leading)

- `\frs<angle>`  
  Baseline skew (shear)

  **Note:**  
  libassmod applies `\frs` behavior assuming **`\an5` anchoring**.
  This intentionally differs from VSFilterMod due to known inconsistencies and
  visual issues observed in VSFilterMod implementations.

- `\z<depth>`  
  Z-axis coordinate / pseudo depth

All of the above are animatable via `\t`.

---

### Transform & movement

- `\distort(u1,v1,u2,v2,u3,v3)`  
  Corner-pin distortion

- `\rnd<value>`  
- `\rndx<value>`  
- `\rndy<value>`  
- `\rndz<value>`  

  Randomized boundary deformation.

  **Note:**  
  `\rnd` behavior does **not** currently match VSFilterMod.
  Exact VSFilterMod-style randomness is difficult to reproduce and remains
  imperfect.

- `\jitter(left,right,up,down,period[,seed])`  
  Position jitter / shaking effect

- `\mover(x1,y1,x2,y2,angle1,angle2,radius1,radius2[,t1,t2])`  
  Polar / arc-based movement

- `\moves3(...)`  
- `\moves4(...)`  
  Spline-based movement

---

### Vector & clip control

- `\movevc(x1,y1[,x2,y2[,t1,t2]])`  
  Movable vector clip independent of main motion

---
### Color & transparency

- `\$vc(c1,c2,c3,c4)`  
  Per-vertex color gradients

- `\$va(a1,a2,a3,a4)`  
  Per-vertex alpha gradients

Both are animatable via `\t`.

Gradient rendering is implemented via a custom RGBA pipeline.
For implementation details and API notes, see:

https://github.com/amanosatosi/libassmod/blob/master/docs/rgba-rendering.md

---

### Image fill (RGBA pipeline)

- `\img(path[,xoffset,yoffset])` (alias of `\1img`)
- `\1img(path[,xoffset,yoffset])`
- `\2img(path[,xoffset,yoffset])`
- `\3img(path[,xoffset,yoffset])`
- `\4img(path[,xoffset,yoffset])`

Image fill is host-supplied: libassmod does not decode files by itself.
Applications register decoded RGBA images by path through the API, then `\img`
tags sample those images during rendering.

See integration details:

`docs/img-tags-host-api.md`

---

## Unsupported / not yet implemented

The following VSFilterMod-related features are **not supported**:

- Lua extensions  
  - `\lua(...)`
  - Inline Lua calls

- Projection & blur  
  - `\ortho0`, `\ortho1`
  - `\xblur`
  - `\yblur`

- Blend modes  
  - `\blend` and related variants

---

## Usage

libassmod is designed as a **drop-in alternative to libass**.

It can be built as a **separate shared library** (DLL / SO), allowing applications
to switch renderers without rebuilding.

---

## Aegisub integration

libassmod is bundled and tested with:

**Aegisub Toshi-ban v1.1**  
https://github.com/amanosatosi/Aegisub_Toshi-ban

This fork allows switching between upstream libass and libassmod for previewing
VSFilterMod-style effects.

---

## Compatibility notes

- Standard ASS scripts supported by libass should continue to work
- VSFilterMod-only scripts may render differently
- Visual output is **not guaranteed** to match VSFilterMod

---

## Status

⚠️ **Work in progress**

- APIs may change
- Features may be incomplete or incorrect
- Not recommended for production pipelines
