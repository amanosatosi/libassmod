/*
 * Regression coverage for Mangetsu RGBA output ownership. This deliberately
 * includes the auto API's legacy-only cleanup path, because it still builds
 * and destroys RGBA tiles on every frame.
 */

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ass.h"
#include "ass_render.h"

enum {
    WIDTH = 640,
    HEIGHT = 360,
    NORMAL_STRESS_FRAMES = 1000,
    NORMAL_RENDERER_CYCLES = 3,
    EXTENDED_STRESS_FRAMES = 3000,
    EXTENDED_RENDERER_CYCLES = 4,
    AUTO_PROGRESS_INTERVAL = 25,
    AUTO_VERBOSE_FRAMES = 3,
};
static const uint32_t stress_seed = UINT32_C(0x4d616e67);

static const char stress_script[] =
    "[Script Info]\n"
    "ScriptType: v4.00+\n"
    "PlayResX: 640\n"
    "PlayResY: 360\n"
    "ScaledBorderAndShadow: yes\n"
    "\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
    "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
    "Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Arial,42,&H00FFFFFF,&H0000FFFF,&H00000000,&H50000000,0,0,0,0,100,100,0,0,1,3,2,2,10,10,10,1\n"
    "\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
    "Dialogue: 0,0:00:00.00,0:00:02.50,Default,,0,0,0,,{\\an7\\pos(8,8)}persistent legacy logo\n"
    "Dialogue: 0,0:00:00.00,0:00:02.50,Default,,0,0,0,,{\\an7\\pos(8,48)}persistent legacy logo two\n"
    "Dialogue: 0,0:00:00.00,0:00:02.50,Default,,0,0,0,,{\\an7\\pos(8,88)}persistent legacy logo three\n"
    "Dialogue: 0,0:00:00.00,0:00:02.50,Default,,0,0,0,,{\\an7\\pos(8,128)}persistent legacy logo four\n"
    "Dialogue: 1,0:00:00.50,0:00:01.00,Default,,0,0,0,,{\\an5\\pos(320,180)\\pgrd(100,100,540,260,0,&H000000&,&HFFFFFF&)}positioned gradient\n"
    "Dialogue: 2,0:00:01.00,0:00:01.50,Default,,0,0,0,,{\\an5\\pos(320,180)\\img(texture.png,1,2)}image-filled text\n"
    "Dialogue: 3,0:00:01.50,0:00:02.00,Default,,0,0,0,,{\\an7\\pos(-20,90)\\1vc(&H0000FF&,&H00FF00&,&HFF0000&,&HFFFFFF&)\\clip(m 0 0 l 640 0 640 360)}vector-clipped gradient\n"
    "Dialogue: 4,0:00:02.00,0:00:02.50,Default,,0,0,0,,{\\an5\\pos(320,180)\\bs4\\boxp16\\1bs3\\1bbc&HFFFFFF&\\2bs5\\2bbc&H000000&\\distort(1,0,1.25,1,-0.2,1)}BS4 distortion\n";

static void msg_cb(int level, const char *fmt, va_list va, void *data)
{
    (void) level;
    (void) fmt;
    (void) va;
    (void) data;
}

static bool expect(bool condition, const char *message)
{
    if (!condition)
        fprintf(stderr, "%s (seed=0x%08x)\n", message, (unsigned) stress_seed);
    return condition;
}

static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static double elapsed_seconds(clock_t start)
{
    clock_t now = clock();
    if (now == (clock_t) -1 || start == (clock_t) -1)
        return 0.0;
    return (double) (now - start) / CLOCKS_PER_SEC;
}

static void report_phase(const char *phase, clock_t start)
{
    fprintf(stderr, "phase: %s complete, %.1fs\n", phase,
            elapsed_seconds(start));
}

static void report_auto_progress(int completed_frames, int stress_frames,
                                 clock_t stress_start, clock_t block_start,
                                 uint64_t *block_registry_scans,
                                 uint64_t *block_registry_scan_steps)
{
    size_t live_allocations;
    uint64_t registry_scans, registry_scan_steps;
    ass_rgba_debug_allocation_stats(&live_allocations, &registry_scans,
                                    &registry_scan_steps);
    fprintf(stderr,
            "auto lifetime stress: frame %d/%d complete, elapsed %.1fs, "
            "block %.1fs, live registry entries=%zu, registry scans=%" PRIu64
            " (+%" PRIu64 "), scan steps=%" PRIu64 " (+%" PRIu64 ")\n",
            completed_frames, stress_frames, elapsed_seconds(stress_start),
            elapsed_seconds(block_start),
            live_allocations, registry_scans,
            registry_scans - *block_registry_scans, registry_scan_steps,
            registry_scan_steps - *block_registry_scan_steps);
    *block_registry_scans = registry_scans;
    *block_registry_scan_steps = registry_scan_steps;
}

