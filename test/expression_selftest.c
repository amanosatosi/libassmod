/*
 * Copyright (C) 2026 libass contributors
 * SPDX-License-Identifier: ISC
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ass.h"
#include "ass_expression.h"

static bool arithmetic(void)
{
    const ASS_ExprBounds bounds = {100, 300, 50, 150, true};
    const struct { const char *text; double value; } cases[] = {
        {"$start+500", 500}, {"$end-500", 2500}, {"$mid+200", 1700},
        {"$dur/2", 1500}, {" 10 % ", 300}, {"0%", 0}, {"100%", 3000},
        {"90%", 2700}, {" - 10% ", 2700}, {"$end-10%", 2700},
        {"$mid+10%", 1800}, {"10%+500", 800}, {"$end-10%-200", 2500},
        {"($dur/2)+10%", 1800}, {"(-10%)", -300}, {"-10%+500", 200},
        {"12.5%", 375}, {"$width+20/2", 210}, {"($width+20)/2", 110},
        {"$right-$left", 200}, {"$bottom-$top", 100},
        {"$height*1.5 + $middle - $center", 50}, {"-($height/2)", -50},
        {"--.5 * +(2 + 2)", 2}, {"0-1", -1}, {"1e-2 * 2", .02},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        ASS_Expression expr;
        double value;
        const char *s = cases[i].text;
        if (!ass_expr_compile(s, s + strlen(s), true, &expr) ||
                !ass_expr_eval(&expr, 3000, &bounds, &value) ||
                fabs(value - cases[i].value) > 1e-8) {
            fprintf(stderr, "Expression failed: %s\n", s);
            return false;
        }
    }
    const char *invalid[] = {
        "", "$lwidth", "$widthx", "$white", "1/0", "1+/2", "2**3", "$dur+",
        "10%%", "10% 90%", "()", "nan+1", "1e309", "1e300*1e300", "2e+",
        "(((((((((((((((((1)))))))))))))))))",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        ASS_Expression expr;
        double value;
        const char *s = invalid[i];
        if (ass_expr_compile(s, s + strlen(s), true, &expr) &&
                ass_expr_eval(&expr, 3000, &bounds, &value)) {
            fprintf(stderr, "Invalid expression accepted: %s\n", s);
            return false;
        }
    }
    ASS_Expression expr;
    double value;
    const char percent[] = "10%";
    const char end[] = "$end";
    const char width[] = "$width";
    char long_text[ASS_EXPR_MAX_LENGTH + 1];
    memset(long_text, '1', sizeof(long_text));
    const ASS_ExprBounds unavailable = {0};
    return !ass_expr_compile(long_text, long_text + sizeof(long_text), false, &expr) &&
           !ass_expr_compile(percent, percent + sizeof(percent) - 1, false, &expr) &&
           ass_expr_compile(end, end + sizeof(end) - 1, false, &expr) &&
           ass_expr_eval(&expr, 0, &bounds, &value) && value == 0 &&
           ass_expr_eval(&expr, 1, &bounds, &value) && value == 1 &&
           ass_expr_eval(&expr, 2147483648., &bounds, &value) && value == 2147483648. &&
           ass_expr_compile(width, width + sizeof(width) - 1, false, &expr) &&
           !ass_expr_eval(&expr, 3000, &unavailable, &value);
}

static ASS_Track *read_track(ASS_Library *lib, const char *tags, const char *body,
                             const char *metadata)
{
    char script[4096];
    int n = snprintf(script, sizeof(script),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour,"
        " Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle,"
        " Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Pixel Operator Mono,40,&H00FFFFFF,&H00FFFFFF,&H0,&H0,"
        "0,0,0,0,100,100,0,0,1,0,0,7,10,10,10,1\n"
        "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "%sDialogue: 0,0:00:07.00,0:00:10.00,Default,,0,0,0,,{%s}%s\n",
        metadata, tags, body);
    return n > 0 && n < (int) sizeof(script) ? ass_read_memory(lib, script, n, NULL) : NULL;
}

static uint64_t frame(ASS_Renderer *renderer, ASS_Track *track, int time)
{
    ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, time, NULL);
    uint64_t hash = images ? UINT64_C(14695981039346656037) : 0;
    for (ASS_ImageRGBA *img = images; img; img = img->next) {
        const int geometry[] = {img->dst_x, img->dst_y, img->w, img->h, img->type};
        for (size_t i = 0; i < sizeof(geometry) / sizeof(*geometry); i++)
            hash = (hash ^ (uint32_t) geometry[i]) * UINT64_C(1099511628211);
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < 4 * img->w; x++)
                hash = (hash ^ img->rgba[(size_t) y * img->stride + x]) * UINT64_C(1099511628211);
    }
    ass_free_images_rgba(images);
    return hash;
}

#define DRAW "m 0 0 l 200 0 200 100 0 100"
#define BASE "\\an7\\pos(100,100)\\p1"
#define WIPE "\\t(0%,100%,\\1grd(90,&HFF0000&,&H0000FF&))"

static bool renders(ASS_Library *lib, ASS_Renderer *renderer)
{
    const struct { const char *actual, *expected, *metadata; } cases[] = {
        {BASE "\\t($start+500,$end-500,\\fscx150)", BASE "\\t(500,2500,\\fscx150)", ""},
        {BASE "\\t(10%,90%,\\fscx150)", BASE "\\t(300,2700,\\fscx150)", ""},
        {BASE "\\t(10%,-10%,\\blur5)", BASE "\\t(300,2700,\\blur5)", ""},
        {BASE "\\t($mid-200,$mid+200,\\blur10)", BASE "\\t(1300,1700,\\blur10)", ""},
        {BASE "\\t(($dur/4),($dur*3/4),1+1,\\fscx150)",
         BASE "\\t(750,2250,2,\\fscx150)", ""},
        {BASE "\\t(-10%+500,$end-10%-200,\\fscx150)", BASE "\\t(200,2500,\\fscx150)", ""},
        {BASE "\\t(90%,10%,\\fscx150)", BASE "\\t(2700,300,\\fscx150)", ""},
        {BASE "\\t(0%,0%,\\fscx150)", BASE "\\fscx150", ""},
        {BASE "\\t(10%,$oops,\\fscx150)\\t(10%%,90%,\\fscx150)\\t(10%,,90%,\\fscx150)", BASE, ""},
        {BASE "\\move(100,100,200,100,$dur/4,$dur*3/4)", BASE, ""},
        {"\\an7\\move((50+50),100,200,100,$dur/4,$dur*3/4)\\p1",
         "\\an7\\move(100,100,200,100,750,2250)\\p1", ""},
        {"\\an7\\move(100,100,200,100,10%,90%)\\pos(100,100)\\p1", BASE, ""},
        {BASE "\\org($center,$middle)\\frz(5+5)", BASE "\\org(200,150)\\frz10", ""},
        {BASE "\\clip(($left+20),($top+10),($right-20),($bottom-10))",
         BASE "\\clip(120,110,280,190)", ""},
        {BASE "\\fscx($width/2+20)\\fscy($height+20)\\bord(2*2)\\1bs(4+1)\\2bs(1+2)\\shad(2+1)\\be(1+1)",
         BASE "\\fscx120\\fscy120\\bord4\\1bs5\\2bs3\\shad3\\be2", ""},
        {BASE "\\grds(($width+$height)/6)" WIPE, BASE "\\grds50" WIPE, ""},
        {BASE "\\grds(12)\\grds($height/0)" WIPE, BASE "\\grds12" WIPE, ""},
        {BASE WIPE, BASE "\\grds50" WIPE,
         "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grds10\\grds($height/2)}\n"},
        {BASE "\\grds0\\r" WIPE, BASE "\\r\\grds50" WIPE,
         "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grds($height/2)}\n"},
        {BASE "\\pgrd($left,$top,$right,$bottom,($width/4),$white,($height/2)%,$black,$white)",
         BASE "\\pgrd(100,100,300,200,50,$white,50%,$black,$white)", ""},
        {BASE "\\fscx(1/0)\\pos($unknown,0)\\blur($width*1e300)", BASE, ""},
    };
    const int times[] = {7000, 7333, 8500, 9800, 8000, 7333};
    bool ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        ASS_Track *actual = read_track(lib, cases[i].actual, DRAW, cases[i].metadata);
        ASS_Track *expected = read_track(lib, cases[i].expected, DRAW, "");
        bool good = actual && expected;
        for (size_t j = 0; good && j < sizeof(times) / sizeof(*times); j++) {
            uint64_t ref = frame(renderer, expected, times[j]);
            good = ref && frame(renderer, actual, times[j]) == ref;
            if (!good)
                fprintf(stderr, "Frame mismatch at %d: %s\n", times[j], cases[i].actual);
        }
        if (actual) ass_free_track(actual);
        if (expected) ass_free_track(expected);
        ok &= good;
    }
    return ok;
}

static bool attach_font(ASS_Library *lib, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    long size = !fseek(file, 0, SEEK_END) ? ftell(file) : -1;
    char *bytes = size > 0 && size <= 16 * 1024 * 1024 ? ass_malloc(size) : NULL;
    bool ok = bytes && !fseek(file, 0, SEEK_SET) && fread(bytes, 1, size, file) == (size_t) size;
    if (ok)
        ass_add_font(lib, path, bytes, size);
    ass_free(bytes);
    fclose(file);
    return ok;
}

static bool layout(ASS_Library *lib, ASS_Renderer *renderer)
{
    const struct { const char *tags, *text; } cases[] = {
        {"\\an8\\pos(320,60)", "office $10 ffi"},
        {"\\an5\\pos(320,180)", "short\\Nlonger line\\Nthird line"},
        {"\\an7\\fs80\\q0", "Several long words that wrap across more than one line"},
        {"\\an7\\pos(80,100)\\fnNoto Sans Myanmar", "မြန်မာ ဆာတို့"},
        {"\\an7\\pos(80,100)\\vert1", "漢字かな"},
        {"\\an7\\pos(80,100)\\furi1", "<漢字|かんじ> 漢字"},
        {"\\an5\\pos(320,180)\\ct(m -140 0 l 0 -25 140 0)", "curved text"},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        char expression[512], literal[512];
        snprintf(expression, sizeof(expression), "%s\\fscx(120+($right-$left-$width))"
                 "\\bord(2+($bottom-$top-$height))", cases[i].tags);
        snprintf(literal, sizeof(literal), "%s\\fscx120\\bord2", cases[i].tags);
        ASS_Track *actual = read_track(lib, expression, cases[i].text, "");
        ASS_Track *expected = read_track(lib, literal, cases[i].text, "");
        bool good = actual && expected;
        if (good) {
            for (int scale = 1; scale <= 2; scale++) {
                ass_set_frame_size(renderer, 640 * scale, 360 * scale);
                uint64_t ref = frame(renderer, expected, 8500);
                uint64_t first = frame(renderer, actual, 8500);
                good &= ref && first == ref && frame(renderer, actual, 7100) == first;
            }
        }
        if (!good)
            fprintf(stderr, "Expression layout mismatch: %s\n", cases[i].tags);
        if (actual) ass_free_track(actual);
        if (expected) ass_free_track(expected);
        ok &= good;
    }
    ass_set_frame_size(renderer, 640, 360);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 4 || !arithmetic())
        return 1;
    ASS_Library *lib = ass_library_init();
    ASS_Renderer *renderer = lib ? ass_renderer_init(lib) : NULL;
    if (!renderer) {
        if (lib) ass_library_done(lib);
        return 2;
    }
    ass_set_frame_size(renderer, 640, 360);
    bool ok = attach_font(lib, argv[2]) && attach_font(lib, argv[3]);
    ass_set_fonts(renderer, argv[1], NULL, ASS_FONTPROVIDER_NONE, NULL, 0);
    ok &= renders(lib, renderer);
    ok &= layout(lib, renderer);
    ass_renderer_done(renderer);
    ass_library_done(lib);
    return ok ? 0 : 1;
}
