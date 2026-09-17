#pragma once
// Same renderer is compiled for the watch, native visual tests, and browser preview.
// Canvas only needs fillRect(x,y,w,h,RGB565). Firmware draws into ui::target(),
// so the existing TGSHOT mirror remains authoritative. No new framebuffer/heap.
#include "app_launcher_model.h"
#include "app_launcher_assets.h"

namespace apps { namespace render {
static const uint8_t alphabet[26][5] = {
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},
    {0x20,0x40,0x41,0x3f,0x01},{0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},{0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f},{0x7f,0x20,0x18,0x20,0x7f},{0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43}
};
inline int textWidth(const char* s, int scale=1) {
    int n=0; while (s && s[n]) ++n;
    return n ? (n*6-1)*scale : 0;
}
inline uint8_t column(char c, int x) {
    if (c>='a' && c<='z') c=static_cast<char>(c-'a'+'A');
    if (c>='A' && c<='Z') return alphabet[c-'A'][x];
    if (c==':') return x==2 ? 0x36 : 0;
    if (c=='.') return x==2 ? 0x40 : 0;
    if (c=='-') return 0x08;
    if (c=='+') return x==2 ? 0x3e : 0x08;
    return 0;
}
template<class Canvas> void text(Canvas& c, const char* s, int x, int y, uint16_t color, int scale=1) {
    for (; s && *s; ++s,x+=6*scale) {
        for (int u=0; u<5; ++u) {
            const uint8_t bits=column(*s,u);
            for (int v=0; v<7; ++v) if (bits & (1<<v)) c.fillRect(x+u*scale,y+v*scale,scale,scale,color);
        }
    }
}
template<class Canvas> void centered(Canvas& c, const char* s, int y, uint16_t color, int scale=1) {
    text(c,s,233-textWidth(s,scale)/2,y,color,scale);
}
inline uint16_t shade(uint16_t color, unsigned percent) {
    return static_cast<uint16_t>(((((color>>11)&31)*percent/100)<<11) |
        ((((color>>5)&63)*percent/100)<<5) | ((color&31)*percent/100));
}
// Generated RLE is bounded; malformed data fails closed instead of overreading.
template<class Canvas> bool asset(Canvas& c, const art::Asset& a, int ox, int oy, unsigned brightness=100) {
    uint32_t pos=0;
    int x=0, y=0;
    while (pos+1<a.bytes && y<a.height) {
        const unsigned length=a.runs[pos++], index=a.runs[pos++];
        if (!length || index>=128 || x+static_cast<int>(length)>a.width) return false;
        const uint16_t color=shade(art::palette[index],brightness);
        if (color) c.fillRect(ox+x,oy+y,static_cast<int>(length),1,color);
        x+=static_cast<int>(length);
        if (x==a.width) { x=0; ++y; }
    }
    return pos==a.bytes && y==a.height && x==0;
}
template<class Canvas> void badge(Canvas& c, const char* label, int cx, int y, uint16_t color) {
    const int width=textWidth(label);
    c.fillRect(cx-width/2-4,y-3,width+8,13,0);
    text(c,label,cx-width/2,y,color);
}
template<class Canvas> void brackets(Canvas& c, const Rect& r, uint16_t color) {
    const int x=r.x+12, y=r.y+12, w=r.w-24, h=r.h-24, len=9;
    for (int ix=0;ix<2;++ix) for (int iy=0;iy<2;++iy) {
        const int px=ix ? x+w-2 : x, py=iy ? y+h-2 : y;
        c.fillRect(ix ? px-len+2 : px,py,len,2,color);
        c.fillRect(px,iy ? py-len+2 : py,2,len,color);
    }
}
inline const char* noticeLabel(int index) {
    switch (index) {
        case 2: return "VIBE IS NOT BUILT YET";
        case 3: return "SONIFIER IS NOT BUILT YET";
        case 4: return "TAP TAP IS NOT BUILT YET";
        default: return "";
    }
}
template<class Canvas> void draw(Canvas& c, const Launcher& state, const char* warning=nullptr) {
    c.fillRect(0,0,466,466,0);
    if (state.view()==View::SteadyPreview) {
        centered(c,"PREVIEW ONLY",31,0xA7E8);
        centered(c,"STEADY",59,0xCFF4,3);
        asset(c,art::assets[1],161,102);
        centered(c,"NOT A PLAYABLE GAME",264,0xFFFF);
        centered(c,"THE LAUNCHER ROUTE IS READY.",289,0x8C71);
        centered(c,"GAME LOGIC IS NOT INSTALLED.",305,0x8C71);
        c.fillRect(143,345,180,56,0xA7E8);
        c.fillRect(145,347,176,52,0x08A1);
        centered(c,"BACK TO APPS",366,0xCFF4,2);
        centered(c,warning ? warning : "A OR B: BACK",418,warning ? 0xFBC0 : 0x9492);
        return;
    }
    asset(c,art::assets[5],189,238,10);
    text(c,"+",175,28,0x52B0);
    text(c,"+",285,28,0x52B0);
    text(c,"+",222,253,0x3188);
    text(c,"+",220,302,0x3188);
    text(c,"+",154,405,0x3188);
    centered(c,"APPS",23,0xDEFF,3);
    const char* status=warning ? warning : (state.selected()==0 ? "PET" : "STEADY PREVIEW");
    if (!warning && state.notice()>=0) status=noticeLabel(state.notice());
    centered(c,status,52,warning ? 0xFBC0 : 0x9CD5);
    for (size_t i=0;i<kAppCount;++i) {
        const Manifest& m=kApps[i];
        const bool disabled=m.availability==Availability::Planned;
        const unsigned brightness=disabled ? 38 : (i==state.selected() ? 100 : 82);
        asset(c,art::assets[i],m.art.x,m.art.y,brightness);
        if (m.availability!=Availability::Ready) {
            badge(c,disabled ? "SOON" : "PREVIEW",m.art.x+m.art.w/2,m.art.y+13,
                  disabled ? 0x94B2 : 0xCFF4);
        }
        if (i==state.selected()) brackets(c,m.art,0xDEFF);
    }
    centered(c,"A:OPEN B:NEXT",451,0xAD55);
}
}} // namespace apps::render
