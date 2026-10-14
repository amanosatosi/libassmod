/* Global positioned alpha: public API pixels, scopes, composited overlaps,
 * hardware blend output and boxes. Uses an embedded font on every platform. */
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ass.h"

enum { W = 320, H = 200, PLANES = 4, ART = 3 };
#define POS "\\an5\\pos(160,100)"
#define FULL "\\pgrdalpha(0,0,320,200,0,$255,$255)"
#define HALF "\\pgrdalpha(0,0,320,200,0,$128,$128)"
#define OPAQUE "\\pgrdalpha(0,0,320,200,0,$0,$0)"
static const char header[] =
    "[Script Info]\nScriptType: v4.00+\nPlayResX: 320\nPlayResY: 200\n"
    "ScaledBorderAndShadow: yes\n[V4+ Styles]\n"
    "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
    "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
    "Alignment,MarginL,MarginR,MarginV,Encoding\n"
    "Style: Default,sans-serif,32,&H00FFFFFF,&H000000FF,&H0000FF00,&H00FF0000,"
    "0,0,0,0,100,100,0,0,1,3,4,5,10,10,10,1\n"
    "[Events]\nFormat: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n";
typedef struct { uint8_t *pixels, *video; } Frame;

static uint8_t *pixel(Frame *f, int plane, int x, int y)
{ return f->pixels + ((size_t) plane * W * H + y * W + x) * 4; }
static void release(Frame *f) { free(f->pixels); free(f->video); *f = (Frame) {0}; }
static ASS_Track *track_for(ASS_Library *lib, const char *text)
{
    size_t size = strlen(header) + strlen(text) + 160;
    char *script = malloc(size);
    if (!script) return NULL;
    snprintf(script, size, "%sDialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,%s\n", header, text);
    ASS_Track *t = ass_read_memory(lib, script, strlen(script), NULL);
    free(script); return t;
}
static bool render_track(ASS_Renderer *renderer, ASS_Track *track, long long now, Frame *f)
{
    f->pixels = calloc((size_t) PLANES * W * H, 4);
    f->video = malloc((size_t) W * H * 4);
    if (!f->pixels || !f->video) return false;
    for (int i = 0; i < W * H; i++) {
        f->video[4*i] = 37; f->video[4*i+1] = 59; f->video[4*i+2] = 83; f->video[4*i+3] = 0;
    }
    ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, now, NULL);
    bool ok = ass_composite_images_bgra(images, f->video, W, H, W * 4) == 0;
    for (ASS_ImageRGBA *img = images; img; img = img->next) {
        if (img->type > 2) { ok = false; continue; }
        for (int y = 0; y < img->h; y++) for (int x = 0; x < img->w; x++) {
            int px = img->dst_x + x, py = img->dst_y + y;
            if (px < 0 || px >= W || py < 0 || py >= H) continue;
            const uint8_t *s = img->rgba + (size_t) y * img->stride + 4 * x;
            for (int c = 0; c < 3; c++) if (s[c] > s[3]) ok = false;
            for (int j = 0; j < 2; j++) {
                uint8_t *d = pixel(f, j ? ART : img->type, px, py);
                for (int c = 0; c < 4; c++) d[c] = s[c] + (d[c] * (255 - s[3]) + 127) / 255;
            }
        }
    }
    ass_free_images_rgba(images);
    return ok;
}
static bool render(ASS_Library *lib, ASS_Renderer *renderer, const char *text, long long now, Frame *f)
{
    ASS_Track *track = track_for(lib, text);
    if (!track) return false;
    bool ok = render_track(renderer, track, now, f);
    ass_free_track(track); return ok;
}
static bool equal(ASS_Library *lib, ASS_Renderer *renderer, const char *a, const char *b,
                  long long now, const char *label)
{
    Frame fa = {0}, fb = {0};
    bool ok = render(lib, renderer, a, now, &fa) && render(lib, renderer, b, now, &fb) &&
        !memcmp(fa.pixels, fb.pixels, (size_t) PLANES * W * H * 4) &&
        !memcmp(fa.video, fb.video, (size_t) W * H * 4);
    if (!ok) fprintf(stderr, "%s: %s != %s\n", label, a, b);
    release(&fa); release(&fb); return ok;
}
static uint64_t coverage(Frame *f, int plane)
{
    uint64_t n = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) n += pixel(f,plane,x,y)[3];
    return n;
}
/* Oracle is the completed baseline artwork, not independently scaled tiles. */
static bool masked_pixels(ASS_Library *lib, ASS_Renderer *renderer, const char *setup,
                          const char *body, int x0, int y0, int x1, int y1,
                          unsigned first, unsigned last, long long now)
{
    char a[2048], b[2048]; Frame baseline = {0}, masked = {0};
    snprintf(a,sizeof(a),"{" POS "%s}%s",setup,body);
    snprintf(b,sizeof(b),"{" POS "%s\\pgrdalpha(%d,%d,%d,%d,0,$%u,$%u)}%s",
             setup,x0,y0,x1,y1,first,last,body);
    bool ok = render(lib,renderer,a,now,&baseline) && render(lib,renderer,b,now,&masked);
    unsigned inside_ink = 0, outside_ink = 0, changed = 0;
    if (ok) for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        uint8_t *src = pixel(&baseline,ART,x,y), *dst = pixel(&masked,ART,x,y);
        bool inside = x + .5 >= x0 && x + .5 <= x1 && y + .5 >= y0 && y + .5 <= y1;
        unsigned opacity = 255;
        if (inside) {
            double t = (x + .5 - x0) / (x1 - x0);
            unsigned alpha = (unsigned) (first * (1-t) + last*t + .5);
            opacity = 255 - alpha;
            inside_ink += src[3] > 0;
        } else outside_ink += src[3] > 0;
        for (int c = 0; c < 4; c++) {
            unsigned expected = (src[c]*opacity + 127)/255;
            if (inside ? abs((int) dst[c] - (int) expected) > 3 : dst[c] != src[c]) {
                fprintf(stderr,"mask pixel (%d,%d) c%d: %u expected %u, %s\n",x,y,c,dst[c],expected,setup);
                ok = false; goto done;
            }
            changed += dst[c] != src[c];
        }
    }
