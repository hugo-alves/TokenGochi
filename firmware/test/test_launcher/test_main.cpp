#include "app_launcher_model.h"
#ifdef LAUNCHER_STANDALONE
#include <cstdio>
#include <cstdlib>
static unsigned assertions=0;
#define CHECK(x) do { ++assertions; if (!(x)) { std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(1); } } while(0)
#define RUN_TEST(f) do { f(); std::printf("PASS %s\n",#f); } while(0)
#else
#include <unity.h>
#define CHECK(x) TEST_ASSERT_TRUE(x)
void setUp() {}
void tearDown() {}
#endif
using namespace apps;
void test_catalog_and_availability() {
    CHECK(kAppCount==5); CHECK(selectable(0)); CHECK(selectable(1));
    CHECK(!selectable(2)); CHECK(!selectable(3)); CHECK(!selectable(4)); CHECK(!selectable(5));
    CHECK(kApps[0].availability==Availability::Ready);
    CHECK(kApps[1].availability==Availability::Preview);
    for (size_t i=0;i<kAppCount;++i) CHECK(hitTest(kApps[i].hit.cx,kApps[i].hit.cy)==static_cast<int>(i));
}
void test_geometry_exhaustive() {
    CHECK(!inDisc(0,0)); CHECK(!inDisc(465,465)); CHECK(!inDisc(233,466));
    CHECK(!inDisc(-32768,32767)); CHECK(hitTest(233,280)==-1);
    for (int y=0;y<466;++y) for (int x=0;x<466;++x) {
        int count=0; for (size_t i=0;i<kAppCount;++i) count+=contains(kApps[i].hit,x,y);
        CHECK(count<=1);
        if (!inDisc(x,y)) CHECK(hitTest(x,y)==-1);
    }
}
void test_navigation_and_preview() {
    Launcher l; l.enter(); CHECK(l.view()==View::Grid); CHECK(l.selected()==0); CHECK(l.dirty());
    l.painted(); CHECK(!l.dirty()); l.next(); CHECK(l.selected()==1); CHECK(l.dirty());
    CHECK(l.accept(0)==Action::None); CHECK(l.view()==View::SteadyPreview);
    l.tap(0,0,0); CHECK(l.view()==View::SteadyPreview);
    l.tap(233,300,0); CHECK(l.view()==View::SteadyPreview);
    l.tap(233,370,0); CHECK(l.view()==View::Grid); CHECK(l.selected()==1);
    l.accept(1); CHECK(l.view()==View::SteadyPreview); l.next(); CHECK(l.view()==View::Grid);
    l.next(); CHECK(l.selected()==0); CHECK(l.accept(1)==Action::OpenPet);
}
void test_disabled_and_invalid_input() {
    Launcher l; l.enter(); l.painted();
    CHECK(l.open(99,1)==Action::None); CHECK(!l.dirty());
    CHECK(l.tap(0,0,1)==Action::None); CHECK(!l.dirty());
    for (size_t i=2;i<kAppCount;++i) {
        CHECK(l.tap(kApps[i].hit.cx,kApps[i].hit.cy,3)==Action::None);
        CHECK(l.view()==View::Grid); CHECK(l.selected()==0); CHECK(l.notice()==static_cast<int>(i));
    }
    l.next(); CHECK(l.notice()==-1); CHECK(l.selected()==1);
}
void test_notice_expiry_and_wrap() {
    Launcher l; l.open(2,0); l.painted(); l.tick(kNoticeMs-1); CHECK(l.notice()==2); CHECK(!l.dirty());
    l.tick(kNoticeMs); CHECK(l.notice()==-1); CHECK(l.dirty());
    l.open(4,UINT32_MAX-999); l.tick(599); CHECK(l.notice()==4); l.tick(600); CHECK(l.notice()==-1);
}
void test_short_and_long_hold() {
    HoldGesture h;
    CHECK(!h.update(0,true,false,true)); CHECK(!h.update(799,false,false,true));
    CHECK(!h.update(1000,true,false,true)); CHECK(!h.update(1800,true,false,true));
    CHECK(h.update(1801,false,false,true)); CHECK(!h.update(1802,false,false,true));
    CHECK(!h.update(2000,true,false,true)); CHECK(h.update(2800,false,false,true));
}
void test_chord_always_wins() {
    HoldGesture h;
    CHECK(!h.update(0,true,false,true)); CHECK(!h.update(1500,true,true,true));
    CHECK(!h.update(3000,true,false,true)); CHECK(!h.update(4000,false,false,true));
    CHECK(!h.update(5000,false,true,true)); CHECK(!h.update(5100,true,true,true));
    CHECK(!h.update(7000,false,true,true)); CHECK(!h.update(7100,false,false,true));
}
void test_blocked_origin_wake_and_wrap() {
    HoldGesture h;
    CHECK(!h.update(0,true,false,false)); CHECK(!h.update(900,true,false,true));
    CHECK(!h.update(1200,false,false,true));
    CHECK(!h.update(2000,true,false,true)); h.cancelUntilRelease();
    CHECK(!h.update(5000,false,false,true));
    CHECK(!h.update(UINT32_MAX-399,true,false,true)); CHECK(h.update(400,false,false,true));
    CHECK(!h.update(1000,true,false,true)); CHECK(!h.update(1500,true,false,false));
    CHECK(!h.update(2500,false,false,true));
}
void test_state_fuzz() {
    Launcher l; uint32_t seed=0x526466, now=UINT32_MAX-10000;
    for (int i=0;i<100000;++i) {
        seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; now+=seed&31;
        switch (seed%6) {
            case 0: l.next(); break;
            case 1: l.accept(now); break;
            case 2: l.tap(static_cast<int>((seed>>8)%700)-100,static_cast<int>((seed>>20)%700)-100,now); break;
            case 3: l.tick(now); break;
            case 4: l.back(); break;
            case 5: l.open((seed>>8)%20,now); break;
        }
        CHECK(l.selected()<kAppCount); CHECK(selectable(l.selected()));
        CHECK(l.notice()>=-1 && l.notice()<static_cast<int>(kAppCount));
    }
}
int main() {
#ifndef LAUNCHER_STANDALONE
    UNITY_BEGIN();
#endif
    RUN_TEST(test_catalog_and_availability);
    RUN_TEST(test_geometry_exhaustive);
    RUN_TEST(test_navigation_and_preview);
    RUN_TEST(test_disabled_and_invalid_input);
    RUN_TEST(test_notice_expiry_and_wrap);
    RUN_TEST(test_short_and_long_hold);
    RUN_TEST(test_chord_always_wins);
    RUN_TEST(test_blocked_origin_wake_and_wrap);
    RUN_TEST(test_state_fuzz);
#ifdef LAUNCHER_STANDALONE
    std::printf("%u assertions passed\n",assertions); return 0;
#else
    return UNITY_END();
#endif
}
