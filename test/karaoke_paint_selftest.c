#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ass.h"

#define W 384
#define H 216

typedef struct {
    uint32_t *r, *g, *b, *a;
} Frame;

static const char header[] =
    "[Script Info]\n"
    "ScriptType: v4.00+\n"
    "PlayResX: 384\n"
    "PlayResY: 216\n"
    "ScaledBorderAndShadow: yes\n\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
    "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
    "Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Arial,48,&H00FFFFFF,&H00FF0000,&H00000000,&H00000000,"
    "0,0,0,0,100,100,0,0,1,0,0,5,20,20,20,1\n\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

static bool alloc_frame(Frame *f)
{
    *f = (Frame) {0};
    f->r = calloc(W * H, sizeof(*f->r));
    f->g = calloc(W * H, sizeof(*f->g));
    f->b = calloc(W * H, sizeof(*f->b));
    f->a = calloc(W * H, sizeof(*f->a));
    return f->r && f->g && f->b && f->a;
}

static void free_frame(Frame *f)
{
    free(f->r); free(f->g); free(f->b); free(f->a);
    *f = (Frame) {0};
}

static char *make_script(const char *events)
{
    size_t size = strlen(header) + strlen(events) + 2;
    char *script = malloc(size);
    if (script)
        snprintf(script, size, "%s%s\n", header, events);
    return script;
}

static bool render_events(ASS_Library *lib, ASS_Renderer *renderer,
                          const char *events, long long now, Frame *frame)
{
    if (!alloc_frame(frame)) {
        free_frame(frame);
        return false;
    }
    char *script = make_script(events);
    if (!script)
        goto fail;
    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    free(script);
    if (!track)
        goto fail;

    ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, now, NULL);
    for (ASS_ImageRGBA *img = images; img; img = img->next) {
        for (int y = 0; y < img->h; y++) {
            int yy = img->dst_y + y;
            if (yy < 0 || yy >= H)
                continue;
            const uint8_t *row = img->rgba + y * img->stride;
            for (int x = 0; x < img->w; x++) {
                int xx = img->dst_x + x;
                if (xx < 0 || xx >= W)
                    continue;
                int off = yy * W + xx;
                frame->r[off] += row[4 * x + 0];
                frame->g[off] += row[4 * x + 1];
                frame->b[off] += row[4 * x + 2];
                frame->a[off] += row[4 * x + 3];
            }
        }
    }
    ass_free_images_rgba(images);
    ass_free_track(track);
    return true;

fail:
    free_frame(frame);
    return false;
}

static bool render_text(ASS_Library *lib, ASS_Renderer *renderer,
                        const char *text, long long now, Frame *frame)
{
    const char *prefix =
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,";
    size_t size = strlen(prefix) + strlen(text) + 1;
    char *event = malloc(size);
    if (!event)
        return false;
    snprintf(event, size, "%s%s", prefix, text);
    bool ok = render_events(lib, renderer, event, now, frame);
    free(event);
    return ok;
}

static uint64_t sum_plane(const uint32_t *p, int x0, int y0, int x1, int y1)
{
    uint64_t sum = 0;
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > W ? W : x1; y1 = y1 > H ? H : y1;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            sum += p[y * W + x];
    return sum;
}

static uint64_t total_alpha(const Frame *f)
{
    return sum_plane(f->a, 0, 0, W, H);
}

static bool same_frame(const Frame *a, const Frame *b)
{
    size_t size = W * H * sizeof(*a->a);
    return !memcmp(a->r, b->r, size) && !memcmp(a->g, b->g, size) &&
           !memcmp(a->b, b->b, size) && !memcmp(a->a, b->a, size);
}

static bool frame_bounds(const Frame *f, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = W; *y0 = H; *x1 = *y1 = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (f->a[y * W + x]) {
                if (x < *x0) *x0 = x;
                if (y < *y0) *y0 = y;
                if (x + 1 > *x1) *x1 = x + 1;
                if (y + 1 > *y1) *y1 = y + 1;
            }
    return *x0 < *x1 && *y0 < *y1;
}

static double centroid_x(const uint32_t *p, int x0, int y0, int x1, int y1,
                         uint64_t *coverage)
{
    uint64_t sum = 0;
    long double weighted = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t value = p[y * W + x];
            sum += value;
            weighted += (x + 0.5) * value;
        }
    *coverage = sum;
    return sum ? (double) (weighted / sum) : 0;
}