static void fill_opaque(ASS_ImageRGBAPriv *priv)
{
    memset(priv->buffer, 0xFF, priv->alloc_size);
}

static bool test_clipped_buffer_ownership(ASS_Renderer *renderer)
{
    bool ok = true;
    ASS_ImageRGBA *img = ass_rgba_image_alloc(
        renderer, 24, 20, -6, -5, IMAGE_TYPE_CHARACTER,
        ASS_RGBA_OWNER_EVENT, "ownership selftest clipped image");
    ok &= expect(img != NULL, "could not allocate clipped ownership image");
    if (!img)
        return false;

    ASS_ImageRGBAPriv *priv = ass_rgba_image_private(
        img, "ownership selftest clipped image");
    uint8_t *base = priv->buffer;
    fill_opaque(priv);
    ok &= expect(ass_rgba_image_clip_to_frame(renderer, img),
                 "clipped ownership image unexpectedly disappeared");
    ok &= expect(img->rgba != base,
                 "clipping did not advance the public RGBA view pointer");
    ok &= expect(ass_rgba_image_view_valid(img, "ownership selftest clipped view"),
                 "clipped public RGBA view was not contained by its allocation");
    ass_free_images_rgba(img);

    img = ass_rgba_image_alloc(renderer, 8, 8, -100, -100,
                               IMAGE_TYPE_CHARACTER, ASS_RGBA_OWNER_EVENT,
                               "ownership selftest off-frame image");
    ok &= expect(img != NULL, "could not allocate off-frame ownership image");
    if (img) {
        fill_opaque(ass_rgba_image_private(img, "ownership selftest off-frame image"));
        ok &= expect(!ass_rgba_image_clip_to_frame(renderer, img),
                     "fully off-frame RGBA image survived clipping");
        ass_free_images_rgba(img);
    }

    img = ass_rgba_image_alloc(renderer, 24, 20, -6, -5,
                               IMAGE_TYPE_CHARACTER, ASS_RGBA_OWNER_EVENT,
                               "ownership selftest replacement image");
    ok &= expect(img != NULL, "could not allocate replacement ownership image");
    if (!img)
        return false;

    priv = ass_rgba_image_private(img, "ownership selftest replacement image");
    fill_opaque(priv);
    ok &= expect(ass_rgba_image_clip_to_frame(renderer, img),
                 "replacement image unexpectedly disappeared after clipping");
    ok &= expect(img->rgba != priv->buffer,
                 "replacement test did not create an offset public view");
    ok &= expect(ass_rgba_image_view_valid(img, "ownership selftest replacement view"),
                 "replacement source view was not contained by its allocation");

    for (int i = 0; i < 16; i++) {
        int stride;
        size_t alloc_size;
        uint8_t *buffer = ass_rgba_alloc_buffer(
            renderer, img->w, img->h, priv->alloc_size, &stride, &alloc_size,
            "ownership selftest replacement buffer");
        ok &= expect(buffer != NULL, "could not allocate replacement buffer");
        if (!buffer)
            break;
        memset(buffer, 0xFF, alloc_size);
        ass_rgba_image_replace_buffer(img, buffer, alloc_size,
                                      img->w, img->h, stride);
        priv = ass_rgba_image_private(img, "ownership selftest replacement result");
        ok &= expect(img->rgba == priv->buffer,
                     "replacement did not restore the public view to its base");
    }
    ass_free_images_rgba(img);
    return ok;
}

