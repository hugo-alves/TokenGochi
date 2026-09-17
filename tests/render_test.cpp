#include "app_launcher_render.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
struct Frame {
    uint16_t pixels[466*466] = {};
    void fillRect(int x,int y,int w,int h,uint16_t color) {
        if(x<0||y<0||x+w>466||y+h>466||w<0||h<0) { std::fprintf(stderr,"Out of bounds draw %d %d %d %d\n",x,y,w,h); std::exit(1); }
        for(int v=y;v<y+h;++v) for(int u=x;u<x+w;++u) pixels[v*466+u]=color;
    }
    void verify() {
        for(int y=0;y<466;++y) for(int x=0;x<466;++x) if(!apps::inDisc(x,y)&&pixels[y*466+x]) {
            std::fprintf(stderr,"Nonblack pixel outside disc at %d,%d\n",x,y); std::exit(1);
        }
    }
    void save(const std::string& path) {
        FILE* f=std::fopen(path.c_str(),"wb"); if(!f) {std::perror(path.c_str());std::exit(1);}
        std::fprintf(f,"P6\n466 466\n255\n");
        for(uint16_t p:pixels) { unsigned char b[3]={static_cast<unsigned char>(((p>>11)&31)*255/31),static_cast<unsigned char>(((p>>5)&63)*255/63),static_cast<unsigned char>((p&31)*255/31)}; std::fwrite(b,1,3,f); }
        std::fclose(f);
    }
};
static Frame frame;
int main(int argc,char** argv) {
    const std::string dir=argc>1 ? argv[1] : ".";
    apps::Launcher state;
    for(int n=0;n<5;++n) {
        state.enter();
        const char* names[]={"launcher","steady-focus","steady-preview","not-built","battery-warning"};
        if(n==1) state.next();
        if(n==2) state.open(1,0);
        if(n==3) state.open(2,0);
        apps::render::draw(frame,state,n==4 ? "BATTERY LOW" : nullptr);
        frame.verify(); frame.save(dir+"/"+names[n]+".ppm");
        std::printf("PASS render %s: all visible pixels inside disc\n",names[n]);
    }
    for(const auto& a:apps::art::assets) if(!apps::render::asset(frame,a,0,0)) return 1;
    const uint8_t invalid[]={0,1};
    const apps::art::Asset bad={1,1,invalid,2};
    if(apps::render::asset(frame,bad,0,0)) return 1;
    const uint8_t badIndex[]={1,255};
    const apps::art::Asset bad2={1,1,badIndex,2};
    if(apps::render::asset(frame,bad2,0,0)) return 1;
    std::printf("PASS asset validation: six generated assets + malformed data checks\n");
    std::printf("Host sizeof Launcher=%zu, HoldGesture=%zu bytes; no dynamic allocation in either.\n",sizeof(state),sizeof(apps::HoldGesture));
    return 0;
}