static double delta_centroid_x(const uint32_t *before, const uint32_t *after,
                               int x0, int y0, int x1, int y1,
                               uint64_t *coverage)
{
    uint64_t sum = 0;
    long double weighted = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int off = y * W + x;
            uint32_t value = after[off] > before[off] ?
                after[off] - before[off] : 0;
            sum += value;
            weighted += (x + 0.5) * value;
        }
    *coverage = sum;
    return sum ? (double) (weighted / sum) : 0;
}

static int last_col(const uint32_t *p)
{
    for (int x = W - 1; x >= 0; x--)
        if (sum_plane(p, x, 0, x + 1, H))
            return x;
    return -1;
}

static int expect(bool ok, const char *message)
{
    if (!ok)
        fprintf(stderr, "%s\n", message);
    return ok ? 0 : 1;
}

static int test_secondary_outline(ASS_Library *lib, ASS_Renderer *renderer)
{
    int fail = 0;
    Frame legacy0 = {0}, legacy1 = {0};
    bool ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord6\\3c&HFF0000&"
        "\\kt50\\k100}A", 499, &legacy0) &&
        render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord6\\3c&HFF0000&"
        "\\kt50\\k100}A", 500, &legacy1);
    fail |= expect(ok && same_frame(&legacy0, &legacy1),
                   "ordinary \\k changed outline without a secondary source");
    free_frame(&legacy0); free_frame(&legacy1);

    Frame legacy_kf0 = {0}, legacy_kf1 = {0};
    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord6\\3c&HFF0000&"
        "\\kf100}A", 250, &legacy_kf0) &&
        render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord6\\3c&HFF0000&"
        "\\kf100}A", 750, &legacy_kf1);
    fail |= expect(ok && same_frame(&legacy_kf0, &legacy_kf1),
                   "ordinary \\kf changed outline without a secondary source");
    free_frame(&legacy_kf0); free_frame(&legacy_kf1);

    const char *solid =
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord6"
        "\\3c&HFF0000&\\3sc&H0000FF&\\kt50\\k100}WW";
    Frame waiting = {0}, active = {0};
    ok = render_text(lib, renderer, solid, 499, &waiting) &&
         render_text(lib, renderer, solid, 500, &active);
    uint64_t wr = ok ? sum_plane(waiting.r, 0, 0, W, H) : 0;
    uint64_t wb = ok ? sum_plane(waiting.b, 0, 0, W, H) : 0;
    uint64_t ar = ok ? sum_plane(active.r, 0, 0, W, H) : 0;
    uint64_t ab = ok ? sum_plane(active.b, 0, 0, W, H) : 0;
    fail |= expect(ok && wr > 3 * wb && ab > 3 * ar,
                   "\\3sc did not switch to the active \\3c at \\k activation");
    free_frame(&waiting); free_frame(&active);

    const char *kf =
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord6"
        "\\3c&HFF0000&\\3sc&H0000FF&\\kt10\\kf100}WW";
    Frame q1 = {0}, q3 = {0};
    ok = render_text(lib, renderer, kf, 350, &q1) &&
         render_text(lib, renderer, kf, 850, &q3);
    uint64_t q1r = ok ? sum_plane(q1.r, 0, 0, W, H) : 0;
    uint64_t q1b = ok ? sum_plane(q1.b, 0, 0, W, H) : 0;
    uint64_t q3r = ok ? sum_plane(q3.r, 0, 0, W, H) : 0;
    uint64_t q3b = ok ? sum_plane(q3.b, 0, 0, W, H) : 0;
    fail |= expect(ok && q1r && q1b && q3r < q1r && q3b > q1b,
                   "\\kf did not progressively split active/secondary outline paint");
    free_frame(&q1); free_frame(&q3);

    Frame rot25 = {0}, rot50 = {0}, rot75 = {0};
    const char *rotated_kf =
        "{\\an1\\pos(220,180)\\frz180\\1a&HFF&\\2a&HFF&\\bord6"
        "\\3c&HFF0000&\\3sc&H0000FF&\\kf100}WW";
    ok = render_text(lib, renderer, rotated_kf, 250, &rot25) &&
         render_text(lib, renderer, rotated_kf, 500, &rot50) &&
         render_text(lib, renderer, rotated_kf, 750, &rot75);
    uint64_t rd1 = 0, rd2 = 0, rd3 = 0;
    double rx1 = ok ? centroid_x(rot25.b, 0, 0, W, H, &rd1) : 0;
    double rx2 = ok ? delta_centroid_x(rot25.b, rot50.b, 0, 0, W, H,
                                       &rd2) : 0;
    double rx3 = ok ? delta_centroid_x(rot50.b, rot75.b, 0, 0, W, H,
                                       &rd3) : 0;
    fail |= expect(ok && rd1 && rd2 && rd3 && rx1 > rx2 && rx2 > rx3,
                   "rotated secondary outline \\kf direction differed from fill");
    free_frame(&rot25); free_frame(&rot50); free_frame(&rot75);

    Frame thin = {0}, thick = {0};
    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord2\\3c&HFF0000&"
        "\\3sc&H0000FF&\\kf100}H", 500, &thin) &&
        render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord12\\3c&HFF0000&"
        "\\3sc&H0000FF&\\kf100}H", 500, &thick);
    fail |= expect(ok && abs(last_col(thin.b) - last_col(thick.b)) <= 1,
                   "outline thickness changed the logical \\kf frontier");
    free_frame(&thin); free_frame(&thick);

    const char *vector_k =
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord8\\3c&HFF0000&"
        "\\3svc(&H0000FF&,&H00FF00&,&H0000FF&,&H00FF00&)\\kt50\\k100}WW";
    const char *vector_kf =
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord8\\3c&HFF0000&"
        "\\3svc(&H0000FF&,&H00FF00&,&H0000FF&,&H00FF00&)\\kf100}WW";
    Frame vk0 = {0}, vk1 = {0}, vkf = {0};
    ok = render_text(lib, renderer, vector_k, 499, &vk0) &&
         render_text(lib, renderer, vector_k, 500, &vk1) &&
         render_text(lib, renderer, vector_kf, 500, &vkf);
    bool vector_rgba = ass_frame_needs_rgba(renderer) != 0;
    fail |= expect(ok && sum_plane(vk0.r, 0, 0, W, H) &&
                   sum_plane(vk0.g, 0, 0, W, H) &&
                   sum_plane(vk1.b, 0, 0, W, H) &&
                   sum_plane(vkf.r, 0, 0, W, H) &&
                   sum_plane(vkf.b, 0, 0, W, H) && vector_rgba,
                   "\\3svc did not participate in \\k and \\kf outline karaoke");
    free_frame(&vk0); free_frame(&vk1); free_frame(&vkf);

    const char *gradient_k =
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord8\\3c&HFF0000&"
        "\\3sgrd(0,&H0000FF&,&H00FF00&)\\kt50\\k100}WW";
    const char *gradient_kf =
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord8\\3c&HFF0000&"
        "\\3sgrd(0,&H0000FF&,&H00FF00&)\\kf100}WW";
    Frame gk0 = {0}, gk1 = {0}, gkf = {0};
    ok = render_text(lib, renderer, gradient_k, 499, &gk0) &&
         render_text(lib, renderer, gradient_k, 500, &gk1) &&
         render_text(lib, renderer, gradient_kf, 500, &gkf);
    bool gradient_rgba = ass_frame_needs_rgba(renderer) != 0;
    fail |= expect(ok && sum_plane(gk0.r, 0, 0, W, H) &&
                   sum_plane(gk0.g, 0, 0, W, H) &&
                   sum_plane(gk1.b, 0, 0, W, H) &&
                   sum_plane(gkf.r, 0, 0, W, H) &&
                   sum_plane(gkf.b, 0, 0, W, H) && gradient_rgba,
                   "\\3sgrd did not participate in \\k and \\kf outline karaoke");
    free_frame(&gk0); free_frame(&gk1); free_frame(&gkf);

    Frame precedence = {0};
    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord8\\3sc&H0000FF&"
        "\\3svc(&H00FF00&,&H00FF00&,&H00FF00&,&H00FF00&)"
        "\\kt50\\k100}A", 499, &precedence);
    fail |= expect(ok && sum_plane(precedence.g, 0, 0, W, H) &&
                   !sum_plane(precedence.r, 0, 0, W, H),
                   "later secondary vector source did not replace \\3sc");
    free_frame(&precedence);

    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord8"
        "\\3svc(&H00FF00&,&H00FF00&,&H00FF00&,&H00FF00&)"
        "\\3sgrd(0,&H0000FF&,&H0000FF&)\\kt50\\k100}A",
        499, &precedence);
    fail |= expect(ok && sum_plane(precedence.r, 0, 0, W, H) &&
                   !sum_plane(precedence.g, 0, 0, W, H),
                   "later secondary Mangetsu gradient did not replace \\3svc");
    free_frame(&precedence);

    Frame ko0 = {0}, ko1 = {0};
    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord8\\3sc&H0000FF&"
        "\\kt50\\ko100}A", 499, &ko0) &&
        render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord8\\3sc&H0000FF&"
        "\\kt50\\ko100}A", 500, &ko1);
    fail |= expect(ok && !total_alpha(&ko0) && total_alpha(&ko1),
                   "lowercase \\ko exposed a configured secondary outline");
    free_frame(&ko0); free_frame(&ko1);

    Frame reset = {0}, baseline = {0};
    ok = render_text(lib, renderer,
        "{\\3sc&H0000FF&\\r\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&"
        "\\bord6\\3c&HFF0000&\\kt50\\k100}A", 499, &reset) &&
        render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord6\\3c&HFF0000&"
        "\\kt50\\k100}A", 499, &baseline);
    fail |= expect(ok && same_frame(&reset, &baseline),
                   "\\r did not clear secondary outline karaoke state");
    free_frame(&reset); free_frame(&baseline);

    ok = render_text(lib, renderer,
        "{\\3sc&H0000FF&\\rDefault\\an1\\pos(40,180)\\1a&HFF&"
        "\\2a&HFF&\\bord6\\3c&HFF0000&\\kt50\\k100}A", 499,
        &reset) && render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord6\\3c&HFF0000&"
        "\\kt50\\k100}A", 499, &baseline);
    fail |= expect(ok && same_frame(&reset, &baseline),
                   "\\rStyleName did not clear secondary outline karaoke state");
    free_frame(&reset); free_frame(&baseline);

    const char *two_events =
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,"
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord6\\3c&HFF0000&"
        "\\3sc&H0000FF&\\kt50\\k100}A\n"
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,"
        "{\\an1\\pos(240,180)\\1a&HFF&\\2a&HFF&\\bord6\\3c&HFF0000&"
        "\\kt50\\k100}A";
    Frame events = {0};
    ok = render_events(lib, renderer, two_events, 499, &events);
    uint64_t left_red = ok ? sum_plane(events.r, 0, 0, 192, H) : 0;
    uint64_t right_red = ok ? sum_plane(events.r, 192, 0, W, H) : 0;
    uint64_t right_blue = ok ? sum_plane(events.b, 192, 0, W, H) : 0;
    fail |= expect(ok && left_red && !right_red && right_blue,
                   "secondary outline state leaked between events");
    free_frame(&events);
    return fail;
}