static bool test_allocation_failures(ASS_Renderer *renderer)
{
    bool ok = true;
    ass_set_cache_limits(renderer, 0, 1);
    ASS_ImageRGBA *too_large = ass_rgba_image_alloc(
        renderer, 1024, 1024, 0, 0, IMAGE_TYPE_CHARACTER,
        ASS_RGBA_OWNER_EVENT, "ownership selftest allocation failure");
    ok &= expect(too_large == NULL,
                 "RGBA image allocation limit did not reject oversized image");
    ass_free_images_rgba(too_large);

    ASS_ImageRGBA *img = ass_rgba_image_alloc(
        renderer, 16, 16, 0, 0, IMAGE_TYPE_CHARACTER,
        ASS_RGBA_OWNER_EVENT, "ownership selftest replacement failure image");
    ok &= expect(img != NULL, "could not allocate replacement failure image");
    if (img) {
        ASS_ImageRGBAPriv *priv = ass_rgba_image_private(
            img, "ownership selftest replacement failure image");
        uint8_t *old_buffer = priv->buffer;
        size_t old_size = priv->alloc_size;
        int old_w = img->w, old_h = img->h, old_stride = img->stride;
        int stride;
        size_t alloc_size;
        uint8_t *buffer = ass_rgba_alloc_buffer(
            renderer, 1024, 1024, old_size, &stride, &alloc_size,
            "ownership selftest replacement failure");
        ok &= expect(buffer == NULL,
                     "RGBA replacement allocation limit did not reject oversized buffer");
        ok &= expect(priv->buffer == old_buffer && priv->alloc_size == old_size &&
                     img->w == old_w && img->h == old_h && img->stride == old_stride,
                     "failed RGBA replacement changed the existing image");
        ass_free_images_rgba(img);
    }
    ass_set_cache_limits(renderer, 0, 0);
    return ok;
}

static bool test_auto_lifetimes(ASS_Library *library, ASS_Renderer *renderer,
                                int stress_frames)
{
    uint8_t texture[4 * 4 * 4];
    for (int i = 0; i < (int) sizeof(texture); i++)
        texture[i] = (uint8_t) (i * 37 + 11);
    for (int i = 3; i < (int) sizeof(texture); i += 4)
        texture[i] = 255;
    if (ass_set_tag_image_rgba(renderer, "texture.png", ASS_TAG_IMAGE_FORMAT_PNG,
                               4, 4, 16, texture) < 0)
        return expect(false, "could not register RGBA texture");

    ASS_Track *track = ass_read_memory(library, stress_script,
                                       sizeof(stress_script) - 1, NULL);
    if (!track)
        return expect(false, "could not create RGBA ownership stress track");

    bool ok = true;
    uint32_t random = stress_seed;
    clock_t stress_start = clock();
    clock_t block_start = stress_start;
    uint64_t block_registry_scans, block_registry_scan_steps;
    ass_rgba_debug_allocation_stats(NULL, &block_registry_scans,
                                    &block_registry_scan_steps);
    fprintf(stderr,
            "auto lifetime stress: frame 0/%d, elapsed 0.0s, "
            "live registry entries=%zu\n",
            stress_frames, ass_rgba_debug_live_allocation_count());
    for (int frame = 0; frame < stress_frames; frame++) {
        uint32_t value = next_random(&random);
        int width = value & 1 ? WIDTH : WIDTH / 2;
        int height = value & 2 ? HEIGHT : HEIGHT / 2;
        ass_set_storage_size(renderer, width, height);
        ass_set_frame_size(renderer, width, height);
        /* Start with guaranteed legacy/RGBA alternation, then randomize the
         * remaining timeline while retaining this seed as a reproducer. */
        long long now = frame < 64 ? (frame & 1 ? 750 : 250) :
                        frame == 64 ? 3000 : value % 3500;

        if (frame < AUTO_VERBOSE_FRAMES)
            fprintf(stderr, "frame %d: before ass_render_frame_auto\n", frame);
        ASS_RenderResult result = ass_render_frame_auto(renderer, track, now, NULL);
        if (frame < AUTO_VERBOSE_FRAMES)
            fprintf(stderr,
                    "frame %d: after ass_render_frame_auto, "
                    "live registry entries=%zu\n",
                    frame, ass_rgba_debug_live_allocation_count());
        if (frame < 64)
            ok &= expect(result.use_rgba == (frame & 1),
                         "legacy/RGBA auto alternation selected the wrong output");
        if (frame == 64)
            ok &= expect(!result.use_rgba && result.imgs == NULL,
                         "empty frame retained subtitle output");
        if (result.use_rgba) {
            if (frame < AUTO_VERBOSE_FRAMES)
                fprintf(stderr, "frame %d: before result cleanup (caller RGBA list)\n",
                        frame);
            ok &= expect(result.imgs_rgba != NULL,
                         "RGBA-required auto frame returned no RGBA list");
            ass_free_images_rgba(result.imgs_rgba);
        } else {
            if (frame < AUTO_VERBOSE_FRAMES)
                fprintf(stderr,
                        "frame %d: before result cleanup "
                        "(legacy RGBA cleanup was internal)\n",
                        frame);
            ok &= expect(result.imgs_rgba == NULL,
                         "legacy-only auto frame retained an RGBA list");
        }
        if (frame < AUTO_VERBOSE_FRAMES)
            fprintf(stderr,
                    "frame %d: after result cleanup, live registry entries=%zu\n",
                    frame, ass_rgba_debug_live_allocation_count());

        /* Exercise caller-side cleanup independently of auto mode. */
        if ((frame & 15) == 0) {
            if (frame < AUTO_VERBOSE_FRAMES)
                fprintf(stderr, "frame %d: before ass_render_frame_rgba\n", frame);
            ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, now, NULL);
            if (frame < AUTO_VERBOSE_FRAMES)
                fprintf(stderr,
                        "frame %d: after ass_render_frame_rgba, "
                        "live registry entries=%zu\n",
                        frame, ass_rgba_debug_live_allocation_count());
            if (frame < AUTO_VERBOSE_FRAMES)
                fprintf(stderr, "frame %d: before direct RGBA cleanup\n", frame);
            ass_free_images_rgba(images);
            if (frame < AUTO_VERBOSE_FRAMES)
                fprintf(stderr,
                        "frame %d: after direct RGBA cleanup, "
                        "live registry entries=%zu\n",
                        frame, ass_rgba_debug_live_allocation_count());
        }
        if ((frame + 1) % AUTO_PROGRESS_INTERVAL == 0 ||
            frame + 1 == stress_frames || !ok) {
            report_auto_progress(frame + 1, stress_frames, stress_start,
                                 block_start, &block_registry_scans,
                                 &block_registry_scan_steps);
            block_start = clock();
        }
        if (!ok)
            break;
    }

    ass_free_track(track);
    return ok;
}

