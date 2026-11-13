/* Public-API harness, deliberately buildable against the pre-feature branch.
 * Time only warmed rendering with the same attached fonts in both binaries. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "ass.h"

#ifdef MEASURE_ALLOCATIONS
#include <sys/resource.h>

/* GNU ld wraps direct allocator calls in the harness and static libass.
 * Shared font libraries still contribute to process peak RSS. */
static int count_allocations;
static uint64_t allocations, requested_bytes;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);

static void record_allocation(size_t size, const void *ptr)
{
    if (count_allocations && ptr) {
        allocations++;
        requested_bytes += size;
    }
}

void *__wrap_malloc(size_t size)
{
    void *ptr = __real_malloc(size);
    record_allocation(size, ptr);
    return ptr;
}

void *__wrap_calloc(size_t count, size_t size)
{
    void *ptr = __real_calloc(count, size);
    record_allocation(count * size, ptr);
    return ptr;
}

void *__wrap_realloc(void *old, size_t size)
{
    void *ptr = __real_realloc(old, size);
    record_allocation(size, ptr);
    return ptr;
}
#endif

#define BENCH_SOURCE "(0,&H0000FF&,&HFF0000&)"
#define BENCH_TARGET "(45,&HFF00FF&,&H00FF00&)"
#define BENCH_STOPS "(-35,&H00FFFF&,25%,&H00FF00&,55%,&H8080FF&,70%,&HFFFFFF&,&HFF0000&)"
#define BENCH_FIELD "(120,80,1100,840,0,&H0000FF&,&HFF0000&)"
#define BENCH_FIELD_TARGET "(250,120,1300,960,-45,&HFF00FF&,40%,&H00FF00&,&H00FFFF&)"
#define BENCH_T(tags) "\\t(0,1000," tags ")"

static const char *animated_gradient(const char *workload)
{
    if (!strcmp(workload, "fill"))
        return "\\1c&H0000FF&" BENCH_T("\\1grd" BENCH_TARGET);
    if (!strcmp(workload, "stops"))
        return "\\1grd" BENCH_SOURCE BENCH_T("\\1grd" BENCH_STOPS);
    if (!strcmp(workload, "rgb-alpha"))
        return "\\1grd" BENCH_SOURCE "\\1gra(0,&H00&,&H40&)"
               BENCH_T("\\1grd" BENCH_STOPS "\\1gra(85,&H10&,35%,&HC0&,&H30&)");
    if (!strcmp(workload, "outlines"))
        return "\\1bs3\\2bs4\\3bs5\\1bgrd" BENCH_SOURCE "\\2bgrd" BENCH_SOURCE
               "\\3bgrd" BENCH_SOURCE BENCH_T("\\1bgrd" BENCH_TARGET
               "\\2bgrd" BENCH_STOPS "\\3bgrd" BENCH_TARGET);
    if (!strcmp(workload, "positioned"))
        return "\\pgrd" BENCH_FIELD "\\pgrdalpha(100,100,1100,800,0,&H00&,&H70&)"
               BENCH_T("\\pgrd" BENCH_FIELD_TARGET
               "\\pgrdalpha(200,100,1300,950,35,&HC0&,&H20&)");
    if (!strcmp(workload, "overlap"))
        return "\\1grd" BENCH_SOURCE BENCH_T("\\1grd" BENCH_TARGET)
               "\\t(100,950,\\1grd" BENCH_STOPS ")"
               "\\t(200,1050,\\1grd(90,&H00FF00&,&HFF00FF&))"
               "\\t(250,1150,\\1grd(-70,&H00FFFF&,&H00FF00&))"
               "\\t(300,1150,\\1grd(25,&HFF0000&,&HFFFFFF&))"
               "\\t(350,1150,\\1grd(0,&HFFFFFF&,&H0000FF&))";
    return NULL;
}

static void quiet(int level, const char *format, va_list args, void *data)
{ (void)level; (void)format; (void)args; (void)data; }

static int font(ASS_Library *lib, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int ok = !fseek(f, 0, SEEK_END);
    long n = ok ? ftell(f) : -1;
    ok = n > 0 && n < 100000000 && !fseek(f, 0, SEEK_SET);
    char *data = ok ? malloc(n) : NULL;
    ok = data && fread(data, 1, n, f) == (size_t)n;
    if (ok) ass_add_font(lib, path, data, (int)n);
    free(data); fclose(f); return ok;
}