static int test_reveal(ASS_Library *lib, ASS_Renderer *renderer)
{
    int fail = 0;
    const char *abc = "{\\an1\\pos(40,180)\\kO20}A{\\kO20}B{\\kO20}C";
    Frame f0 = {0}, f199 = {0}, f200 = {0}, f400 = {0};
    bool ok = render_text(lib, renderer, abc, 0, &f0) &&
              render_text(lib, renderer, abc, 199, &f199) &&
              render_text(lib, renderer, abc, 200, &f200) &&
              render_text(lib, renderer, abc, 400, &f400);
    fail |= expect(ok && total_alpha(&f0) &&
                   total_alpha(&f199) == total_alpha(&f0) &&
                   total_alpha(&f200) > total_alpha(&f199) &&
                   total_alpha(&f400) > total_alpha(&f200),
                   "\\kO did not reveal whole segments at their exact starts");
    free_frame(&f0); free_frame(&f199); free_frame(&f200); free_frame(&f400);

    const char *hidden =
        "{\\an1\\pos(40,160)\\kt50\\kO100\\2c&H0000FF&\\bord8"
        "\\3sc&H00FF00&\\shad18\\2bs5\\2bc&HFF0000&"
        "\\3bs5\\3bc&H0000FF&}A";
    Frame before = {0}, after = {0};
    ok = render_text(lib, renderer, hidden, 499, &before) &&
         render_text(lib, renderer, hidden, 500, &after);
    fail |= expect(ok && !total_alpha(&before) && total_alpha(&after),
                   "unreached \\kO leaked fill, outline, multi-border, or shadow");
    free_frame(&before); free_frame(&after);

    Frame shadow0 = {0}, shadow1 = {0};
    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,150)\\1a&HFF&\\2a&HFF&\\bord0\\shad18"
        "\\kt50\\kO20}A", 499, &shadow0) &&
        render_text(lib, renderer,
        "{\\an1\\pos(40,150)\\1a&HFF&\\2a&HFF&\\bord0\\shad18"
        "\\kt50\\kO20}A", 500, &shadow1);
    fail |= expect(ok && !total_alpha(&shadow0) && total_alpha(&shadow1),
                   "\\kO shadow was visible before start or absent after reveal");
    free_frame(&shadow0); free_frame(&shadow1);

    Frame border0 = {0}, border1 = {0};
    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\3a&HFF&\\bord0"
        "\\2bs8\\2bc&H0000FF&\\2ba&H00&\\kt50\\kO20}A",
        499, &border0) && render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\3a&HFF&\\bord0"
        "\\2bs8\\2bc&H0000FF&\\2ba&H00&\\kt50\\kO20}A",
        500, &border1);
    fail |= expect(ok && !total_alpha(&border0) && total_alpha(&border1),
                   "\\kO additional border was visible before start or absent after reveal");
    free_frame(&border0); free_frame(&border1);

    Frame box0 = {0}, box1 = {0};
    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\bs4\\kt50\\kO20}A", 499, &box0) &&
        render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\bs4\\kt50\\kO20}A", 500, &box1);
    fail |= expect(ok && !total_alpha(&box0) && total_alpha(&box1),
                   "\\kO BorderStyle=4 box leaked before reveal");
    free_frame(&box0); free_frame(&box1);

    Frame deco0 = {0}, deco1 = {0};
    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\u1\\s1\\kt50\\kO20}Text", 499,
        &deco0) && render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\u1\\s1\\kt50\\kO20}Text", 500,
        &deco1);
    fail |= expect(ok && !total_alpha(&deco0) && total_alpha(&deco1),
                   "\\kO underline/strikeout leaked before reveal");
    free_frame(&deco0); free_frame(&deco1);

    Frame alpha_reveal = {0}, alpha_plain = {0};
    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\alpha&H80&\\kO20}A", 200,
        &alpha_reveal) && render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\alpha&H80&}A", 200, &alpha_plain);
    fail |= expect(ok && same_frame(&alpha_reveal, &alpha_plain),
                   "\\kO did not preserve the user's active alpha");
    free_frame(&alpha_reveal); free_frame(&alpha_plain);

    Frame kt0 = {0}, kt1 = {0}, rot0 = {0}, rot1 = {0};
    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\kt50\\kO20}A", 499, &kt0) &&
        render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\kt50\\kO20}A", 500, &kt1) &&
        render_text(lib, renderer,
        "{\\an1\\pos(100,180)\\frz180\\kt50\\kO20}A", 499, &rot0) &&
        render_text(lib, renderer,
        "{\\an1\\pos(100,180)\\frz180\\kt50\\kO20}A", 500, &rot1);
    fail |= expect(ok && !total_alpha(&kt0) && total_alpha(&kt1) &&
                   !total_alpha(&rot0) && total_alpha(&rot1),
                   "\\kt or frz180 inverted \\kO activation");
    free_frame(&kt0); free_frame(&kt1); free_frame(&rot0); free_frame(&rot1);

    Frame k = {0}, big_k = {0}, kf = {0}, ko = {0}, reveal = {0};
    ok = render_text(lib, renderer, "{\\kt50\\k20}A", 499, &k) &&
         render_text(lib, renderer, "{\\kt50\\K20}A", 499, &big_k) &&
         render_text(lib, renderer, "{\\kt50\\kf20}A", 499, &kf) &&
         render_text(lib, renderer, "{\\kt50\\ko20}A", 499, &ko) &&
         render_text(lib, renderer, "{\\kt50\\kO20}A", 499, &reveal);
    fail |= expect(ok && total_alpha(&k) && total_alpha(&big_k) &&
                   total_alpha(&kf) && total_alpha(&ko) &&
                   !total_alpha(&reveal) && same_frame(&big_k, &kf),
                   "karaoke parser collapsed case-sensitive k/K/kf/ko/kO tags");
    free_frame(&k); free_frame(&big_k); free_frame(&kf);
    free_frame(&ko); free_frame(&reveal);

    Frame transformed = {0}, transformed_plain = {0};
    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\kt50\\kO50"
        "\\t(0,1000,\\fscx200\\alpha&H80&)}A",
        750, &transformed) && render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\t(0,1000,\\fscx200\\alpha&H80&)}A",
        750, &transformed_plain);
    fail |= expect(ok && same_frame(&transformed, &transformed_plain),
                   "transforms did not continue evolving while \\kO was hidden");
    free_frame(&transformed); free_frame(&transformed_plain);

    Frame drawing0 = {0}, drawing1 = {0};
    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\kt50\\kO20\\p1}m 0 0 l 40 0 40 40 0 40",
        499, &drawing0) && render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\kt50\\kO20\\p1}m 0 0 l 40 0 40 40 0 40",
        500, &drawing1);
    fail |= expect(ok && !total_alpha(&drawing0) && total_alpha(&drawing1),
                   "drawing glyphs leaked before \\kO reveal");
    free_frame(&drawing0); free_frame(&drawing1);
    return fail;
}