int main(int argc, char *argv[])
{
    setvbuf(stderr, NULL, _IONBF, 0);
    bool extended = false;
    if (argc == 2 && !strcmp(argv[1], "--extended"))
        extended = true;
    else if (argc != 1) {
        fprintf(stderr, "usage: %s [--extended]\n", argv[0]);
        return 2;
    }

    int stress_frames = extended ? EXTENDED_STRESS_FRAMES : NORMAL_STRESS_FRAMES;
    int renderer_cycles = extended ? EXTENDED_RENDERER_CYCLES :
                                     NORMAL_RENDERER_CYCLES;
    bool ok = true;
    fprintf(stderr, "rgba ownership stress seed=0x%08x\n", (unsigned) stress_seed);
    fprintf(stderr, "rgba ownership stress workload=%d frames x %d cycles\n",
            stress_frames, renderer_cycles);
    for (int cycle = 0; cycle < renderer_cycles; cycle++) {
        clock_t cycle_start = clock();
        fprintf(stderr, "cycle %d/%d start\n", cycle + 1, renderer_cycles);
        ASS_Library *library = ass_library_init();
        if (!library)
            return 1;
        ass_set_message_cb(library, msg_cb, NULL);

        ASS_Renderer *renderer = ass_renderer_init(library);
        if (!renderer) {
            ass_library_done(library);
            return 1;
        }
        ass_set_storage_size(renderer, WIDTH, HEIGHT);
        ass_set_frame_size(renderer, WIDTH, HEIGHT);
        ass_set_fonts(renderer, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT,
                      NULL, 1);

        clock_t phase_start = clock();
        fprintf(stderr, "phase: direct ownership tests start\n");
        ok &= test_clipped_buffer_ownership(renderer);
        report_phase("direct ownership tests", phase_start);

        phase_start = clock();
        fprintf(stderr, "phase: allocation failure tests start\n");
        ok &= test_allocation_failures(renderer);
        report_phase("allocation failure tests", phase_start);

        phase_start = clock();
        fprintf(stderr, "phase: auto lifetime stress start\n");
        ok &= test_auto_lifetimes(library, renderer, stress_frames);
        report_phase("auto lifetime stress", phase_start);

        ass_renderer_done(renderer);
        ass_library_done(library);
        fprintf(stderr, "cycle %d/%d complete, %.1fs\n", cycle + 1,
                renderer_cycles, elapsed_seconds(cycle_start));
        if (!ok)
            break;
    }
    return ok ? 0 : 1;
}