done:
    if (ok && first != 0 && last != 0 && inside_ink && !changed) ok = false;
    if (!ok) fprintf(stderr,"positioned alpha oracle failed (inside %u outside %u): %s\n",inside_ink,outside_ink,b);
    release(&baseline); release(&masked); return ok;
}

static bool surfaces(ASS_Library *lib, ASS_Renderer *renderer)
{
    const struct { const char *setup, *body; int plane; } cases[] = {
        {"", "MMMM", IMAGE_TYPE_CHARACTER},
        {"\\kt200\\kf200", "MMMM", IMAGE_TYPE_CHARACTER},
        {"\\1aFF\\2aFF", "MMMM", IMAGE_TYPE_OUTLINE},
        {"\\bord0\\shad6", "MMMM", IMAGE_TYPE_SHADOW},
        {"\\u1\\s1\\bord0\\shad0\\1aFF\\2aFF\\5a$80", "MMMM", IMAGE_TYPE_CHARACTER},
        {"\\u1\\5gra(0,$0,$200)", "MMMM", IMAGE_TYPE_CHARACTER},
        {"", "<MMMM|ruby\\Nruby|gyaku>", IMAGE_TYPE_CHARACTER},
        {"\\bord0\\shad0\\p1", "m 0 0 l 80 0 80 50 0 50", IMAGE_TYPE_CHARACTER},
        {"\\kt200\\kf200\\3sc&H0000FF&", "MMMM", IMAGE_TYPE_OUTLINE},
        {"\\1grd(35,&H0000FF&,50%,&H00FF00&,&HFF0000&)", "MMMM", IMAGE_TYPE_CHARACTER},
        {"\\1pgrd(0,0,320,200,0,&HFF0000&,&H0000FF&)", "MMMM", IMAGE_TYPE_CHARACTER},
        {"\\1gra(0,$20,50%,$100,$200)", "MMMM", IMAGE_TYPE_CHARACTER},
        {"\\va($0,$100,$200,$255)", "MMMM", IMAGE_TYPE_CHARACTER},
        {"\\1cyc(1,&H0000FF&,&H00FF00&)", "MMMM", IMAGE_TYPE_CHARACTER},
        {"\\polc&H0000FF&\\pols3\\polsp7\\zpol1", "MMMM", IMAGE_TYPE_CHARACTER},
        {"\\frz25\\fax0.2\\fry15", "MMMM", IMAGE_TYPE_CHARACTER},
        {"\\distort(1,0,1.1,1,0,1,0,0)", "MMMM", IMAGE_TYPE_CHARACTER},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        char base[2048], hidden[2048]; Frame a = {0}, b = {0};
        snprintf(base,sizeof(base),"{" POS "%s}%s",cases[i].setup,cases[i].body);
        snprintf(hidden,sizeof(hidden),"{" POS "%s" FULL "}%s",cases[i].setup,cases[i].body);
        ok &= render(lib,renderer,base,0,&a) && render(lib,renderer,hidden,0,&b);
        if (!coverage(&a,cases[i].plane) || coverage(&b,ART)) {
            fprintf(stderr,"surface not visibly masked: %s %s\n",cases[i].setup,cases[i].body); ok = false;
        }
        release(&a); release(&b);
        ok &= masked_pixels(lib,renderer,cases[i].setup,cases[i].body,0,0,W,H,128,128,0);
    }
    for (int layer = 1; layer <= 10; layer++) {
        char setup[256];
        snprintf(setup,sizeof(setup),"\\%dbs%d\\%dba$100",layer,3+layer,layer);
        ok &= masked_pixels(lib,renderer,setup,"MMMM",0,0,W,H,128,128,0);
        char text[1024]; Frame a = {0}, b = {0};
        snprintf(text,sizeof(text),"{" POS "%s}MMMM",setup); ok &= render(lib,renderer,text,0,&a);
        snprintf(text,sizeof(text),"{" POS "%s" FULL "}MMMM",setup); ok &= render(lib,renderer,text,0,&b);
        ok &= coverage(&a,IMAGE_TYPE_OUTLINE)>0 && coverage(&b,IMAGE_TYPE_OUTLINE)==0;
        release(&a); release(&b);
    }
    const uint8_t image[] = {255,60,0,128, 0,255,0,0};
    ok &= ass_set_tag_image_rgba(renderer,"alpha.png",ASS_TAG_IMAGE_FORMAT_PNG,2,1,8,image)==0;
    ok &= masked_pixels(lib,renderer,"\\1img(alpha.png)","MMMM",0,0,W,H,128,128,0);
    ok &= masked_pixels(lib,renderer,"\\3img(alpha.png)","MMMM",80,40,240,160,0,255,0);
    ok &= masked_pixels(lib,renderer,"\\p1\\bord0\\shad0\\img(alpha.png)",
                         "m 0 0 l 80 0 80 50 0 50",0,0,W,H,128,128,0);
    return ok;
}
static bool rectangles(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    const char *alpha[] = {"0","$100","FF"};
    for (size_t i=0;i<sizeof(alpha)/sizeof(alpha[0]);i++) {
        char setup[128]; snprintf(setup,sizeof(setup),"\\alpha%s",alpha[i]);
        ok &= masked_pixels(lib,renderer,setup,"MMMM",0,0,W,H,0,0,0);
        ok &= masked_pixels(lib,renderer,setup,"MMMM",0,0,W,H,255,255,0);
        ok &= masked_pixels(lib,renderer,setup,"MMMM",0,0,W,H,128,128,0);
        ok &= masked_pixels(lib,renderer,setup,"MMMM",0,0,20,20,255,255,0);
        ok &= masked_pixels(lib,renderer,setup,"MMMM",120,40,200,160,0,255,0);
    }
    ok &= masked_pixels(lib,renderer,"\\fad(1000,1000)","MMMM",120,40,200,160,0,255,500);
    ok &= masked_pixels(lib,renderer,"\\fade(0,128,255,0,1000,5000,6000)","MMMM",0,0,W,H,128,128,1000);
    ok &= masked_pixels(lib,renderer,"\\clip(110,50,210,150)","MMMM",120,40,200,160,0,255,0);
    ok &= masked_pixels(lib,renderer,"\\iclip(145,70,175,130)","MMMM",120,40,200,160,0,255,0);
    ok &= masked_pixels(lib,renderer,"\\clip(m 100 40 l 220 40 220 160 100 160)","MMMM",120,40,200,160,0,255,0);
    return ok;
}
static bool boxes(ASS_Library *lib, ASS_Renderer *renderer)
{
    const char *setups[] = {"\\bs4\\boxp16\\bbs6\\boxr12", "\\bs4\\boxp16\\bbs6\\2bbs10\\frz20\\fax0.2", "\\bs3"};
    bool ok = true;
    for (size_t i=0;i<sizeof(setups)/sizeof(setups[0]);i++) {
        char base[1024],masked[1024]; Frame a={0},b={0};
        snprintf(base,sizeof(base),"{" POS "%s}MMMM",setups[i]);
        snprintf(masked,sizeof(masked),"{" POS "%s" FULL "}MMMM",setups[i]);
        ok &= render(lib,renderer,base,0,&a) && render(lib,renderer,masked,0,&b);
        ok &= coverage(&a,IMAGE_TYPE_CHARACTER)>0 && coverage(&b,IMAGE_TYPE_CHARACTER)==0;
        int first_box = i==2 ? IMAGE_TYPE_OUTLINE : IMAGE_TYPE_SHADOW;
        for(int plane=first_box;plane<3;plane++)
            if(memcmp(pixel(&a,plane,0,0),pixel(&b,plane,0,0),(size_t)W*H*4)) {
                fprintf(stderr,"box changed: %s plane%d\n",setups[i],plane);ok=false;
            }
        ok &= coverage(&a,first_box)>0;
        release(&a);release(&b);
    }
    const char *chat[] = {"\\chatmode1\\msgshowname1", "\\chatmode2\\msgshowname1", "\\chatmode3\\msgshowname1"};
    for(size_t i=0;i<sizeof(chat)/sizeof(chat[0]);i++) {
        char base[1024],masked[1024];Frame a={0},b={0};
        snprintf(base,sizeof(base),"{%s\\shad0}|{\\bubbs3}Miku:\\NMMMM|",chat[i]);
        snprintf(masked,sizeof(masked),"{%s\\shad0" FULL "}|{\\bubbs3}Miku:\\NMMMM|",chat[i]);
        ok &= render(lib,renderer,base,0,&a) && render(lib,renderer,masked,0,&b);
        if(!coverage(&a,IMAGE_TYPE_CHARACTER) || coverage(&b,IMAGE_TYPE_CHARACTER) ||
            memcmp(pixel(&a,IMAGE_TYPE_SHADOW,0,0),pixel(&b,IMAGE_TYPE_SHADOW,0,0),(size_t)W*H*4)) {
            fprintf(stderr,"chat text/box exemption failed: %s\n",chat[i]);ok=false;
        }
        release(&a);release(&b);
    }
    return ok;
}
static bool scopes_and_parser(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok = true;
    const char *invalid[] = {
        "\\pgrdalpha(0,0,320,200,0,$0,$256)","\\pgrdalpha(0,0,320,200,0,$0,$abc)",
        "\\pgrdalpha(0,0,320,200,0,$0,$)","\\pgrdalpha(0,0,320,200,0,$0,100)",
        "\\pgrdalpha(0,0,320,200,0,$0,FF","\\pgrdalpha0","\\pgrdalpha",
        "\\pgrdalpha(0,0,320,200,0,$0,70%,$100,30%,$128,FF)",
        "\\pgrdalpha(0,0,320,200,0,$0,50%,$128)",
        "\\pgrdalpha(nan,0,320,200,0,00,FF)","\\pgrdalpha(0,0,320,200,inf,00,FF)",
        "\\pgrdalpha(0,,320,200,0,00,FF)","\\pgrdalpha(0,0,320,200,0,00,,FF)",
        "\\1pgrdalpha(0,0,320,200,0,00,FF)","\\2bpgrdalpha(0,0,320,200,0,00,FF)",
        "\\pgrdalphax(0,0,320,200,0,00,FF)",
    };
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        char text[2048];
        snprintf(text,sizeof(text),"{" POS "\\1a$100\\1gra(0,$0,$200)" HALF "%s}MMMM",invalid[i]);
        ok &= equal(lib,renderer,text,"{" POS "\\1a$100\\1gra(0,$0,$200)" HALF "}MMMM",0,"atomic positioned alpha parse");
    }
    ok &= equal(lib,renderer,"{" POS "\\1gra(0,$0,$200)\\1img(alpha.png)" FULL "\\pgrdalpha()}MMMM",
                 "{" POS "\\1gra(0,$0,$200)\\1img(alpha.png)}MMMM",0,"reset preserves layer paint");
    ok &= equal(lib,renderer,"{" POS FULL "\\r}MMMM","{" POS "\\r}MMMM",0,"style reset clears global mask");
    ok &= equal(lib,renderer,"{" POS "\\pgrdalpha(0,0,320,200,0,80,80)}MMMM",
                 "{" POS HALF "}MMMM",0,"hex alpha is hexadecimal");
    ok &= equal(lib,renderer,"{" POS "\\pgrdalpha(0,0,320,200,0,$\xE1\x81\x81\xE1\x81\x82\xE1\x81\x88,$128)}MMMM",
                 "{" POS HALF "}MMMM",0,"Unicode decimal alpha");
    ok &= equal(lib,renderer,"{" POS "\\pgrdalpha(320,200,0,0,0,$128,$128)}MMMM",
                 "{" POS HALF "}MMMM",0,"normalized rectangle");
    ok &= equal(lib,renderer,"{" POS "\\pgrdalpha(0,0,0,200,0,$255,$255)}MMMM",
                 "{" POS "}MMMM",0,"degenerate positioned rectangle falls back");
    Frame a={0},b={0};
    ok &= render(lib,renderer,"{\\an7\\pos(20,60)\\bord0\\shad0}M M M",0,&a);
    ok &= render(lib,renderer,"{\\an7\\pos(20,60)\\bord0\\shad0}M{" FULL "} M{\\pgrdalpha()} M",0,&b);
    int part=0;bool ink_before=false;unsigned removed=0;
    for(int x=0;x<W;x++) {
        bool ink=false;for(int y=0;y<H;y++) ink |= pixel(&a,ART,x,y)[3]!=0;
        if(ink && !ink_before)part++;
        for(int y=0;y<H;y++) for(int c=0;c<4;c++) {
            uint8_t expected=part==2 && ink ? 0 : pixel(&a,ART,x,y)[c];
            if(pixel(&b,ART,x,y)[c]!=expected)ok=false;
            removed += part==2 && pixel(&a,ART,x,y)[c]>0;
        }
        ink_before=ink;
    }
    if(part!=3 || !removed || !ok)fprintf(stderr,"mid-line mask/reset scope failed (%d parts)\n",part);
    release(&a);release(&b);return ok;
}
static bool transforms_and_events(ASS_Library *lib, ASS_Renderer *renderer)
{
    bool ok=true;
    const char *anim="{" POS "\\pgrdalpha(40,20,280,180,350,$0,50%,$100,$200)"
        "\\t(0,1000,\\pgrdalpha(80,40,240,160,10,$200,50%,$0,$100))}MMMM";
    const int times[]={0,250,500,750,1000,500,0,1000,250};
    ASS_Track *seek_track=track_for(lib,anim);
    if(!seek_track)return false;
    for(size_t i=0;i<sizeof(times)/sizeof(times[0]);i++) {
        Frame a={0},b={0};ok &= render_track(renderer,seek_track,times[i],&a) && render(lib,renderer,anim,times[i],&b);
        ok &= !memcmp(a.pixels,b.pixels,(size_t)PLANES*W*H*4);
        release(&a);release(&b);
    }
    int change = 0;
    ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, seek_track, 250, &change);
    ass_free_images_rgba(images);
    images = ass_render_frame_rgba(renderer, seek_track, 750, &change);
    ok &= change == 2;
    ass_free_images_rgba(images);
    ass_free_track(seek_track);
    ok &= equal(lib,renderer,anim,"{" POS "\\pgrdalpha(60,30,260,170,0,$100,50%,$50,$150)}MMMM",
                 500,"rectangle angle and stop interpolation midpoint");
    ok &= equal(lib,renderer,"{" POS OPAQUE "\\t(0,1000,\\pgrdalpha(0,0,320,200,0,$0,$255))}MMMM",
                 "{" POS "\\pgrdalpha(0,0,320,200,0,$0,$127)}MMMM",500,"alpha stop interpolation");
    ok &= equal(lib,renderer,"{" POS HALF "\\t(0,1000,\\pgrdalpha())}MMMM",
                 "{" POS HALF "}MMMM",500,"nested reset matches positioned gradient policy");
    ASS_Track *track=track_for(lib,"{\\an7\\pos(20,40)" FULL "}MMMM");
    if(!track)return false;
    int index=ass_alloc_event(track);
    if(index<0){ass_free_track(track);return false;}
    ASS_Event *next=&track->events[index];
    next->Start=0;next->Duration=10000;next->Style=0;
    next->Text=malloc(128);
    if(!next->Text){ass_free_track(track);return false;}
    strcpy(next->Text,"{\\an7\\pos(20,130)" OPAQUE "}MMMM");
    Frame actual={0},expected={0};
    ok &= render_track(renderer,track,0,&actual) &&
        render(lib,renderer,"{\\an7\\pos(20,130)}MMMM",0,&expected);
    ok &= !memcmp(actual.pixels,expected.pixels,(size_t)PLANES*W*H*4);
    release(&actual);release(&expected);ass_free_track(track);
    // Normal non-positioned dialogues collide vertically. Their mask must be
    // sampled after the collision shift, not in the original event coordinates.
    track=track_for(lib,"{\\pgrdalpha(0,0,320,100,0,$255,$255)}MMMM");
    if(!track)return false;
    index=ass_alloc_event(track);
    if(index<0){ass_free_track(track);return false;}
    next=&track->events[index];
    next->Start=0;next->Duration=10000;next->Style=0;next->Text=malloc(64);
    if(!next->Text){ass_free_track(track);return false;}
    strcpy(next->Text,"MMMM");
    ok &= render_track(renderer,track,0,&actual);
    ok &= coverage(&actual,ART)>0;
    release(&actual);ass_free_track(track);
    return ok;
}
static bool blend_output(ASS_Library *lib,ASS_Renderer *renderer)
{
    bool ok=true;
    for(int mode=1;mode<=15;mode++) {
        char plain[256],opaque[512],hidden[512];Frame a={0},b={0},c={0};
        snprintf(plain,sizeof(plain),"{" POS "\\blend%d}MMMM",mode);
        snprintf(opaque,sizeof(opaque),"{" POS "\\blend%d" OPAQUE "}MMMM",mode);
        snprintf(hidden,sizeof(hidden),"{" POS "\\blend%d" FULL "}MMMM",mode);
        ok &= render(lib,renderer,plain,0,&a) && render(lib,renderer,opaque,0,&b) && render(lib,renderer,hidden,0,&c);
        ok &= !memcmp(a.video,b.video,(size_t)W*H*4);
        for(int i=0;i<W*H;i++) ok &= c.video[4*i]==37 && c.video[4*i+1]==59 && c.video[4*i+2]==83;
        release(&a);release(&b);release(&c);
    }
    return ok;
}
static bool load_font(ASS_Library *lib,const char *path)
{
    FILE *file=fopen(path,"rb");if(!file)return false;
    char *data=NULL;bool ok=false;
    if(fseek(file,0,SEEK_END))goto done;
    long n=ftell(file);if(n<=0 || n>INT_MAX || fseek(file,0,SEEK_SET))goto done;
    data=malloc(n);if(!data || fread(data,1,n,file)!=(size_t)n)goto done;
    ass_add_font(lib,path,data,(int)n);ok=true;
done:free(data);fclose(file);return ok;
}
int main(void)
{
    ASS_Library *lib=ass_library_init();if(!lib)return 2;
    const char *font=getenv("ALPHA_TEST_FONT");if(!font)font="compare/test/font1.ttf";
    if(!load_font(lib,font)){ass_library_done(lib);return 2;}
    ASS_Renderer *renderer=ass_renderer_init(lib);if(!renderer){ass_library_done(lib);return 2;}
    ass_set_frame_size(renderer,W,H);ass_set_storage_size(renderer,W,H);
    ass_set_fonts(renderer,font,"sans-serif",ASS_FONTPROVIDER_NONE,NULL,1);
    bool ok=rectangles(lib,renderer);
    ok &= surfaces(lib,renderer);
    ok &= boxes(lib,renderer);
    ok &= scopes_and_parser(lib,renderer);
    ok &= transforms_and_events(lib,renderer);
    ok &= blend_output(lib,renderer);
    ass_renderer_done(renderer);ass_library_done(lib);return ok?0:1;
}