static int test_furi(ASS_Library *lib, ASS_Renderer *renderer)
{
    int fail = 0;
    const char *k =
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord5\\3c&HFF0000&"
        "\\3sc&H0000FF&}<\xE7\x97\x85|{\\k30}\xE3\x82\x84"
        "{\\k26}\xE3\x81\xBE{\\k10}\xE3\x81\x84>";
    const char *kf =
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord5\\3c&HFF0000&"
        "\\3sc&H0000FF&}<\xE7\x97\x85|{\\kf30}\xE3\x82\x84"
        "{\\kf26}\xE3\x81\xBE{\\kf10}\xE3\x81\x84>";
    Frame k0 = {0}, k1 = {0}, kf0 = {0}, kf1 = {0}, furi_guide = {0};
    bool ok = render_text(lib, renderer, k, 0, &k0) &&
              render_text(lib, renderer, k, 300, &k1) &&
              render_text(lib, renderer, kf, 100, &kf0) &&
              render_text(lib, renderer, kf, 200, &kf1) &&
              render_text(lib, renderer,
                  "{\\an1\\pos(40,180)}\xE7\x97\x85", 0, &furi_guide);
    int gx0, gy0, gx1, gy1;
    bool guided = ok && frame_bounds(&furi_guide, &gx0, &gy0, &gx1, &gy1);
    int lower = guided ? (gy0 + gy1) / 2 : 0;
    bool k_reading = guided &&
        sum_plane(k1.b, 0, 0, W, gy0) > sum_plane(k0.b, 0, 0, W, gy0);
    bool k_base = guided &&
        sum_plane(k1.b, gx0, lower, gx1, gy1) >
        sum_plane(k0.b, gx0, lower, gx1, gy1);
    bool kf_reading = guided &&
        sum_plane(kf1.b, 0, 0, W, gy0) >
        sum_plane(kf0.b, 0, 0, W, gy0);
    bool kf_base = guided &&
        sum_plane(kf1.b, gx0, lower, gx1, gy1) >
        sum_plane(kf0.b, gx0, lower, gx1, gy1);
    fail |= expect(guided && sum_plane(k0.r, 0, 0, W, H) &&
                   sum_plane(k0.b, 0, 0, W, H) && k_reading && k_base &&
                   kf_reading && kf_base,
                   "furigana/base did not share secondary-outline k/kf progress");
    free_frame(&k0); free_frame(&k1); free_frame(&kf0); free_frame(&kf1);
    free_frame(&furi_guide);

    const char *cross =
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord5\\3c&HFF0000&"
        "\\3sc&H0000FF&}<\xE6\x8E\xB4|{\\k40}\xE3\x81\xA4"
        "{\\k60}\xE3\x81\x8B>\xE3\x82\x93{\\k70}\xE3\x81\xA0";
    Frame c0 = {0}, c1 = {0}, base = {0};
    ok = render_text(lib, renderer, cross, 399, &c0) &&
         render_text(lib, renderer, cross, 400, &c1) &&
         render_text(lib, renderer,
             "{\\an1\\pos(40,180)}\xE6\x8E\xB4", 0, &base);
    int x0, y0, x1, y1;
    bool bounded = ok && frame_bounds(&base, &x0, &y0, &x1, &y1);
    uint64_t base_delta = 0, following_delta = 0;
    if (bounded) {
        int lower = (y0 + y1) / 2;
        for (int y = lower; y < y1; y++)
            for (int x = x0; x < W; x++) {
                int off = y * W + x;
                uint32_t delta = c1.b[off] > c0.b[off] ?
                    c1.b[off] - c0.b[off] : 0;
                if (x < x1) base_delta += delta;
                else following_delta += delta;
            }
    }
    fail |= expect(bounded && base_delta && following_delta,
                   "cross-boundary segment did not own base and following outline");
    free_frame(&c0); free_frame(&c1); free_frame(&base);

    const char *rtl_k =
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord5\\3c&HFF0000&"
        "\\3sc&H0000FF&}<WW|{\\k30}\xD7\x90{\\k30}\xD7\x91>";
    Frame rtl = {0}, rtl_base = {0};
    ok = render_text(lib, renderer, rtl_k, 0, &rtl) &&
         render_text(lib, renderer, "{\\an1\\pos(40,180)}WW", 0,
                     &rtl_base);
    bounded = ok && frame_bounds(&rtl_base, &x0, &y0, &x1, &y1);
    uint64_t blue_cov = 0, red_cov = 0, base_blue_cov = 0, base_red_cov = 0;
    double blue_x = 0, red_x = 0, base_blue_x = 0, base_red_x = 0;
    if (bounded) {
        blue_x = centroid_x(rtl.b, 0, 0, W, y0, &blue_cov);
        red_x = centroid_x(rtl.r, 0, 0, W, y0, &red_cov);
        int base_y = (y0 + y1) / 2;
        base_blue_x = centroid_x(rtl.b, x0, base_y, x1, y1,
                                 &base_blue_cov);
        base_red_x = centroid_x(rtl.r, x0, base_y, x1, y1,
                                &base_red_cov);
    }
    fail |= expect(bounded && blue_cov && red_cov && base_blue_cov &&
                   base_red_cov && blue_x > red_x && base_blue_x > base_red_x,
                   "RTL furigana outline ownership did not activate right first");
    free_frame(&rtl); free_frame(&rtl_base);

    const char *rtl_kf =
        "{\\an1\\pos(40,180)\\1a&HFF&\\2a&HFF&\\bord5\\3c&HFF0000&"
        "\\3sc&H0000FF&}<WW|{\\kf100}\xD7\x90{\\kf100}\xD7\x91>";
    Frame r25 = {0}, r50 = {0}, r75 = {0};
    ok = render_text(lib, renderer, rtl_kf, 250, &r25) &&
         render_text(lib, renderer, rtl_kf, 500, &r50) &&
         render_text(lib, renderer, rtl_kf, 750, &r75);
    uint64_t d1 = 0, d2 = 0, d3 = 0;
    double dx1 = ok ? centroid_x(r25.b, 0, 0, W, H, &d1) : 0;
    double dx2 = ok ? delta_centroid_x(r25.b, r50.b, 0, 0, W, H, &d2) : 0;
    double dx3 = ok ? delta_centroid_x(r50.b, r75.b, 0, 0, W, H, &d3) : 0;
    fail |= expect(ok && d1 && d2 && d3 && dx1 > dx2 && dx2 > dx3,
                   "RTL furigana secondary outline \\kf sweep was not right-to-left");
    free_frame(&r25); free_frame(&r50); free_frame(&r75);

    const char *reveal =
        "{\\an1\\pos(40,180)}<\xE7\x97\x85|{\\kO30}\xE3\x82\x84"
        "{\\kO26}\xE3\x81\xBE{\\kO10}\xE3\x81\x84>";
    Frame v0 = {0}, v300 = {0}, v560 = {0};
    ok = render_text(lib, renderer, reveal, 0, &v0) &&
         render_text(lib, renderer, reveal, 300, &v300) &&
         render_text(lib, renderer, reveal, 560, &v560);
    fail |= expect(ok && total_alpha(&v0) &&
                   total_alpha(&v300) > total_alpha(&v0) &&
                   total_alpha(&v560) > total_alpha(&v300),
                   "furigana \\kO regions did not reveal on the shared timeline");
    free_frame(&v0); free_frame(&v300); free_frame(&v560);

    const char *wait =
        "{\\an1\\pos(40,180)}<\xE5\x90\x8C|{\\kO30}\xE3\x81\x8A"
        "{\\kO20}{\\kO50}\xE3\x81\xAA>";
    Frame w299 = {0}, w400 = {0}, w500 = {0};
    ok = render_text(lib, renderer, wait, 299, &w299) &&
         render_text(lib, renderer, wait, 400, &w400) &&
         render_text(lib, renderer, wait, 500, &w500);
    fail |= expect(ok && same_frame(&w299, &w400) &&
                   total_alpha(&w500) > total_alpha(&w400),
                   "wait-only furigana \\kO segment acquired geometry or lost time");
    free_frame(&w299); free_frame(&w400); free_frame(&w500);

    const char *reveal_cross =
        "{\\an1\\pos(40,180)}<\xE6\x8E\xB4|{\\kO40}\xE3\x81\xA4"
        "{\\kO60}\xE3\x81\x8B>\xE3\x82\x93{\\kO70}\xE3\x81\xA0";
    Frame rc0 = {0}, rc1 = {0};
    ok = render_text(lib, renderer, reveal_cross, 399, &rc0) &&
         render_text(lib, renderer, reveal_cross, 400, &rc1) &&
         render_text(lib, renderer,
             "{\\an1\\pos(40,180)}\xE6\x8E\xB4", 0, &base);
    bounded = ok && frame_bounds(&base, &x0, &y0, &x1, &y1);
    base_delta = following_delta = 0;
    if (bounded) {
        int lower = (y0 + y1) / 2;
        for (int y = lower; y < y1; y++)
            for (int x = x0; x < W; x++) {
                int off = y * W + x;
                uint32_t delta = rc1.a[off] > rc0.a[off] ?
                    rc1.a[off] - rc0.a[off] : 0;
                if (x < x1) base_delta += delta;
                else following_delta += delta;
            }
    }
    fail |= expect(bounded && base_delta && following_delta,
                   "cross-boundary \\kO did not reveal base and following glyph together");
    free_frame(&rc0); free_frame(&rc1); free_frame(&base);

    const char *rtl_reveal =
        "{\\an1\\pos(40,180)}<WW|{\\kO30}\xD7\x90{\\kO30}\xD7\x91>";
    Frame rr = {0};
    ok = render_text(lib, renderer, rtl_reveal, 0, &rr) &&
         render_text(lib, renderer, "{\\an1\\pos(40,180)}WW", 0,
                     &rtl_base);
    bounded = ok && frame_bounds(&rtl_base, &x0, &y0, &x1, &y1);
    uint64_t left = bounded ? sum_plane(rr.a, 0, 0, (x0 + x1) / 2, H) : 0;
    uint64_t right = bounded ? sum_plane(rr.a, (x0 + x1) / 2, 0, W, H) : 0;
    fail |= expect(bounded && right > left,
                   "RTL \\kO visual region ownership did not reveal right first");
    free_frame(&rr); free_frame(&rtl_base);

    Frame illegal = {0}, legal = {0};
    ok = render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\kt20}< {\\kO999}A|b>C{\\kO30}D",
        200, &illegal) && render_text(lib, renderer,
        "{\\an1\\pos(40,180)\\kt20}< A|b>C{\\kO30}D",
        200, &legal);
    fail |= expect(ok && same_frame(&illegal, &legal),
                   "base-side \\kO changed the karaoke timeline");
    free_frame(&illegal); free_frame(&legal);
    return fail;
}