int main(int argc, char **argv)
{
    if (argc != 5 && argc != 8) return 1;
    int frames = atoi(argv[3]);
    const char *workload = argv[4];
    const char *animation = animated_gradient(workload);
    int height = argc == 8 ? atoi(argv[5]) : 1080;
    int event_count = argc == 8 ? atoi(argv[6]) : 12;
    int sliding = argc == 8 && !strcmp(argv[7], "slide");
    if ((height != 720 && height != 1080 && height != 2160) ||
            event_count < 1 || event_count > 12 || (sliding && !animation))
        return 1;
    int ruby = !strncmp(workload, "furi-", 5);
    int metadata = !strcmp(workload, "furi-metadata");
    int gradient = !strncmp(workload, "gradient-", 9);
    int gradient_metadata = !strcmp(workload, "gradient-metadata");
    const char *gradient_tags = "{\\grd(45,&HFF0000&,&H0000FF&)\\1gra(0,&H00&,&H40&)}";
    if (frames <= 0 || frames > 1000000) return 1;
    ASS_Library *lib = ass_library_init();
    if (!lib) return 1;
    ass_set_message_cb(lib, quiet, NULL);
    if (!font(lib, argv[1]) || !font(lib, argv[2])) { ass_library_done(lib); return 1; }
    ASS_Renderer *renderer = ass_renderer_init(lib);
    if (!renderer) { ass_library_done(lib); return 1; }
    ass_set_frame_size(renderer, height * 16 / 9, height);
    ass_set_storage_size(renderer, height * 16 / 9, height);
    ass_set_fonts(renderer, NULL, "Noto Sans Myanmar", ASS_FONTPROVIDER_NONE, NULL, 1);
    char script[16384];
    int length = snprintf(script, sizeof(script),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n"
        "ScaledBorderAndShadow: yes\n[V4+ Styles]\n"
        "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
        "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
        "Alignment,MarginL,MarginR,MarginV,Encoding\n"
        "Style: Default,%s,28,&H00FFFFFF,&H00FFFFFF,&H00000000,&H80000000,"
        "0,0,0,0,100,100,0,0,1,2,0,2,20,20,20,1\n"
        "[Events]\nFormat: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n%s",
        ruby ? "Noto Sans Myanmar" : "Pixel Operator Mono",
        metadata ? "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\furis70}\n" :
        gradient_metadata ? "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,mangetsu-style,{\\grd(45,&HFF0000&,&H0000FF&)\\1gra(0,&H00&,&H40&)}\n" : "");
    for (int i = 0; i < event_count; i++) {
        const char *tags = !strcmp(workload, "box-scalar") ? "{\\bs4\\boxp10\\boxpx12\\boxpy8}" :
            !strcmp(workload, "box-asymmetric") ? "{\\an5\\pos(960,540)\\bs4\\boxp(8,16,4,12)\\1a&HFF&\\3a&HFF&}" :
            !strcmp(workload, "box-asymmetric-reference") ? "{\\an5\\pos(964,544)\\bs4\\boxpx12\\boxpy8\\1a&HFF&\\3a&HFF&}" :
            !strcmp(workload, "ta-multiline") ? "{\\ta7}" :
            !strcmp(workload, "ta-explicit") ? "{\\ta7}" :
            !strcmp(workload, "gradient-box-complex") ? "{\\bs4\\boxp10\\boxr6\\bbs2\\2bbs2\\bbgrd(45,&HFF0000&,&H0000FF&)\\4grd(0,&H00FF00&,&HFFFFFF&)\\frx20\\fry15\\frz25\\fax.1}" :
            ruby ? (metadata ? "" : "{\\furis70}") :
            !strcmp(workload, "gradient-borders") ? "{\\bord2\\2bs3\\2bgrd(45,&HFF0000&,&H0000FF&)\\3bs2\\3bgrd(0,&H00FF00&,&HFFFFFF&)}" :
            gradient ? (gradient_metadata ? "" : gradient_tags) :
            !strcmp(workload, "vertical-inline") ? "{\\vert1\\vtype2\\vsp2\\vcolsp8}" :
            !strcmp(workload, "border-karaoke") ? "{\\bord2\\2bs3\\2bc&HFF0000&\\3bs2\\3bc&H00FF00&\\kf100}" :
            !strcmp(workload, "ordinary-overrides") ? "{\\bord2\\blur0.5\\1c&H40A0FF&}" : "";
        const char *content = !strcmp(workload, "ta-multiline") ? "A\\NLONGEST LINE OF TEXT\\NShort" :
            !strcmp(workload, "ta-explicit") ? "A\\N{\\ta7}LONGEST LINE OF TEXT\\N{\\ta7}Short" :
            !strncmp(workload, "box-", 4) ? "BOX" :
            ruby ? "<က|စူဇူကီ> <က||ဆာတို့>" : gradient ? "Gradient" : "Ordinary ASS subtitle text with no Mangetsu metadata.";
        int n;
        if (animation) {
            n = snprintf(script + length, sizeof(script) - length,
                "Dialogue: 0,0:00:00.00,9:00:00.00,Default,,0,0,0,,"
                "{\\an7\\pos(180,%d)\\fs56%s%s}Sliding gradient benchmark %d\n",
                130 + i * 74, sliding ? "\\grds24" : "", animation, i);
        } else {
            n = snprintf(script + length, sizeof(script) - length,
                "Dialogue: 0,0:00:00.00,9:00:00.00,Default,,0,0,0,,%s%s\n", tags, content);
        }
        if (n < 0 || (size_t) n >= sizeof(script) - length) {
            ass_renderer_done(renderer);
            ass_library_done(lib);
            return 1;
        }
        length += n;
    }
    ASS_Track *track = ass_read_memory(lib, script, length, NULL);
    if (!track) { ass_renderer_done(renderer); ass_library_done(lib); return 1; }
    int warmup = animation ? 24 : 100;
    for (int i = 0; i < warmup; i++) {
        if (animation || gradient)
            ass_free_images_rgba(ass_render_frame_rgba(renderer, track, animation ? 400 + i * 8 : i, NULL));
        else ass_render_frame(renderer, track, i, NULL);
    }
    uint64_t checksum = UINT64_C(1469598103934665603);
    unsigned long images = 0;
    clock_t start = clock();
    for (int i = 0; i < frames; i++) {
        if (animation || gradient) {
            int now = animation ? 400 + (i % 60) * 8 : i;
            ASS_ImageRGBA *list = ass_render_frame_rgba(renderer, track, now, NULL);
            for (ASS_ImageRGBA *im = list; im; im = im->next) {
                uint32_t geometry[] = {im->dst_x, im->dst_y, im->w, im->h, im->type};
                for (int j = 0; j < 5; j++)
                    checksum = (checksum ^ geometry[j]) * UINT64_C(1099511628211);
                if (im->w && im->h) {
                    const uint8_t *pixel = im->rgba + (im->h / 2) * im->stride + 4 * (im->w / 2);
                    for (int c = 0; c < 4; c++) checksum = (checksum ^ pixel[c]) * UINT64_C(1099511628211);
                }
                images++;
            }
            ass_free_images_rgba(list);
            continue;
        }
        ASS_Image *im = ass_render_frame(renderer, track, i, NULL);
        for (; im; im = im->next) {
            if ((im->color & 255) == 255)
                continue;
            uint32_t geometry[] = {im->dst_x, im->dst_y, im->w, im->h, im->color};
            for (int j = 0; j < 5; j++)
                checksum = (checksum ^ geometry[j]) * UINT64_C(1099511628211);
            images++;
        }
    }
    double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
#ifdef MEASURE_ALLOCATIONS
    count_allocations = 1;
    int memory_frames = frames < 16 ? frames : 16;
    for (int i = 0; i < memory_frames; i++) {
        if (animation || gradient)
            ass_free_images_rgba(ass_render_frame_rgba(renderer, track, animation ? 440 + i * 16 : i, NULL));
        else
            ass_render_frame(renderer, track, i, NULL);
    }
    count_allocations = 0;
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage))
        return 1;
#endif
    printf("{\"workload\":\"%s\",\"frames\":%d,\"cpu_seconds\":%.9f,"
           "\"checksum\":\"%016" PRIx64 "\",\"images\":%lu",
           workload, frames, seconds, checksum, images);
#ifdef MEASURE_ALLOCATIONS
    printf(",\"allocations_per_frame\":%.3f,\"requested_bytes_per_frame\":%.3f,\"peak_rss_kib\":%ld",
           (double) allocations / memory_frames, (double) requested_bytes / memory_frames,
           usage.ru_maxrss);
#endif
    puts("}");
    ass_free_track(track); ass_renderer_done(renderer); ass_library_done(lib);
    return seconds > 0 && images > 0 ? 0 : 1;
}
