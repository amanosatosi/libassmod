/* Native vertical ruby: public API geometry and pixel contracts. No OS fonts. */
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ass.h"

enum { W = 640, H = 480 };
#define ANCHOR "\\an5\\pos(320,240)"
#define VERT ANCHOR "\\vert1"
#define FONT "Native Vertical Furi"
typedef struct { uint8_t *p; } Frame;
typedef struct { int x0, y0, x1, y1, count; double x, y; uint64_t mass; } Bounds;
static int failures;
static void check(bool ok, const char *label)
{ if (!ok) { fprintf(stderr, "FAIL: %s\n", label); failures++; } }
static void release(Frame *f) { free(f->p); f->p = NULL; }
static bool load_font(ASS_Library *lib, const char *path)
{
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) return false;
    bool ok = !fseek(file, 0, SEEK_END);
    long n = ok ? ftell(file) : -1;
    ok = n > 0 && n <= INT_MAX && !fseek(file, 0, SEEK_SET);
    char *data = ok ? malloc(n) : NULL;
    ok = data && fread(data, 1, n, file) == (size_t)n;
    if (ok) ass_add_font(lib, path, data, (int)n);
    free(data); fclose(file); return ok;
}
static ASS_Track *track_for(ASS_Library *lib, const char *text)
{
    char script[16384];
    snprintf(script, sizeof(script),
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 480\n"
        "ScaledBorderAndShadow: yes\nWrapStyle: 0\n[V4+ Styles]\n"
        "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
        "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
        "Alignment,MarginL,MarginR,MarginV,Encoding\n"
        "Style: Default," FONT ",48,&H000000FF,&H00FF0000,&H0000FF00,&H00FFFFFF,"
        "0,0,0,0,100,100,0,0,1,0,0,5,20,20,20,1\n"
        "[Events]\nFormat: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n"
        "Dialogue: 0,0:00:00.00,0:00:05.00,Default,,0,0,0,,%s\n", text);
    return ass_read_memory(lib, script, strlen(script), NULL);
}
static void over(uint8_t *d, const uint8_t *s)
{ for (int c = 0; c < 4; c++) d[c] = s[c] + (d[c] * (255-s[3]) + 127) / 255; }
static bool render_track(ASS_Renderer *renderer, ASS_Track *track,
                         long long now, bool rgba, bool all, Frame *f)
{
    release(f); f->p = calloc((size_t) W * H, 4);
    if (!f->p) return false;
    if (rgba) {
        ASS_ImageRGBA *images = ass_render_frame_rgba(renderer, track, now, NULL);
        for (ASS_ImageRGBA *im = images; im; im = im->next) {
            if (!all && im->type != IMAGE_TYPE_CHARACTER) continue;
            for (int y=0; y<im->h; y++) for (int x=0; x<im->w; x++) {
                int px=x+im->dst_x, py=y+im->dst_y;
                if (px<0 || py<0 || px>=W || py>=H) continue;
                const uint8_t *s=im->rgba+(size_t)y*im->stride+4*x;
                if (s[0]>s[3] || s[1]>s[3] || s[2]>s[3]) {
                    ass_free_images_rgba(images); return false;
                }
                over(f->p+4*((size_t)py*W+px),s);
            }
        }
        ass_free_images_rgba(images);
    } else {
        for (ASS_Image *im=ass_render_frame(renderer,track,now,NULL);im;im=im->next) {
            if (!all && im->type != IMAGE_TYPE_CHARACTER) continue;
            for (int y=0;y<im->h;y++) for(int x=0;x<im->w;x++) {
                int px=x+im->dst_x, py=y+im->dst_y;
                if(px<0 || py<0 || px>=W || py>=H) continue;
                unsigned a=(im->bitmap[(size_t)y*im->stride+x]*(255-(im->color&255))+127)/255;
                uint8_t s[4]={(im->color>>24)*a/255,((im->color>>16)&255)*a/255,
                             ((im->color>>8)&255)*a/255,a};
                over(f->p+4*((size_t)py*W+px),s);
            }
        }
    }
    return true;
}
static bool render(ASS_Library *lib, ASS_Renderer *renderer, const char *text,
                    long long now, bool rgba, bool all, Frame *f)
{
    ASS_Track *t=track_for(lib,text);
    if(!t) return false;
    bool ok=render_track(renderer,t,now,rgba,all,f);
    ass_free_track(t); return ok;
}
static bool equal(const Frame *a,const Frame *b)
{ return a->p && b->p && !memcmp(a->p,b->p,(size_t)W*H*4); }
static Bounds bounds(const Frame *f, const Frame *subtract, int channel,
                      int x0, int x1)
{
    Bounds b={.x0=W,.y0=H,.x1=0,.y1=0};
    for(int y=0;y<H;y++) for(int x=x0;x<x1;x++) {
        size_t k=4*((size_t)y*W+x);
        int a=f->p[k+3];
        if(subtract && subtract->p[k+3]) continue;
        if(channel>=0 && f->p[k+channel] <= f->p[k+(channel==0?2:0)]) continue;
        if(!a) continue;
        b.x0=x<b.x0?x:b.x0; b.x1=x+1>b.x1?x+1:b.x1;
        b.y0=y<b.y0?y:b.y0; b.y1=y+1>b.y1?y+1:b.y1;
        b.count++; b.mass+=a; b.x+=a*x; b.y+=a*y;
    }
    if(b.mass) { b.x/=b.mass; b.y/=b.mass; }
    return b;
}
static bool base_preserved(const Frame *base,const Frame *ruby)
{
    for(size_t i=0;i<(size_t)W*H;i++)
        if(base->p[4*i+3] && memcmp(base->p+4*i,ruby->p+4*i,4)) return false;
    return true;
}
static bool pair(ASS_Library *lib,ASS_Renderer *r,const char *a,const char *b,
                  bool rgba,bool same,const char *label)
{
    Frame fa={0},fb={0};
    bool ok=render(lib,r,a,250,rgba,true,&fa) && render(lib,r,b,250,rgba,true,&fb);
    ok=ok && (equal(&fa,&fb)==same);
    check(ok,label);release(&fa);release(&fb);return ok;
}
static void geometry(ASS_Library *lib,ASS_Renderer *r,bool rgba)
{
    Frame base={0},right={0},left={0},both={0},other={0};
    char text[2048];
    for(int direction=1;direction<=2;direction++) {
        snprintf(text,sizeof(text),"{" VERT "\\vdir%d}漢字",direction);
        check(render(lib,r,text,0,rgba,false,&base),"base render");
        Bounds b=bounds(&base,NULL,-1,0,W);
        snprintf(text,sizeof(text),"{" VERT "\\vdir%d}<漢字|かんじ>",direction);
        check(render(lib,r,text,0,rgba,false,&right),"normal ruby render");
        Bounds a=bounds(&right,&base,-1,0,W);
        check(base_preserved(&base,&right),"compact normal base exact pixels");
        check(a.count && a.x0>b.x1,"normal ruby physical right");
        check(fabs((a.y0+a.y1-b.y0-b.y1)*0.5)<=1.5,"normal Y centering");
        snprintf(text,sizeof(text),"{" VERT "\\vdir%d}<漢字||かんじ>",direction);
        check(render(lib,r,text,0,rgba,false,&left),"gyaku render");
        Bounds c=bounds(&left,&base,-1,0,W);
        check(base_preserved(&base,&left),"compact gyaku base exact pixels");
        check(c.count && c.x1<b.x0,"gyaku ruby physical left");
        check(fabs((c.y0+c.y1-b.y0-b.y1)*0.5)<=1.5,"gyaku Y centering");
        snprintf(text,sizeof(text),"{" VERT "\\vdir%d}<漢字|かんじ|かんじ>",direction);
        check(render(lib,r,text,0,rgba,false,&both),"two side render");
        bool union_ok=true;
        for(size_t k=0;k<(size_t)W*H*4;k++)
            if(both.p[k] != (right.p[k]>left.p[k]?right.p[k]:left.p[k])) union_ok=false;
        check(union_ok,"both sides independent exact union");
        snprintf(text,sizeof(text),"{" VERT "\\vdir%d\\furiplaceauto1}<漢字|かんじ|かんじ>",direction);
        check(render(lib,r,text,0,rgba,false,&other) && equal(&both,&other),"placeauto does not swap sides");
    }
    for(int an=1;an<=9;an++) {
        snprintf(text,sizeof(text),"{\\an%d\\pos(320,240)\\vert1}漢字",an);
        render(lib,r,text,0,rgba,false,&base);
        snprintf(text,sizeof(text),"{\\an%d\\pos(320,240)\\vert1}<漢字|かんじ|カンジ>",an);
        render(lib,r,text,0,rgba,false,&both);
        check(base_preserved(&base,&both),"all nine compact anchors preserve base");
    }
    render(lib,r,"{" VERT "}漢字",0,rgba,false,&base);
    render(lib,r,"{" VERT "}<漢字|かんじ>",0,rgba,false,&right);
    render(lib,r,"{" VERT "\\furipos(9,7)}<漢字|かんじ>",0,rgba,false,&other);
    Bounds a=bounds(&right,&base,-1,0,W),c=bounds(&other,&base,-1,0,W);
    check(abs(c.x0-a.x0-9)<=1 && abs(c.y0-a.y0+7)<=1,"furipos screen axes");
    render(lib,r,"{" VERT "\\furis80}<漢字|かんじ>",0,rgba,false,&other);
    c=bounds(&other,&base,-1,0,W);
    check(c.x1-c.x0>a.x1-a.x0 && c.y1-c.y0>a.y1-a.y0,"furis scales both reading axes");
    render(lib,r,"{" VERT "\\furifsp8}<漢字|かんじ>",0,rgba,false,&other);
    c=bounds(&other,&base,-1,0,W);
    check(c.y1-c.y0>a.y1-a.y0+5,"furifsp advances Y");
    render(lib,r,"{" VERT "}時",0,rgba,false,&base);
    render(lib,r,"{" VERT "\\furistyle1}<時|とてもながいよみかた>",0,rgba,false,&right);
    render(lib,r,"{" VERT "\\furistyle2}<時|とてもながいよみかた>",0,rgba,false,&other);
    a=bounds(&right,&base,-1,0,W);c=bounds(&other,&base,-1,0,W);
    check(c.count && c.y1-c.y0<a.y1-a.y0,"style2 fits vertical flow axis");
    check(base_preserved(&base,&right) && base_preserved(&base,&other),"long reading base stable");
    release(&base);release(&right);release(&left);release(&both);release(&other);
}
static void columns(ASS_Library *lib,ASS_Renderer *r,bool rgba)
{
    Frame f={0};char text[2048];
    for(int dir=1;dir<=2;dir++) for(int mode=0;mode<=2;mode++) {
        snprintf(text,sizeof(text),"{" VERT "\\vdir%d\\furichangepos%d\\furis90}"
            "<漢字|かんじ|カンジ>\\N{\\1c&HFF0000&}<文字|もじ|もじ>",dir,mode);
        check(render(lib,r,text,0,rgba,false,&f),"annotated columns render");
        Bounds a=bounds(&f,NULL,0,0,W),b=bounds(&f,NULL,2,0,W);
        check(a.count && b.count && (dir==1?b.x1<a.x0:a.x1<b.x0),"column bands do not overlap");
        snprintf(text,sizeof(text),"{" VERT "\\vdir%d\\furichangepos%d}"
            "<漢字|かんじ|カンジ>\\N{\\1c&HFF0000&}文字",dir,mode);
        check(render(lib,r,text,0,rgba,false,&f),"annotated adjacent ordinary column");
        a=bounds(&f,NULL,0,0,W);b=bounds(&f,NULL,2,0,W);
        check(a.count && b.count && (dir==1?b.x1<a.x0:a.x1<b.x0),"ruby clears ordinary adjacent column");
    }
    /* Overflow to a new column must keep the entire associated base together. */
    check(render(lib,r,"{" VERT "}漢字漢字漢字漢字{\\1c&HFF0000&}<日本語|にほんご>",
        0,rgba,false,&f),"automatic wrapped ruby");
    Bounds a=bounds(&f,NULL,0,0,W),b=bounds(&f,NULL,2,0,W);
    check(a.count && b.count && b.x1<a.x0,"automatic column wrapping reserves ruby");
    release(&f);
}
static void karaoke(ASS_Library *lib,ASS_Renderer *r,bool rgba)
{
    Frame a={0},b={0};
    const char *text="{" VERT "}<漢字|{\\kf40}かん{\\kf60}じ>";
    check(render(lib,r,text,200,rgba,false,&a),"vertical ruby karaoke render");
    Bounds red=bounds(&a,NULL,0,0,W),blue=bounds(&a,NULL,2,0,W);
    check(red.count && blue.count && red.y<blue.y,"ruby/base karaoke wipe top to bottom");
    /* At one third of the total reading span the second base glyph must still
     * be waiting. This distinguishes an ink projection from a displaced
     * horizontal baseline used as the vertical range's origin. */
    Frame control={0};
    render(lib,r,"{" VERT "}漢字",200,rgba,false,&control);
    Bounds base=bounds(&control,NULL,-1,0,W);
    int split=(base.y0+base.y1)/2;
    bool lower_waits=true; unsigned waiting=0;
    for(int y=split;y<base.y1;y++) for(int x=base.x0;x<base.x1;x++) {
        size_t k=4*((size_t)y*W+x);
        if(!control.p[k+3]) continue;
        if(a.p[k] > a.p[k+2]) lower_waits=false;
        if(a.p[k+2] > a.p[k]) waiting++;
    }
    check(lower_waits && waiting,"internal ruby karaoke maps to base ink span");
    release(&control);
    check(render(lib,r,"{" VERT "}<漢字|{\\k0}{\\kf100}かんじ|{\\kf100}カンジ>",
        500,rgba,false,&b),"empty and independent two-sided karaoke segments");
    red=bounds(&b,NULL,0,0,W);blue=bounds(&b,NULL,2,0,W);
    check(red.count && blue.count,"two-sided karaoke retains timing colors");
    ASS_Track *track=track_for(lib,text);
    check(track!=NULL,"seeking track allocation");
    if(track) {
        Frame first={0},repeat={0};
        render_track(r,track,250,rgba,true,&first);
        render_track(r,track,900,rgba,true,&repeat);
        render_track(r,track,50,rgba,true,&repeat);
        render_track(r,track,250,rgba,true,&repeat);
        check(equal(&first,&repeat),"seeking and repeated-frame determinism");
        release(&first);release(&repeat);ass_free_track(track);
    }
    release(&a);release(&b);
}
int main(void)
{
    ASS_Library *lib=ass_library_init();if(!lib)return 2;
    const char *font=getenv("VERTICAL_FURI_FONT");
    if(!font)font="test/fonts/native-vertical-furi.ttf";
    if(!load_font(lib,font)){ass_library_done(lib);return 2;}
    const char *myanmar=getenv("FURI_MYANMAR_FONT");
    if(myanmar && !load_font(lib,myanmar)){ass_library_done(lib);return 2;}
    ASS_Renderer *r=ass_renderer_init(lib);if(!r){ass_library_done(lib);return 2;}
    ass_set_frame_size(r,W,H);ass_set_storage_size(r,W,H);
    ass_set_fonts(r,font,FONT,ASS_FONTPROVIDER_NONE,NULL,1);
    for(int rgba=0;rgba<=1;rgba++) {
        geometry(lib,r,rgba);columns(lib,r,rgba);karaoke(lib,r,rgba);
        pair(lib,r,"{" VERT "\\furi0}<漢字|かんじ>",
            "{" VERT "}\\<漢字\\|かんじ\\>",rgba,true,"disabled/literal markup");
        pair(lib,r,"{" VERT "}<漢字|>","{" VERT "}\\<漢字\\|\\>",rgba,true,"empty reading is literal");
        pair(lib,r,"{" VERT "}<漢字|かんじ|カンジ|字>",
            "{" VERT "}\\<漢字\\|かんじ\\|カンジ\\|字\\>",rgba,true,"malformed group literal");
        pair(lib,r,"{" ANCHOR "}<漢字|かんじ|カンジ>",
            "{" ANCHOR "\\vert0}<漢字|かんじ|カンジ>",rgba,true,"horizontal vert0 compatibility");
        pair(lib,r,"{" VERT "\\furis150\\furichangepos0}<漢字|かんじ>",
            "{" VERT "\\furis150\\furichangepos1}<漢字|かんじ>",rgba,false,"block anchors occupied ruby band");
        pair(lib,r,"{" VERT "\\furichangepos0}<漢字|かんじ>",
            "{" VERT "\\furichangepos2}<漢字|かんじ>",rgba,true,"strict retains compact single-column base anchor");
        pair(lib,r,"{" VERT "}漢字", "{" VERT "\\furis90\\furichangepos2}漢字",
             rgba,true,"ordinary vertical metrics unchanged");
        pair(lib,r,"{" VERT "}<漢字|かんじ>","{" VERT "\\furis0\\r}<漢字|かんじ>",
             rgba,true,"style reset restores ruby controls");
        pair(lib,r,"{" VERT "}<漢字||ဆာတို့>",
            "{" VERT "\\furis80}<漢字||ဆာတို့>",rgba,false,"Myanmar cluster geometry and scale");
        pair(lib,r,"{" VERT "\\fnNoto Sans Myanmar}<မြန်မာ|ဆာတို့|စူဇူကီ>",
            "{" VERT "\\fnNoto Sans Myanmar\\furis80}<မြန်မာ|ဆာတို့|စူဇူကီ>",rgba,false,"real Myanmar shaping fallback");
        pair(lib,r,"{" VERT "}<日本語|にほんご>Latin<漢字||မြန်မာ>",
            "{" VERT "\\furifsp4}<日本語|にほんご>Latin<漢字||မြန်မာ>",rgba,false,"mixed scripts and ruby spacing");
    }
    uint8_t image[] = {255,255,255,255, 20,180,240,128};
    check(ass_set_tag_image_rgba(r,"vertical.png",ASS_TAG_IMAGE_FORMAT_PNG,2,1,8,image)==0,
          "image fill fixture registration");
    pair(lib,r,"{" VERT "}<漢字|\\<か\\|ん\\>じ>",
        "{" VERT "}<漢字|かんじ>",true,false,"escaped reading delimiters remain ink");
    pair(lib,r,"{" VERT "}<漢字|かんじ>{\\1c&H00FF00&\\furis70}<文字||もじ>",
        "{" VERT "}<漢字|かんじ><文字||もじ>",true,false,"mid-event style captured per group");
    const char *paint[]={"\\bord3\\shad4","\\2bord3\\10bord1\\10bc&HFF0000&",
        "\\1img(vertical.png)","\\3img(vertical.png)\\bord3","\\1gra(0,$0,$200)","\\1pgrd(0,0,640,480,0,&HFF0000&,&H0000FF&)",
        "\\clip(280,190,365,290)","\\fad(1000,1000)","\\blur2\\frz15",
        "\\t(0,1000,\\fscx120\\fscy110)","\\u1\\5c&HFF0000&"};
    for(size_t i=0;i<sizeof(paint)/sizeof(paint[0]);i++) {
        Frame a={0},b={0};char plain[2048],masked[2048];
        snprintf(plain,sizeof(plain),"{" VERT "%s}<漢字|かんじ|カンジ>",paint[i]);
        snprintf(masked,sizeof(masked),"{" VERT "%s\\pgrdalpha(0,0,640,480,0,$255,$255)}<漢字|かんじ|カンジ>",paint[i]);
        bool ok=render(lib,r,plain,250,true,true,&a) && render(lib,r,masked,250,true,true,&b);
        Bounds ba=bounds(&a,NULL,-1,0,W),bb=bounds(&b,NULL,-1,0,W);
        check(ok && ba.count && !bb.count,"all ruby paint surfaces inherit global mask");
        release(&a);release(&b);
    }
    ass_renderer_done(r);ass_library_done(lib);
    return failures?1:0;
}