static int test_seek_order(ASS_Library *lib, ASS_Renderer *renderer)
{
    const char *text =
        "{\\an1\\pos(40,180)\\kt20\\kO20}A{\\kO20}B{\\kO20}C";
    char *event = NULL;
    const char *prefix =
        "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,";
    size_t event_size = strlen(prefix) + strlen(text) + 1;
    event = malloc(event_size);
    if (!event)
        return 1;
    snprintf(event, event_size, "%s%s", prefix, text);
    char *script = make_script(event);
    free(event);
    if (!script)
        return 1;
    ASS_Track *track = ass_read_memory(lib, script, strlen(script), NULL);
    free(script);
    if (!track)
        return 1;

    const long long times[] = {900, 100, 900};
    Frame seq[3] = {0}, direct = {0};
    bool ok = true;
    for (int i = 0; i < 3 && ok; i++) {
        ok = alloc_frame(&seq[i]);
        ASS_ImageRGBA *images = ok ?
            ass_render_frame_rgba(renderer, track, times[i], NULL) : NULL;
        for (ASS_ImageRGBA *img = images; img; img = img->next)
            for (int y = 0; y < img->h; y++)
                for (int x = 0; x < img->w; x++) {
                    int xx = img->dst_x + x, yy = img->dst_y + y;
                    if (xx < 0 || xx >= W || yy < 0 || yy >= H)
                        continue;
                    int off = yy * W + xx;
                    const uint8_t *px = img->rgba + y * img->stride + 4 * x;
                    seq[i].r[off] += px[0]; seq[i].g[off] += px[1];
                    seq[i].b[off] += px[2]; seq[i].a[off] += px[3];
                }
        ass_free_images_rgba(images);
    }
    ass_free_track(track);
    ok = ok && render_text(lib, renderer, text, 100, &direct);
    int fail = expect(ok && same_frame(&seq[0], &seq[2]) &&
                      same_frame(&seq[1], &direct) &&
                      total_alpha(&seq[1]) < total_alpha(&seq[0]),
                      "\\kO depended on frame order or failed to hide after reverse seek");
    for (int i = 0; i < 3; i++) free_frame(&seq[i]);
    free_frame(&direct);
    return fail;
}

int main(void)
{
    ASS_Library *lib = ass_library_init();
    ASS_Renderer *renderer = lib ? ass_renderer_init(lib) : NULL;
    if (!renderer) {
        if (lib) ass_library_done(lib);
        return 1;
    }
    ass_set_storage_size(renderer, W, H);
    ass_set_frame_size(renderer, W, H);
    ass_set_fonts(renderer, NULL, "Arial", ASS_FONTPROVIDER_AUTODETECT,
                  NULL, 1);

    int fail = 0;
    fail |= test_secondary_outline(lib, renderer);
    fail |= test_reveal(lib, renderer);
    fail |= test_furi(lib, renderer);
    fail |= test_seek_order(lib, renderer);

    ass_renderer_done(renderer);
    ass_library_done(lib);
    return fail ? 1 : 0;
}
