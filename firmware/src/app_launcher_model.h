#pragma once
// Platform-independent launcher state. No heap, hardware, network, or timers owned here.
#include "app_launcher_manifest.h"

namespace apps {
static constexpr uint32_t kHoldMs = 800;
static constexpr uint32_t kNoticeMs = 1600;
enum class View : uint8_t { Grid, SteadyPreview };
enum class Action : uint8_t { None, OpenPet };

inline bool inDisc(int x, int y) {
    if (x < 0 || y < 0 || x >= 466 || y >= 466) return false;
    const int dx = x - 233, dy = y - 233;
    return dx*dx + dy*dy <= 233*233;
}
inline bool contains(const Ellipse& e, int x, int y) {
    if (!inDisc(x,y)) return false;
    const int64_t dx = x-e.cx, dy = y-e.cy;
    const int64_t rx2 = e.rx*e.rx, ry2 = e.ry*e.ry;
    return dx*dx*ry2 + dy*dy*rx2 <= rx2*ry2;
}
inline int hitTest(int x, int y) {
    for (size_t i=0; i<kAppCount; ++i) if (contains(kApps[i].hit,x,y)) return static_cast<int>(i);
    return -1;
}
inline bool selectable(size_t i) { return i<kAppCount && kApps[i].availability!=Availability::Planned; }

// Decide the single-A hold on release, so an A+B chord at ANY point wins.
// Disallowed modes disarm until all buttons are released. Unsigned subtraction
// intentionally handles millis() wrapping. Call before the existing chord handler.
class HoldGesture {
    bool tracking_ = false;
    bool chord_ = false;
    bool blocked_ = false;
    uint32_t started_ = 0;
public:
    void cancelUntilRelease() { tracking_=false; blocked_=true; }
    bool update(uint32_t now, bool aDown, bool bDown, bool allowed) {
        if (!allowed) { tracking_=false; blocked_=aDown||bDown; return false; }
        if (blocked_) { if (!aDown && !bDown) blocked_=false; return false; }
        if (aDown) {
            if (!tracking_) { tracking_=true; started_=now; chord_=bDown; }
            if (bDown) chord_=true;
            return false;
        }
        if (!tracking_) return false;
        tracking_=false;
        return !chord_ && !bDown && static_cast<uint32_t>(now-started_)>=kHoldMs;
    }
};

class Launcher {
    View view_ = View::Grid;
    uint8_t selected_ = 0;
    int8_t notice_ = -1;
    uint32_t noticeAt_ = 0;
    bool dirty_ = true;
public:
    void enter() { view_=View::Grid; selected_=0; notice_=-1; dirty_=true; }
    void exit() { notice_=-1; }
    View view() const { return view_; }
    uint8_t selected() const { return selected_; }
    int notice() const { return notice_; }
    bool dirty() const { return dirty_; }
    void painted() { dirty_=false; }
    void invalidate() { dirty_=true; }
    void tick(uint32_t now) {
        if (notice_>=0 && static_cast<uint32_t>(now-noticeAt_)>=kNoticeMs) { notice_=-1; dirty_=true; }
    }
    void next() {
        if (view_!=View::Grid) { back(); return; }
        for (size_t n=0; n<kAppCount; ++n) {
            selected_=static_cast<uint8_t>((selected_+1)%kAppCount);
            if (selectable(selected_)) break;
        }
        notice_=-1; dirty_=true;
    }
    Action open(size_t index, uint32_t now) {
        if (index>=kAppCount) return Action::None;
        if (!selectable(index)) { notice_=static_cast<int8_t>(index); noticeAt_=now; dirty_=true; return Action::None; }
        selected_=static_cast<uint8_t>(index); notice_=-1; dirty_=true;
        if (kApps[index].id==Id::Pet) return Action::OpenPet;
        // Explicit scaffold, not a playable Steady implementation.
        if (kApps[index].id==Id::Steady) view_=View::SteadyPreview;
        return Action::None;
    }
    Action accept(uint32_t now) {
        if (view_!=View::Grid) { back(); return Action::None; }
        return open(selected_,now);
    }
    Action tap(int x, int y, uint32_t now) {
        if (!inDisc(x,y)) return Action::None;
        if (view_!=View::Grid) {
            // Only the explicit back target responds. No invisible full-screen button.
            if (x>=143 && x<323 && y>=345 && y<401) back();
            return Action::None;
        }
        const int index=hitTest(x,y);
        return index<0 ? Action::None : open(static_cast<size_t>(index),now);
    }
    void back() { view_=View::Grid; notice_=-1; dirty_=true; }
};
} // namespace apps
