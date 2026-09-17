// Host harness for the exact inserted functions, with existing firmware services mocked.
// This tests integration behavior, not the complete firmware or M5 driver implementation.
#include "app_launcher.h"
#include <cstdio>
#include <cstdlib>
#ifndef TOKENGOCHI_COMPACT_UI
#define TOKENGOCHI_COMPACT_UI 0
#endif
#define TOKENGOCHI_HAS_TOUCH 1
static unsigned checks=0;
#define CHECK(x) do { ++checks; if(!(x)){std::fprintf(stderr,"FAIL integration line %d: %s\n",__LINE__,#x);std::exit(1);} }while(0)
enum class Mode : uint8_t { IDLE,VOICE_IDLE,RECORDING,TRANSCRIBING,SHOWING,HISTORY_LIST,HISTORY_READING,STATS,CONFIRM,SETTINGS,SETTINGS_SAVED,ERROR,LAUNCHER };
static Mode g_mode=Mode::IDLE;
static apps::Launcher g_launcher;
static apps::HoldGesture g_launcherHold;
static bool g_launcherWakeRelease=false, g_suppressButtonsUntilRelease=false;
static uint32_t g_suppressButtonsUntilMs=0,g_greetingUntilMs=0,clockMs=0;
static bool g_wifiReconnectRequested=false,awake=true,mic=false;
static unsigned draws=0,quietCalls=0;
struct Settings {bool lowBatteryWarning=true;} g_settings;
namespace battery_status { enum class WarningState:uint8_t{Unknown,None,Low,Critical}; inline bool isWarning(WarningState w){return w==WarningState::Low||w==WarningState::Critical;} }
static battery_status::WarningState g_batteryWarning=battery_status::WarningState::Unknown;
struct Button {bool down=false,pressed=false,clicked=false;bool isPressed(){return down;}bool wasPressed(){return pressed;}bool wasClicked(){return clicked;}};
struct TouchMock {int count=0;bool enabled=true,clicked=false;int16_t x=0,y=0;bool isEnabled(){return enabled;}int getCount(){return count;}};
struct Hardware {Button BtnA,BtnB;TouchMock Touch;} M5;
struct SerialMock {template<class...Args>void printf(const char*,Args...){} } Serial;
namespace audio { bool micActive(){return mic;} }
namespace ui {void pageReset(){} }
namespace apps {void quietLauncherPeripherals(){CHECK(!mic);++quietCalls;}void drawLauncher(const Launcher&,const char*){++draws;} }
static uint32_t millis(){return clockMs;}
static bool displayVisible(){return awake;}
static void noteInteraction(const char*){awake=true;g_wifiReconnectRequested=true;}
static bool buttonsSuppressed(){if(g_suppressButtonsUntilRelease){if(!M5.BtnA.isPressed()&&!M5.BtnB.isPressed()){g_suppressButtonsUntilRelease=false;g_suppressButtonsUntilMs=millis()+80;}return true;}return millis()<g_suppressButtonsUntilMs;}
static bool btnAClicked(){if(buttonsSuppressed())return false;if(M5.BtnA.wasClicked()){noteInteraction("A");return true;}return false;}
static bool btnBClicked(){if(buttonsSuppressed())return false;if(M5.BtnB.wasClicked()){noteInteraction("B");return true;}return false;}
static bool touchClicked(int16_t*x,int16_t*y){if(!M5.Touch.clicked)return false;*x=M5.Touch.x;*y=M5.Touch.y;noteInteraction("touch");return true;}
static void returnToPet(const char*){if(g_mode==Mode::LAUNCHER){g_launcher.exit();apps::quietLauncherPeripherals();noteInteraction("return");}g_mode=Mode::IDLE;}
#include "app_launcher_runtime.inc"
static void reset(){g_mode=Mode::IDLE;g_launcher=apps::Launcher();g_launcherHold=apps::HoldGesture();g_launcherWakeRelease=false;g_suppressButtonsUntilRelease=false;g_suppressButtonsUntilMs=0;clockMs=100;g_wifiReconnectRequested=false;awake=true;mic=false;M5=Hardware();draws=quietCalls=0;}
static bool input(uint32_t t,bool a=false,bool b=false,bool aClick=false,bool bClick=false,int touches=0,bool tap=false,int16_t x=0,int16_t y=0){clockMs=t;M5.BtnA.pressed=a&&!M5.BtnA.down;M5.BtnB.pressed=b&&!M5.BtnB.down;M5.BtnA.down=a;M5.BtnB.down=b;M5.BtnA.clicked=aClick;M5.BtnB.clicked=bClick;M5.Touch.count=touches;M5.Touch.clicked=tap;M5.Touch.x=x;M5.Touch.y=y;return handleLauncherNavigation();}
static void settled(){input(clockMs+100);input(clockMs+100);input(clockMs+100);}
int main(){
    reset();
#if TOKENGOCHI_COMPACT_UI
    CHECK(!launcherEntryAllowed());enterLauncher("compact");CHECK(g_mode==Mode::IDLE);CHECK(draws==0);
    CHECK(!input(100,true));CHECK(!input(1500));settled();
    // Compile the update path too; it remains unreachable through compact entry.
    g_mode=Mode::LAUNCHER; updateLauncher();
#else
    CHECK(!input(100,true));CHECK(!input(500,true));CHECK(input(1000));
    CHECK(g_mode==Mode::LAUNCHER);CHECK(draws==1);CHECK(!g_wifiReconnectRequested);
    settled();
    CHECK(!input(clockMs+100,false,false,false,true));updateLauncher();CHECK(g_launcher.selected()==1);
    CHECK(!input(clockMs+100,false,false,true));updateLauncher();CHECK(g_launcher.view()==apps::View::SteadyPreview);
    CHECK(!input(clockMs+100,false,false,false,false,1,true,233,370));updateLauncher();CHECK(g_launcher.view()==apps::View::Grid);
    CHECK(!input(clockMs+100,false,false,false,false,1,true,363,300));updateLauncher();CHECK(g_launcher.notice()==2);CHECK(g_mode==Mode::LAUNCHER);
    input(clockMs+100,false,false,false,false,1,true,159,155);updateLauncher();CHECK(g_mode==Mode::IDLE);CHECK(g_wifiReconnectRequested);
    // Wake touch is consumed through its release, not interpreted as an app launch.
    reset();enterLauncher("test");settled();awake=false;
    CHECK(input(clockMs+100,false,false,false,false,1,false,159,155));CHECK(awake);
    CHECK(input(clockMs+100,false,false,false,false,1,true,159,155));CHECK(g_mode==Mode::LAUNCHER);
    settled();CHECK(g_launcher.selected()==0);
    // The single-A action must not run after any observed chord.
    reset();CHECK(!input(100,true));CHECK(!input(1100,true,true));CHECK(!input(2400,true,true));
    g_mode=Mode::SETTINGS;CHECK(!input(2500));CHECK(g_mode==Mode::SETTINGS);CHECK(draws==0);
    // Recording and destructive confirmation remain outside the new shortcut.
    const Mode guarded[]={Mode::RECORDING,Mode::TRANSCRIBING,Mode::CONFIRM,Mode::SHOWING};
    for(Mode m:guarded){
        reset();g_mode=m;CHECK(!input(100,true));CHECK(!input(2000));CHECK(g_mode==m);CHECK(quietCalls==0);
    }
    reset();g_mode=Mode::VOICE_IDLE;mic=true;CHECK(!input(100,true));CHECK(!input(2000));CHECK(g_mode==Mode::VOICE_IDLE);CHECK(quietCalls==0);
    // Short A in Pet is not intercepted, leaving the existing history route intact.
    reset();CHECK(!input(100,true));CHECK(!input(300,false,false,true));CHECK(btnAClicked());
#endif
    std::printf("PASS inserted-function integration harness (%s): %u checks\n",TOKENGOCHI_COMPACT_UI?"compact entry disabled":"round target",checks);
}
