/* libScePad on SDL3 gamepads, with a keyboard fallback. SDL events are pumped
 * by the window thread (gpu/shim/window.cpp); here state is only sampled.
 *
 * Default keyboard layout (GOW3_KEY_MAP changes the buttons, see apply_button_map):
 *   WASD left stick, arrow keys right stick, Space Cross, LShift Circle,
 *   E Square, Q Triangle, 1 L1, 3 R1, R L2, F R2, Z L3, C R3,
 *   Enter Options, Tab left touchpad (photo mode), Backspace right touchpad (pause menu),
 *   IJKL d-pad (I up, K down, J left, L right).
 *
 * GOW3_INPUT_CONFIG (or UC_INPUT_CONFIG) names an input.ini, written by the launcher's Controls
 * page: keyboard, mouse and controller bindings and mouse look (see load_input). Its layout, the
 * action table and the mouse as a right stick are ported from SuperMedo's uncharted_pc (GPL). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/gow3gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
    /* Host-only sources, never shown to the game: a click on one side of the touchpad from a
     * button or key without a touch surface (play_click turns them into a touch and a click). */
    CLICK_LEFT=0x40000000, CLICK_RIGHT=0x80000000,
};
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened, sdl_ready;
static SDL_Gamepad *gamepad;
static size_t reads;
static uint8_t connected_count;

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}

/* A click without a touch surface (Back/View, Tab, Backspace, the pad file) is played like a
 * finger: the touch is reported CLICK_LEAD_US before the button, the button stays at least
 * CLICK_HOLD_US, and the touch lingers CLICK_TAIL_US after the release. God of War III opens
 * photo mode on the release of a left click with the finger still down, and ignores a touch
 * that comes and goes with the button. The side is chosen when the click starts and kept: the
 * right source with L1 held is a left click (photo mode on pads without a touchpad). */
enum { CLICK_LEAD_US=50000, CLICK_HOLD_US=50000, CLICK_TAIL_US=100000 };
static struct { int side; uint64_t pressed, released; } click={-1,0,0};
static void play_click(PadData *d) {
    const uint64_t now=d->timestamp;
    uint32_t sources=d->buttons & (CLICK_LEFT|CLICK_RIGHT);
    d->buttons&=~(CLICK_LEFT|CLICK_RIGHT);
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) { /* a replay, or a pad without fingers */
        d->buttons&=~BTN_TOUCHPAD;
        sources|=CLICK_LEFT;
    }
    const int side=(sources & CLICK_LEFT) ? 0 : (sources & CLICK_RIGHT) ? !(d->buttons & BTN_L1) : -1;
    if (side>=0 && (click.side<0 || click.released)) { click.side=side; click.pressed=now; click.released=0; }
    else if (side<0 && click.side>=0 && !click.released) click.released=now;
    if (click.side<0) return;
    const uint64_t down=click.pressed+CLICK_LEAD_US;
    uint64_t up=down+CLICK_HOLD_US;
    if (!click.released) up=UINT64_MAX;
    else if (click.released>up) up=click.released;
    if (now>=up && now-up>=CLICK_TAIL_US) { click.side=-1; return; }
    if (d->touch_count<2) {
        d->touches[d->touch_count]=(PadTouch){.x=click.side ? 1440 : 480,.y=471,.id=d->touch_count};
        ++d->touch_count;
    }
    if (now>=down && now<up) d->buttons|=BTN_TOUCHPAD;
}

/* Opens the first gamepad SDL knows about; called under lock. */
static SDL_Gamepad *current_gamepad(void) {
    if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
    if (sdl_ready<0) return NULL;
    if (gamepad && !SDL_GamepadConnected(gamepad)) { SDL_CloseGamepad(gamepad); gamepad=NULL; }
    if (!gamepad) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) {
            gamepad=SDL_OpenGamepad(ids[0]);
            if (gamepad) { ++connected_count; printf("Runtime: gamepad connected: %s\n",SDL_GetGamepadName(gamepad)); }
        }
        SDL_free(ids);
    }
    return gamepad;
}
/* Button sources: a host button or key and the PS4 button it presses. The defaults below can be
 * changed per PS4 button with GOW3_PAD_MAP (gamepad) and GOW3_KEY_MAP (keyboard), e.g.
 * GOW3_KEY_MAP="cross=Q,square=Left Shift" or GOW3_PAD_MAP="cross=b,circle=a": comma-separated
 * <ps4 button>=<SDL key or gamepad button name>. A mapped PS4 button loses its default sources.
 * touchpad_left (alias touchpad) and touchpad_right are the two sides of the touchpad click:
 * photo mode and the pause menu. Back/View (Share on a DualSense) is the right click, and with
 * L1 held the left one. Sticks, triggers, the real touchpad click and the overlay shortcuts are
 * not remapped. */
static const struct { const char *name; uint32_t ps; } ps_names[]={
    {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
    {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
    {"options",BTN_OPTIONS}, {"touchpad",CLICK_LEFT}, {"touchpad_left",CLICK_LEFT}, {"touchpad_right",CLICK_RIGHT},
    {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
};
/* Sources in key_map besides SDL scancodes: a mouse button (bit as in gow3gpu_mouse_sample) or a
 * wheel step; in pad_map besides SDL gamepad buttons: a trigger pressed past a third. */
enum { SRC_MOUSE=0x1000, SRC_WHEEL_UP=0x2000, SRC_WHEEL_DOWN=0x2001,
       SRC_TRIGGER_LEFT=0x3000, SRC_TRIGGER_RIGHT=0x3001 };
typedef struct { int source; uint32_t ps; } ButtonSource;
typedef struct { ButtonSource entries[48]; size_t count; } ButtonMap;
static ButtonMap pad_map={{
    {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
    {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,CLICK_RIGHT},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
},15};
static ButtonMap key_map={{
    {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_LSHIFT,BTN_CIRCLE}, {SDL_SCANCODE_E,BTN_SQUARE},
    {SDL_SCANCODE_Q,BTN_TRIANGLE}, {SDL_SCANCODE_1,BTN_L1}, {SDL_SCANCODE_3,BTN_R1},
    {SDL_SCANCODE_R,BTN_L2}, {SDL_SCANCODE_F,BTN_R2}, {SDL_SCANCODE_Z,BTN_L3}, {SDL_SCANCODE_C,BTN_R3},
    {SDL_SCANCODE_RETURN,BTN_OPTIONS}, {SDL_SCANCODE_TAB,CLICK_LEFT}, {SDL_SCANCODE_BACKSPACE,CLICK_RIGHT},
    {SDL_SCANCODE_I,BTN_UP}, {SDL_SCANCODE_K,BTN_DOWN}, {SDL_SCANCODE_J,BTN_LEFT}, {SDL_SCANCODE_L,BTN_RIGHT},
},17};

static char *trim(char *text) {
    while (*text==' ' || *text=='\t') ++text;
    char *end=text+strlen(text);
    while (end>text && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r' || end[-1]=='\n')) *--end=0;
    return text;
}

/* Keys (or mouse buttons) that push a stick fully: ls up, down, left, right, rs up, down, left,
 * right; two each. */
enum { STICK_LS_UP, STICK_LS_DOWN, STICK_LS_LEFT, STICK_LS_RIGHT, STICK_RS_UP, STICK_RS_DOWN, STICK_RS_LEFT, STICK_RS_RIGHT, STICK_KEYS };
static const char *const stick_names[STICK_KEYS]={"ls_up","ls_down","ls_left","ls_right","rs_up","rs_down","rs_left","rs_right"};
static int stick_keys[STICK_KEYS][2]={
    {SDL_SCANCODE_W,-1}, {SDL_SCANCODE_S,-1}, {SDL_SCANCODE_A,-1}, {SDL_SCANCODE_D,-1},
    {SDL_SCANCODE_UP,-1}, {SDL_SCANCODE_DOWN,-1}, {SDL_SCANCODE_LEFT,-1}, {SDL_SCANCODE_RIGHT,-1},
};
/* Mouse settings from input.ini (mouse look stays off without one). */
static struct { int look, invert_y, pad_invert_y; float sensitivity, aim_sensitivity; } mouse_cfg={0,0,0,1.0f,1.0f};

/* An SDL key name, "Mouse Left/Right/Middle/4/5" or "Wheel Up/Down"; -1 when unknown. */
static int parse_key_source(const char *name) {
    static const struct { const char *name; int source; } mouse[]={
        {"Mouse Left",SRC_MOUSE|1}, {"Mouse Right",SRC_MOUSE|2}, {"Mouse Middle",SRC_MOUSE|4},
        {"Mouse 4",SRC_MOUSE|8}, {"Mouse 5",SRC_MOUSE|16}, {"Wheel Up",SRC_WHEEL_UP}, {"Wheel Down",SRC_WHEEL_DOWN},
    };
    if (!*name) return -1;
    for (size_t i=0;i<sizeof(mouse)/sizeof(*mouse);++i) if (!SDL_strcasecmp(name,mouse[i].name)) return mouse[i].source;
    const SDL_Scancode code=SDL_GetScancodeFromName(name);
    return code==SDL_SCANCODE_UNKNOWN ? -1 : (int)code;
}
/* An SDL gamepad button name, "lefttrigger" or "righttrigger"; -1 when unknown. */
static int parse_pad_source(const char *name) {
    if (!SDL_strcasecmp(name,"lefttrigger")) return SRC_TRIGGER_LEFT;
    if (!SDL_strcasecmp(name,"righttrigger")) return SRC_TRIGGER_RIGHT;
    const SDL_GamepadButton button=SDL_GetGamepadButtonFromString(name);
    return button==SDL_GAMEPAD_BUTTON_INVALID ? -1 : (int)button;
}

/* Applies a GOW3_PAD_MAP/GOW3_KEY_MAP value; returns how many entries were used. Bad entries are
 * logged and skipped, so their PS4 button keeps its default. */
static int apply_button_map(ButtonMap *map, const char *spec, bool keyboard) {
    if (!spec || !*spec) return 0;
    char copy[1024];
    snprintf(copy,sizeof(copy),"%s",spec);
    int used=0;
    char *entry=copy;
    while (entry) {
        char *next=strchr(entry,',');
        if (next) *next++=0;
        char *eq=strchr(entry,'=');
        if (!eq) {
            if (*trim(entry)) printf("Runtime: pad map: ignored '%s' (expected button=name)\n",trim(entry));
            entry=next;
            continue;
        }
        *eq=0;
        const char *button=trim(entry), *name=trim(eq+1);
        entry=next;
        uint32_t ps=0;
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!SDL_strcasecmp(button,ps_names[i].name)) ps=ps_names[i].ps;
        const int source=!*name ? -1 : keyboard ? parse_key_source(name) : parse_pad_source(name);
        if (!ps || source<0) {
            printf("Runtime: pad map: ignored '%s=%s'\n",button,name);
            continue;
        }
        size_t kept=0;
        for (size_t i=0;i<map->count;++i) if (map->entries[i].ps!=ps) map->entries[kept++]=map->entries[i];
        map->count=kept;
        if (map->count<sizeof(map->entries)/sizeof(*map->entries)) map->entries[map->count++]=(ButtonSource){source,ps};
        ++used;
    }
    return used;
}

/* Replaces the sources of one PS4 button with up to two ("A|B"); an empty value unbinds it. */
static void set_sources(ButtonMap *map, uint32_t ps, const char *value, bool keyboard) {
    char text[128];
    snprintf(text,sizeof(text),"%s",value);
    size_t kept=0;
    for (size_t i=0;i<map->count;++i) if (map->entries[i].ps!=ps) map->entries[kept++]=map->entries[i];
    map->count=kept;
    char *second=strchr(text,'|');
    if (second) *second++=0;
    const char *names[2]={trim(text),second ? trim(second) : ""};
    for (int i=0;i<2;++i) {
        if (!SDL_strcasecmp(names[i],"none")) continue; /* the launcher's "not assigned" */
        const int source=keyboard ? parse_key_source(names[i]) : parse_pad_source(names[i]);
        if (source>=0 && map->count<sizeof(map->entries)/sizeof(*map->entries)) map->entries[map->count++]=(ButtonSource){source,ps};
        else if (*names[i]) printf("Runtime: input.ini: unknown %s '%s'\n",keyboard ? "key" : "pad button",names[i]);
    }
}
/* The actions the launcher's Controls window and the port menu edit, with their default keys and
 * controller buttons (pad NULL: a stick direction, keyboard only). Keep in step with ini_keys
 * below and the launcher's BUTTON_ACTIONS / STICK_ACTIONS. */
static const RuntimeInputAction input_actions[]={
    {"cross","Cross","Space","a"}, {"circle","Circle","Left Ctrl|Q","b"}, {"square","Square","F","x"},
    {"triangle","Triangle","E","y"}, {"l1","L1","G","leftshoulder"}, {"r1","R1","R","rightshoulder"},
    {"l2","L2 (aim)","Mouse Right","lefttrigger"}, {"r2","R2 (fire)","Mouse Left","righttrigger"},
    {"l3","L3","V","leftstick"}, {"r3","R3","Mouse Middle","rightstick"}, {"options","Options","Escape|Return","start"},
    {"touchpad_left","Touchpad, left side","Tab","none"}, {"touchpad_right","Touchpad, right side","Backspace","back"},
    {"up","D-pad up","4","dpup"}, {"down","D-pad down","3","dpdown"}, {"left","D-pad left","1|Wheel Up","dpleft"},
    {"right","D-pad right","2|Wheel Down","dpright"},
    {"ls_up","Move forward","W",NULL}, {"ls_down","Move back","S",NULL}, {"ls_left","Move left","A",NULL},
    {"ls_right","Move right","D",NULL}, {"rs_up","Look up","I",NULL}, {"rs_down","Look down","K",NULL},
    {"rs_left","Look left","J",NULL}, {"rs_right","Look right","L",NULL},
};
const RuntimeInputAction *runtime_input_actions(size_t *count) {
    *count=sizeof(input_actions)/sizeof(*input_actions);
    return input_actions;
}
/* input.ini as last read, for the port menu: its key=value lines in file order. */
static pthread_mutex_t input_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { char key[48], value[96]; } input_lines[96];
static size_t input_count;
static volatile int input_reload; /* the port menu changed input.ini: read it again */

/* input.ini (GOW3_INPUT_CONFIG): with it the keyboard starts from the launcher's layout (WASD move,
 * mouse left R2, mouse right L2, Space Cross, Left Ctrl/Q Circle, F Square, E Triangle, G L1, R R1,
 * V L3, mouse middle R3, Escape/Enter Options, Tab and Backspace the touchpad sides, 4 3 1 2 and the
 * wheel d-pad up/down/left/right, IJKL right stick), then its lines change it:
 *   key.<action>=<key or mouse>|<second>  pad.<action>=<SDL gamepad button or lefttrigger/righttrigger>
 *   mouse_look=0/1  mouse_sensitivity=<x>  mouse_aim_sensitivity=<multiplier while L2 is held>
 *   mouse_invert_y=0/1  pad_invert_y=0/1
 * Actions: the ps_names buttons and ls_up ls_down ls_left ls_right rs_up rs_down rs_left rs_right. */
static void load_input(void) {
    const char *path=getenv("GOW3_INPUT_CONFIG");
    FILE *f=path && *path ? fopen(path,"r") : NULL;
    if (!f) {
        if (path && *path) printf("Runtime: input.ini not found (%s), default controls\n",path);
        return;
    }
    static const ButtonMap ini_keys={{
        {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_LCTRL,BTN_CIRCLE}, {SDL_SCANCODE_Q,BTN_CIRCLE},
        {SDL_SCANCODE_F,BTN_SQUARE}, {SDL_SCANCODE_E,BTN_TRIANGLE}, {SDL_SCANCODE_G,BTN_L1}, {SDL_SCANCODE_R,BTN_R1},
        {SRC_MOUSE|2,BTN_L2}, {SRC_MOUSE|1,BTN_R2}, {SDL_SCANCODE_V,BTN_L3}, {SRC_MOUSE|4,BTN_R3},
        {SDL_SCANCODE_ESCAPE,BTN_OPTIONS}, {SDL_SCANCODE_RETURN,BTN_OPTIONS},
        {SDL_SCANCODE_TAB,CLICK_LEFT}, {SDL_SCANCODE_BACKSPACE,CLICK_RIGHT},
        {SDL_SCANCODE_4,BTN_UP}, {SDL_SCANCODE_3,BTN_DOWN},
        {SDL_SCANCODE_1,BTN_LEFT}, {SRC_WHEEL_UP,BTN_LEFT}, {SDL_SCANCODE_2,BTN_RIGHT}, {SRC_WHEEL_DOWN,BTN_RIGHT},
    },21};
    static const int ini_sticks[STICK_KEYS]={SDL_SCANCODE_W,SDL_SCANCODE_S,SDL_SCANCODE_A,SDL_SCANCODE_D,
                                             SDL_SCANCODE_I,SDL_SCANCODE_K,SDL_SCANCODE_J,SDL_SCANCODE_L};
    key_map=ini_keys;
    for (int i=0;i<STICK_KEYS;++i) { stick_keys[i][0]=ini_sticks[i]; stick_keys[i][1]=-1; }
    mouse_cfg.look=mouse_cfg.invert_y=mouse_cfg.pad_invert_y=0;
    mouse_cfg.sensitivity=mouse_cfg.aim_sensitivity=1.0f;
    pthread_mutex_lock(&input_lock);
    input_count=0;
    pthread_mutex_unlock(&input_lock);
    char line[256];
    while (fgets(line,sizeof(line),f)) {
        char *eq=strchr(line,'=');
        if (*trim(line)=='#' || *trim(line)==';' || !eq) continue;
        *eq=0;
        const char *key=trim(line), *value=trim(eq+1);
        pthread_mutex_lock(&input_lock);
        if (input_count<sizeof(input_lines)/sizeof(*input_lines)) {
            snprintf(input_lines[input_count].key,sizeof(input_lines[0].key),"%s",key);
            snprintf(input_lines[input_count].value,sizeof(input_lines[0].value),"%s",value);
            ++input_count;
        }
        pthread_mutex_unlock(&input_lock);
        if (!strcmp(key,"mouse_look")) mouse_cfg.look=atoi(value)!=0;
        else if (!strcmp(key,"mouse_sensitivity")) mouse_cfg.sensitivity=(float)atof(value);
        else if (!strcmp(key,"mouse_aim_sensitivity")) mouse_cfg.aim_sensitivity=(float)atof(value);
        else if (!strcmp(key,"mouse_invert_y")) mouse_cfg.invert_y=atoi(value)!=0;
        else if (!strcmp(key,"pad_invert_y")) mouse_cfg.pad_invert_y=atoi(value)!=0;
        else if (!strncmp(key,"key.",4) || !strncmp(key,"pad.",4)) {
            const bool keyboard=key[0]=='k';
            const char *action=key+4;
            bool known=false;
            for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) {
                if (SDL_strcasecmp(action,ps_names[i].name)) continue;
                set_sources(keyboard ? &key_map : &pad_map,ps_names[i].ps,value,keyboard);
                known=true;
                break;
            }
            for (int i=0;i<STICK_KEYS && !known;++i) {
                if (SDL_strcasecmp(action,stick_names[i])) continue;
                known=true;
                if (!keyboard) break; /* the controller's sticks are its own */
                char text[128];
                snprintf(text,sizeof(text),"%s",value);
                char *second=strchr(text,'|');
                if (second) *second++=0;
                stick_keys[i][0]=parse_key_source(trim(text));
                stick_keys[i][1]=second ? parse_key_source(trim(second)) : -1;
            }
            if (!known) printf("Runtime: input.ini: unknown action '%s'\n",key);
        }
    }
    fclose(f);
    if (mouse_cfg.sensitivity<=0.0f) mouse_cfg.sensitivity=1.0f;
    if (mouse_cfg.aim_sensitivity<=0.0f) mouse_cfg.aim_sensitivity=1.0f;
    printf("Runtime: controls from %s (mouse look %s, sensitivity %.2f, aiming x%.2f)\n",path,
           mouse_cfg.look ? "on" : "off",mouse_cfg.sensitivity,mouse_cfg.aim_sensitivity);
}

static void load_button_maps(void) {
    static bool loaded;
    static ButtonMap pad_defaults;
    if (loaded && !input_reload) return;
    if (!loaded) pad_defaults=pad_map;
    else pad_map=pad_defaults; /* input.ini again, after a change in the port menu */
    loaded=true;
    input_reload=0;
    load_input();
    gow3gpu_mouse_configure(mouse_cfg.look);
    const int pad=apply_button_map(&pad_map,getenv("GOW3_PAD_MAP"),false);
    const int keys=apply_button_map(&key_map,getenv("GOW3_KEY_MAP"),true);
    if (pad || keys) printf("Runtime: pad map: %d gamepad and %d keyboard buttons remapped\n",pad,keys);
}

static void key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}

/* Degrees of camera turn per mouse pixel at sensitivity 1. */
#define MOUSE_DEGREES_PER_PIXEL 0.08f
static float mouse_vx, mouse_vy;
static uint64_t mouse_last_us;
static uint64_t wheel_until_us[2]; /* wheel up, wheel down: held briefly for the game to see */
/* Mouse motion as a right stick: a turn rate (degrees/s) as a stick byte, through the inverse of
 * the U1 follow camera's response measured below its speed ramp (SuperMedo): about 240 x^2.5
 * degrees/s (x = 0.25: 7, 0.5: 37). */
static uint8_t mouse_axis(float rate) {
    const float r=rate<0 ? -rate : rate;
    if (r<0.5f) return 128;
    float out=SDL_powf(r/240.0f,0.4f);
    if (out>1.0f) out=1.0f;
    int x=(int)(128.0f+(rate<0 ? -out : out)*127.5f);
    return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x);
}
/* A key_map source held now: a key, a mouse button or a recent wheel step. */
static bool source_down(int source, const bool *k, uint32_t mouse, uint64_t now) {
    if (source<0) return false;
    if (source==SRC_WHEEL_UP || source==SRC_WHEEL_DOWN) return now<wheel_until_us[source-SRC_WHEEL_UP];
    if ((source & 0xf000)==SRC_MOUSE) return (mouse & (uint32_t)(source & 0xff))!=0;
    return k && source<SDL_SCANCODE_COUNT && k[source];
}
static bool stick_down(int stick, const bool *k, uint32_t mouse, uint64_t now) {
    return source_down(stick_keys[stick][0],k,mouse,now) || source_down(stick_keys[stick][1],k,mouse,now);
}
/* ---- mouse camera (from SuperMedo's uncharted_pc, GPL) -------------------------------------
 * The games turn the camera from the right stick through their own curve, a speed ramp and a top
 * speed (U1: about 240 degrees/s, 45 while aiming), which no stick position can make feel like a
 * mouse. Where the camera's state is known, the mouse turns it directly instead: a callback at the
 * start of the follow camera's per-frame update (installed by the loader when its code is found,
 * runtime_guest_callbacks) rotates its direction by the mouse motion since the last frame.
 * Elsewhere the mouse still moves the right stick. ucndc: the camera's fields are checked before
 * each write (a unit horizontal direction, an arc in [0,1]); 30 bad frames in a row turn the
 * callback off until the game restarts. */
static pthread_mutex_t camera_lock=PTHREAD_MUTEX_INITIALIZER;
/* Mouse motion (pixels) not yet given to the follow camera [0] and to the joypad state [1]: each
 * gets all of it (the follow camera does not read the joypad's right stick values, the other
 * cameras do not use the follow camera's direction). */
static float camera_dx[5], camera_dy[5]; /* [2]: the aiming camera (U3), [3]: the cover camera (U3 test), [4]: aiming on a climb (U3) */
static volatile uint64_t camera_hook_us; /* last follow camera callback: it turns with the mouse */
static volatile uint64_t sticks_hook_us; /* last joypad callback: the mouse is a right stick value there */
static int mouse_debug(void) {
    static int on=-1;
    if (on<0) on=getenv("GOW3_MOUSE_DEBUG")!=NULL;
    return on;
}
static void camera_take(int queue, float *dx, float *dy) {
    pthread_mutex_lock(&camera_lock);
    *dx=camera_dx[queue]; *dy=camera_dy[queue];
    camera_dx[queue]=camera_dy[queue]=0;
    pthread_mutex_unlock(&camera_lock);
}
static float wrap_degrees(float a) {
    while (a>180.0f) a-=360.0f;
    while (a<-180.0f) a+=360.0f;
    return a;
}
static float heading_of(const float *dir) { return SDL_atan2f(-dir[0],-dir[2])*57.29578f; }
/* The follow camera's update runs once a frame with the camera object: its horizontal direction
 * (x, y, z) at +0x1310 (U2 +0x9ec0, U3 +0x78c0) and the vertical arc position at +0x12bc (U2
 * +0x9e60, U3 +0x7874: the right stick up lowers it). The camera keeps the turns of its direction
 * while the right stick is held: with the stick at rest the game sets the direction itself every
 * few frames and most turns were lost. So while the mouse turns the direction, the right stick is
 * held a little (0.3) the same way (follow_push), and the mouse moves a wanted heading: each frame
 * the direction turns by the mouse's motion plus what it is still short of it. While the mouse
 * rests, the wanted heading follows the direction (the game's own camera moves stay free). Should
 * the camera keep no turns at all, the mouse goes back to the right stick, with a small turn each
 * frame to notice when turns are kept again. */
static volatile int follow_direct=1;
static float follow_kept=1.0f;            /* average fraction of a turn still there the next frame */
static float follow_before, follow_after; /* heading before and after the last turn */
static int follow_pending;
/* GOW3_MOUSE_U1_REWRITE_LOG=1 (diagnostic, off; Opus's study of the UC1 drops to the stick): at the
 * end of the U1 follow camera update (0xaf7c2) the game can set the direction again from where the
 * camera really is (normalized target - position), and marks it in the byte +0x1410 (0 on the
 * other paths). The callback runs at the start of the next update, so the byte says whether the
 * frame that lost our turn rewrote the direction. Counts lost turns (kept < 0.05) with the byte at
 * 1 and at 0, printed at each switch to and from the stick. -1: not U1, or off. */
static int follow_rewrite=-1;
static unsigned follow_lost_rewrite, follow_lost_other, follow_kept_rewrite, follow_kept_other;
/* Opus's second hypothesis: the update at 0xaede0 serves several cameras, and the measure keeps no
 * track of which one it turned. A ring of the last 16 measured turns (camera object, raw kept,
 * +0x1410, callbacks in that frame, camera changed since the turn before), printed at each drop
 * to the stick; and a line when the camera object changes (at most one a second). */
struct u1_turn { uint64_t us, camera; float kept; int rewrite, calls, changed; };
static struct u1_turn u1_turns[16];
static unsigned u1_turn_count;
static uint64_t u1_camera, u1_turned_camera, u1_last_call_us, u1_changed_log_us;
static int u1_calls; /* callbacks less than 8 ms apart: the same frame */
static int u1_rewrite_log(void) {
    static int on=-1;
    if (on<0) { const char *v=getenv("GOW3_MOUSE_U1_REWRITE_LOG"); on=v && v[0]=='1'; if (on) printf("Mouse: logging the U1 follow camera's direction rewrites (+0x1410) at each drop to the stick\n"); }
    return on;
}
static volatile int aim_held;             /* L2 is held: the aiming camera (also while it blends in) */
/* The mouse sensitivity now: times mouse_aim_sensitivity while aiming. */
static float mouse_sensitivity(void) { return mouse_cfg.sensitivity*(aim_held ? mouse_cfg.aim_sensitivity : 1.0f); }
static float follow_target;               /* wanted heading (degrees) */
static uint64_t follow_moved_us;          /* last frame the mouse moved */
static volatile float follow_correction;  /* degrees the direction is short of the wanted heading */
static volatile float follow_push;        /* the right stick while the direction turns (-0.3, 0, 0.3) */
static volatile int follow_has_arc;       /* the follow camera's vertical arc is known */
static int follow_bad, follow_off;        /* frames in a row with unexpected fields; 1: callback off */
/* The fields looked right at least once: unexpected values later are a camera mode of the game
 * (climbing tilts the direction), not a wrong offset. Then the mouse is a right stick only while
 * they last, and the camera takes it again as soon as they look right (the same for aim/cover). */
static int follow_seen_ok, aim_seen_ok, cover_seen_ok;
/* Back from the stick only after this many good frames in a row, so a camera at the edge of the
 * check does not switch the mouse every frame; and at most one such log line every 5 s. */
enum { FIELDS_GOOD_AGAIN = 10 };
static int follow_good, aim_good, cover_good;
static int fields_log_now(uint64_t *last_us) {
    const uint64_t now=now_us();
    if (*last_us && now-*last_us<5000000u) return 0;
    *last_us=now;
    return 1;
}
static uint64_t follow_log_us, aim_log_us, cover_log_us;
/* 1 when the camera's fields look like a unit direction that is mostly horizontal and an arc in
 * [0,1]. Climbing tilts the direction (U3: y 0.14, horizontal length 0.99); the turns rotate x/z
 * around the vertical and the heading only reads x/z, so a tilted direction turns the same. A
 * wrong offset gives a length far from 1. */
static int follow_fields_ok(const float *dir, const float *arc) {
    const float len=dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2];
    const float flat=dir[0]*dir[0]+dir[2]*dir[2];
    const int ok=len>0.96f && len<1.04f && flat>=0.49f &&
                 (!arc || (*arc>=-0.01f && *arc<=1.01f));
    if (ok) {
        if (follow_bad>=30) {
            if (++follow_good<FIELDS_GOOD_AGAIN) return 0;
            if (fields_log_now(&follow_log_us))
                printf("Mouse: follow camera fields as expected again, the mouse turns the camera\n");
        }
        follow_bad=follow_good=0;
        follow_seen_ok=1;
        return 1;
    }
    follow_good=0;
    if (++follow_bad==30) {
        follow_off=!follow_seen_ok;
        if (follow_off || fields_log_now(&follow_log_us)) printf("Mouse: follow camera fields unexpected (direction %.3f %.3f %.3f, arc %.3f), %s\n",
               dir[0],dir[1],dir[2],arc ? *arc : 0.0f,
               follow_off ? "mouse camera off, the mouse is a stick" : "the mouse is a stick until they look right again");
    }
    return 0;
}
/* arc_degrees: the camera pitch one unit of arc covers (U1 61; measured in U2 about 135, in U3 about 105) */
static void follow_camera(uint64_t camera, uint32_t dir_offset, uint32_t arc_offset, float arc_degrees) {
    if (follow_off || !camera) return;
    float *dir=(float *)(uintptr_t)(camera+dir_offset);
    float *arc=arc_offset ? (float *)(uintptr_t)(camera+arc_offset) : NULL;
    if (!follow_fields_ok(dir,arc)) return;
    const uint64_t now=now_us();
    follow_has_arc=arc_offset!=0;
    if (camera_hook_us && now-camera_hook_us>100000) {
        float ignore_x, ignore_y;
        camera_take(0,&ignore_x,&ignore_y);
        follow_moved_us=0;
    }
    camera_hook_us=now;
    const float heading=heading_of(dir);
    float dx, dy;
    camera_take(0,&dx,&dy);
    if (aim_held) { follow_moved_us=0; follow_pending=0; return; } /* the aiming camera takes the mouse */
    if (follow_pending) {
        float kept=wrap_degrees(heading-follow_before)/wrap_degrees(follow_after-follow_before);
        kept=kept<0.0f ? 0.0f : kept>1.0f ? 1.0f : kept;
        follow_kept+=0.2f*(kept-follow_kept);
        const int direct=follow_kept<0.12f ? 0 : follow_kept>0.3f ? 1 : follow_direct;
        if (follow_rewrite>=0) {
            struct u1_turn *t=&u1_turns[u1_turn_count++%16];
            t->us=now; t->camera=camera; t->kept=kept; t->rewrite=follow_rewrite; t->calls=u1_calls;
            t->changed=u1_turned_camera && u1_turned_camera!=camera;
            if (kept<0.05f) { if (follow_rewrite) ++follow_lost_rewrite; else ++follow_lost_other; }
            else { if (follow_rewrite) ++follow_kept_rewrite; else ++follow_kept_other; }
        }
        if (direct!=follow_direct) printf("Mouse: follow camera %s (kept %.2f)\n",direct ? "turns with the mouse" : "keeps no turns, the mouse turns it with the stick",follow_kept);
        if (direct!=follow_direct && follow_rewrite>=0 && !direct) {
            const unsigned n=u1_turn_count<16 ? u1_turn_count : 16;
            for (unsigned i=u1_turn_count-n; i<u1_turn_count; ++i) {
                const struct u1_turn *t=&u1_turns[i%16];
                printf("Mouse: U1 turn t=%.4f camera %#llx kept %.2f rewrite %d calls/frame %d%s\n",t->us*1e-6,
                       (unsigned long long)t->camera,t->kept,t->rewrite,t->calls,t->changed ? " CAMERA CHANGED" : "");
            }
        }
        if (direct!=follow_direct && follow_rewrite>=0) {
            printf("Mouse: U1 rewrites (+0x1410) since the last switch: turns lost %u with a rewrite, %u without; turns kept %u with, %u without\n",
                   follow_lost_rewrite,follow_lost_other,follow_kept_rewrite,follow_kept_other);
            follow_lost_rewrite=follow_lost_other=follow_kept_rewrite=follow_kept_other=0;
        }
        follow_direct=direct;
        follow_pending=0;
    }
    const float turn=dx*MOUSE_DEGREES_PER_PIXEL*mouse_cfg.sensitivity;
    /* the wanted heading and how far the direction is short of it (degrees still to turn). The
     * camera rests 0.15 s after the mouse stopped, once the direction has reached the wanted heading
     * (fast turns go beyond what the game keeps in a frame) or at the latest after 0.5 s. */
    float correction=wrap_degrees(heading-follow_target);
    const uint64_t still=now-follow_moved_us;
    const int resting=!follow_moved_us || (still>150000 && (SDL_fabsf(correction)<0.5f || still>500000));
    if (turn!=0.0f) follow_moved_us=now;
    if (resting) {
        follow_target=heading;
        correction=0.0f;
    }
    if (correction>90.0f || correction<-90.0f) { /* at most a quarter turn behind */
        correction=correction>0.0f ? 90.0f : -90.0f;
        follow_target=wrap_degrees(heading-correction);
    }
    follow_target=wrap_degrees(follow_target-turn); /* a positive turn lowers the heading */
    if (!follow_direct) {
        /* the stick turns the camera (with the shortfall as extra speed); a small probe turn shows
         * whether direct turns are kept again */
        follow_correction=correction;
        follow_push=0.0f;
        if (turn!=0.0f) {
            const float probe=turn>0.0f ? 0.3f : -0.3f;
            const float a=probe*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
            const float x=dir[0], z=dir[2];
            dir[0]=x*c-z*s;
            dir[2]=x*s+z*c;
            follow_before=heading;
            follow_after=heading_of(dir);
            follow_pending=1;
            u1_turned_camera=camera;
        }
        if (mouse_debug()) printf("Mouse: follow t=%.4f dx=%.1f heading=%.2f target=%.2f kept=%.2f rewrite=%d stick\n",
                                  now*1e-6,dx,heading,follow_target,follow_kept,follow_rewrite);
        return;
    }
    follow_correction=0.0f;
    /* the shortfall is made up a few degrees a frame (no jump when the game keeps turns again) */
    /* GOW3_MOUSE_CLAMP / GOW3_MOUSE_PUSH (tuning, defaults 4 and 0.3) */
    static float clamp=-1.0f, push=-1.0f;
    if (clamp<0.0f) { const char *v=getenv("GOW3_MOUSE_CLAMP"); clamp=v && atof(v)>0 ? (float)atof(v) : 4.0f; }
    if (push<0.0f) { const char *v=getenv("GOW3_MOUSE_PUSH"); push=v && atof(v)>=0 ? (float)atof(v) : 0.3f; }
    if (correction<-clamp) correction=-clamp;
    if (correction>clamp) correction=clamp;
    const float total=turn+correction;
    follow_push=total>0.05f ? push : total<-0.05f ? -push : 0.0f;
    if (mouse_debug()) printf("Mouse: follow t=%.4f dx=%.1f heading=%.2f target=%.2f turn=%.2f total=%.2f\n",
                              now*1e-6,dx,heading,follow_target,turn,total);
    if (total!=0.0f) {
        const float a=total*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float x=dir[0], z=dir[2];
        dir[0]=x*c-z*s;
        dir[2]=x*s+z*c;
        if (SDL_fabsf(total)>=0.15f) {
            follow_before=heading;
            follow_after=heading_of(dir);
            follow_pending=1;
            u1_turned_camera=camera;
        }
    }
    if (dy!=0.0f && arc) {
        float v=*arc+(mouse_cfg.invert_y ? -dy : dy)*(MOUSE_DEGREES_PER_PIXEL/arc_degrees)*mouse_cfg.sensitivity;
        *arc=v<0.0f ? 0.0f : v>1.0f ? 1.0f : v;
    }
}
static void __attribute__((ms_abi)) u1_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)image; (void)r14;
    follow_rewrite=camera && u1_rewrite_log() ? *(volatile uint8_t *)(uintptr_t)(camera+0x1410) : -1;
    if (follow_rewrite>=0) {
        const uint64_t now=now_us();
        u1_calls=u1_last_call_us && now-u1_last_call_us<8000 ? u1_calls+1 : 1;
        u1_last_call_us=now;
        if (camera!=u1_camera) {
            if (u1_camera && now-u1_changed_log_us>=1000000) {
                printf("Mouse: U1 follow camera object changed %#llx -> %#llx (%d calls/frame)\n",
                       (unsigned long long)u1_camera,(unsigned long long)camera,u1_calls);
                u1_changed_log_us=now;
            }
            u1_camera=camera;
        }
    }
    follow_camera(camera,0x1310,0x12bc,61.0f);
}
static void __attribute__((ms_abi)) u2_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x9ec0,0x9e60,135.0f);
}
static void __attribute__((ms_abi)) u3_follow_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)image; (void)r14;
    follow_camera(camera,0x78c0,0x7874,105.0f);
}
/* U1: the game's pad update fills the caller's joypad state (its first argument): the sticks as
 * floats lx ly rx ry at +0 and again at +0x10, converted from the pad's bytes. Cameras other than
 * the follow camera (aiming: about 45 degrees/s * x|x|, no speed ramp) get the mouse as a right
 * stick value there, not limited to the stick's range. */
static volatile uint64_t joypad_state;   /* the state the pad update fills (its first argument) */
static uint64_t sticks_last_us;
static float aim_stick(float rate) {
    const float r=rate<0 ? -rate : rate;
    const float x=r<=45.0f ? SDL_sqrtf(r/45.0f) : (r+9.0f)/54.0f;
    return rate<0 ? -x : x;
}
/* The follow camera's response to the right stick value (measured in U1): about 144 x^2 degrees/s up
 * to x = 0.5, then a speed that builds up over a second towards 240 degrees/s from x = 0.7. */
static float follow_stick(float rate) {
    const float r=rate<0 ? -rate : rate;
    float x=r<=36.0f ? SDL_sqrtf(r/144.0f) : 0.5f+(r-36.0f)/(240.0f-36.0f)*0.4f;
    if (x>0.9f) x=0.9f;
    return rate<0 ? -x : x;
}
static void __attribute__((ms_abi)) u1_pad_update(uint64_t state, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)r14;
    if (mouse_debug() && joypad_state!=state)
        printf("Mouse: joypad state %#llx (image %#llx)\n",(unsigned long long)state,(unsigned long long)image);
    joypad_state=state;
}
static volatile uint64_t aim_hook_us; /* the aiming camera's callback, below */
static volatile int aim_direct;
/* the cover camera's state (below) */
static volatile uint64_t cover_hook_us;
static volatile int cover_direct;
static int cover_off;
/* U1's right stick as the game gets it, the mouse included (for GOW3_MOUSE_DISCOVER records) */
static volatile float u1_right[2];
static volatile uint64_t u1_right_us;
static void __attribute__((ms_abi)) u1_joypad_sticks(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t r14) {
    (void)unused0; (void)unused1; (void)image;
    const uint64_t state=joypad_state;
    if (!state || r14-0x62!=state) return; /* pad 0 only */
    float *pad=(float *)(uintptr_t)state;
    if (!(pad[2]>=-2.0f && pad[2]<=2.0f && pad[3]>=-2.0f && pad[3]<=2.0f)) return; /* not stick values */
    const uint64_t now=now_us();
    sticks_hook_us=now;
    float dt=sticks_last_us ? (float)(now-sticks_last_us)*1e-6f : 1.0f/60.0f;
    sticks_last_us=now;
    if (dt<1.0f/480.0f) dt=1.0f/480.0f;
    if (dt>0.05f) dt=0.05f;
    float dx, dy;
    camera_take(1,&dx,&dy);
    /* the turn rate (degrees/s) the mouse asks for, through the inverse of the aiming curve
     * measured in U1: 45 x^2 degrees/s up to x = 1, then about 54 x - 9 */
    const float scale=MOUSE_DEGREES_PER_PIXEL*mouse_sensitivity()/dt;
    const int following=camera_hook_us && now-camera_hook_us<50000;
    float push=0.0f;
    if (!aim_held && following && follow_direct) { /* the follow camera turned itself */
        dx=dy=0.0f;
        push=follow_push;
    }
    if (aim_held && aim_hook_us && now-aim_hook_us<100000 && aim_direct) dx=dy=0.0f; /* the aiming camera turned itself */
    if (!aim_held && !following && !cover_off && cover_hook_us && now-cover_hook_us<100000 && cover_direct)
        dx=dy=0.0f; /* so did the cover camera */
    float (*curve)(float)=aim_held ? aim_stick : follow_stick;
    /* following through the joypad: the shortfall is made up within about a quarter second */
    const float extra=!aim_held && following && !follow_direct ? follow_correction*4.0f : 0.0f;
    const float x=curve(dx*scale+extra)+push;
    const float y=curve((mouse_cfg.invert_y ? -dy : dy)*scale);
    if (mouse_debug()) printf("Mouse: aim t=%.4f dt=%.4f dx=%.1f x=%.3f game=%.3f\n",now*1e-6,dt,dx,x,pad[2]);
    if (x!=0.0f) { pad[2]+=x; pad[6]+=x; }
    if (y!=0.0f) { pad[3]+=y; pad[7]+=y; }
    u1_right[0]=pad[2]; u1_right[1]=pad[3]; u1_right_us=now;
}
/* The functions' first bytes in 1.00 and 1.02 (?? : a RIP-relative displacement); a pattern
 * found other than exactly once is left alone (probe.c). U1's pad update pattern is found twice in
 * 1.02: the right one has the sticks code 0x64e bytes in. */
#define U1_STICKS "80 7c 01 6d 00 74 3b c5 e2 59 64 01 40 c4 c1 7a 11 66 be c5 e2 59 64 01"
/* The aiming cameras (fields found by the GOW3_MOUSE_DISCOVER dumps) hold the 3D look direction
 * (unit, the pitch in y) and the same vector again: U3 (CameraControlZoom, the update at file
 * 0x515cc0 in 1.02, 0x13fa60 in 1.00, its object, a new one every few frames, in rdi) at +0x200
 * and +0x11e0; U1 (camerazoom, the update at eboot file 0xc95c0 in 1.02, its object in rsi, rdi
 * the caller's buffer) at +0x1680 and +0x16b0, their 4th floats left alone; U2 (camerazoom, the
 * update at big2 file 0x459670 in 1.02, 0x194270 in 1.00, its object in rdi, one of two in turn)
 * at +0x1f30, +0xdc0 and +0xd90.
 * GOW3_MOUSE_AIM=0 leaves aiming to the stick. While aiming the mouse turns it directly:
 * yaw around the world's y axis, pitch up to 70 degrees either way. Checked like the follow camera
 * (30 bad frames in a row turn it off); should the game not keep the turns, the mouse goes back to
 * the right stick (U1: through the joypad state). */
static volatile uint64_t aim_hook_us;
static volatile int aim_direct=1;
static int aim_bad, aim_off, aim_pending;
static float aim_kept=1.0f, aim_before, aim_after;
static void discover_call(int f, uint64_t rdi, uint64_t rsi);
#define DISCOVER_EDGE 26 /* the climbing aim candidates start here (GOW3_MOUSE_DISCOVER) */
static int discover_on(void) {
    static int on=-1;
    if (on<0) { const char *v=getenv("GOW3_MOUSE_DISCOVER"); on=v && v[0]=='1'; }
    return on;
}
static float aim_heading(const float *v) { return SDL_atan2f(v[0],v[2])*57.29578f; }
static int aim_enabled(void) {
    static int on=-1;
    if (on<0) { const char *v=getenv("GOW3_MOUSE_AIM"); on=!(v && v[0]=='0'); }
    return on;
}
/* Turns a unit look vector by yaw degrees around the world's y axis and raises it by pitch
 * degrees (within 70 either way); leaves it alone unless its length is within 0.04 of 1. */
static void aim_rotate(float *v, float turn, float pitch) {
    const float len=SDL_sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    if (!(SDL_fabsf(len-1.0f)<0.04f)) return;
    float x=v[0], y=v[1], z=v[2];
    if (turn!=0.0f) {
        const float a=turn*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float nx=x*c-z*s, nz=x*s+z*c;
        x=nx; z=nz;
    }
    if (pitch!=0.0f) {
        const float h=SDL_sqrtf(x*x+z*z);
        if (h>1e-4f) {
            float el=SDL_asinf(y<-1.0f ? -1.0f : y>1.0f ? 1.0f : y)*57.29578f+pitch;
            el=el<-70.0f ? -70.0f : el>70.0f ? 70.0f : el;
            const float r=el*0.017453293f, ch=SDL_cosf(r);
            x=x/h*ch; z=z/h*ch; y=SDL_sinf(r);
        }
    }
    v[0]=x; v[1]=y; v[2]=z;
}
/* GOW3_MOUSE_AIM_U1_ALL=1 (off by default; the launcher turns it on for U1): U1's aim object holds
 * two more look vectors within a degree of +0x1680, at +0x1690 and +0x1830 (the update writes this
 * one itself), which may pull our turn back; each gets the same turn and pitch change, rotated
 * rather than copied. */
static int aim_u1_all(void) {
    static int on=-1;
    if (on<0) { const char *v=getenv("GOW3_MOUSE_AIM_U1_ALL"); on=v && v[0]=='1'; if (on) printf("Mouse: U1 aim turns the look vectors at +0x1690 and +0x1830 too\n"); }
    return on;
}
/* copy2_at: a second copy (U2), 0 for none; more: further look vectors turned alike (count n_more).
 * Returns the turn given (degrees), 0 for none. settle_us: U1's zoom in puts its own direction back
 * for about half a second after aiming starts, but takes the right stick; for settle_us the mouse
 * goes to the stick (with probe turns) for as long as the last turn written was not kept exactly,
 * and back as soon as one is, with no smoothing of the decision. */
static uint64_t aim_start_us;
static int aim_was_held, aim_settling;
static float aim_camera(uint64_t camera, size_t dir_at, size_t copy_at, size_t copy2_at, const size_t *more, int n_more,
                        uint64_t settle_us) {
    if (!aim_enabled() || aim_off || !camera) return 0.0f;
    float *v=(float *)(uintptr_t)(camera+dir_at), *copy=(float *)(uintptr_t)(camera+copy_at);
    const float len=v[0]*v[0]+v[1]*v[1]+v[2]*v[2];
    if (!(len>0.96f && len<1.04f) || !(v[1]>-0.9f && v[1]<0.9f) || SDL_fabsf(copy[0]-v[0])>1e-3f || SDL_fabsf(copy[1]-v[1])>1e-3f ||
        SDL_fabsf(copy[2]-v[2])>1e-3f) {
        aim_good=0;
        if (++aim_bad==30) {
            aim_off=!aim_seen_ok;
            if (aim_off || fields_log_now(&aim_log_us)) printf("Mouse: aim camera fields unexpected (%.3f %.3f %.3f / %.3f %.3f %.3f), mouse aims with the stick%s\n",
                   v[0],v[1],v[2],copy[0],copy[1],copy[2],aim_off ? "" : " until they look right again");
        }
        return 0.0f;
    }
    if (aim_bad>=30) {
        if (++aim_good<FIELDS_GOOD_AGAIN) return 0.0f;
        if (fields_log_now(&aim_log_us)) printf("Mouse: aim camera fields as expected again\n");
    }
    aim_bad=aim_good=0;
    aim_seen_ok=1;
    const uint64_t last_us=aim_hook_us;
    aim_hook_us=now_us();
    float dx, dy;
    camera_take(2,&dx,&dy);
    const float heading=aim_heading(v);
    if (aim_held && (!aim_was_held || aim_hook_us-last_us>250000)) { /* aiming starts */
        aim_start_us=aim_hook_us;
        aim_settling=settle_us!=0;
        if (aim_settling) { aim_kept=1.0f; aim_direct=0; aim_pending=0; }
    }
    aim_was_held=aim_held;
    if (aim_pending && aim_settling) { /* kept: the direction is still exactly what was written */
        aim_direct=SDL_fabsf(wrap_degrees(heading-aim_after))<0.05f;
        aim_pending=0;
    }
    if (aim_settling && aim_hook_us-aim_start_us>=settle_us) { aim_settling=0; aim_kept=aim_direct ? 1.0f : 0.0f; }
    if (aim_pending && !aim_settling) { /* how much of the last turn is still there */
        float kept=wrap_degrees(heading-aim_before)/wrap_degrees(aim_after-aim_before);
        kept=kept<0.0f ? 0.0f : kept>1.0f ? 1.0f : kept;
        aim_kept+=0.2f*(kept-aim_kept);
        const int direct=aim_kept<0.12f ? 0 : aim_kept>0.3f ? 1 : aim_direct;
        if (direct!=aim_direct) printf("Mouse: aim camera %s (kept %.2f, %.0f ms after aiming started)\n",direct ? "turns with the mouse" : "keeps no turns, mouse aims with the stick",
                                       aim_kept,(double)(aim_hook_us-aim_start_us)/1000.0);
        aim_direct=direct;
        aim_pending=0;
    }
    if (!aim_held) return 0.0f;
    float turn=dx*MOUSE_DEGREES_PER_PIXEL*mouse_sensitivity();
    float pitch=(mouse_cfg.invert_y ? dy : -dy)*MOUSE_DEGREES_PER_PIXEL*mouse_sensitivity();
    if (!aim_direct) { if (turn!=0.0f) turn=turn>0.0f ? 0.5f : -0.5f; pitch=0.0f; } /* a probe turn */
    if (turn==0.0f && pitch==0.0f) return 0.0f;
    const float el_before=SDL_asinf(v[1])*57.29578f;
    aim_rotate(v,turn,pitch);
    const float x=v[0], y=v[1], z=v[2];
    copy[0]=x; copy[1]=y; copy[2]=z;
    /* the others get the pitch change the main vector actually got (after its 70 degree limit) */
    const float dpitch=SDL_asinf(y<-1.0f ? -1.0f : y>1.0f ? 1.0f : y)*57.29578f-el_before;
    for (int i=0;i<n_more;++i) aim_rotate((float *)(uintptr_t)(camera+more[i]),turn,dpitch);
    if (copy2_at) {
        float *copy2=(float *)(uintptr_t)(camera+copy2_at);
        copy2[0]=x; copy2[1]=y; copy2[2]=z;
    }
    if (SDL_fabsf(turn)>=0.15f) { aim_before=heading; aim_after=aim_heading(v); aim_pending=1; }
    if (mouse_debug()) printf("Mouse: aim t=%.4f dx=%.1f dy=%.1f heading=%.2f->%.2f y=%.3f kept=%.2f%s\n",
                              aim_hook_us*1e-6,dx,dy,heading,aim_heading(v),y,aim_kept,aim_direct ? "" : " stick");
    return turn;
}
/* GOW3_MOUSE_DISCOVER: the callers (return addresses, image offsets) of the aiming cameras' updates,
 * counted per second for each camera: they should share the base method that calls UpdateLocation. */
static void discover_ret(const char *name, int slot, uint64_t image) {
    static struct { uint64_t at, calls; } seen[4][8];
    static uint64_t second_us[4];
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    const uint64_t ret=runtime_guest_ret, now=now_us();
    pthread_mutex_lock(&lock);
    if (ret>=image && ret<image+0x4000000) {
        int i=0;
        while (i<8 && seen[slot][i].at && seen[slot][i].at!=ret-image) ++i;
        if (i<8) { seen[slot][i].at=ret-image; ++seen[slot][i].calls; }
    }
    if (!second_us[slot]) second_us[slot]=now;
    if (now-second_us[slot]>=1000000) {
        second_us[slot]=now;
        char line[512];
        int n=snprintf(line,sizeof(line),"Mouse discover: %s returns",name);
        for (int i=0;i<8 && seen[slot][i].at;++i) {
            if (seen[slot][i].calls) n+=snprintf(line+n,sizeof(line)-n," img %#llx x%llu",(unsigned long long)seen[slot][i].at,(unsigned long long)seen[slot][i].calls);
            seen[slot][i].calls=0;
        }
        puts(line);
    }
    pthread_mutex_unlock(&lock);
}
static volatile uint64_t aim_request_us; /* the last u3_aim_camera call */
static void __attribute__((ms_abi)) u3_aim_camera(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)r14;
    if (discover_on()) { discover_call(2,camera,0); discover_ret("U3 aim camera",0,image); }
    aim_request_us=now_us();
    aim_camera(camera,0x200,0x11e0,0,NULL,0,0);
}
static int edge_aim_enabled(void);
/* U3 1.02: in some places (hanging on some walls) the zoom camera runs its UpdateLocation (vtable +0x150,
 * big3 file 0x51f3e0, its object in rsi; found by the GOW3_MOUSE_CAMERA_LOG census) without the request
 * evaluation that calls u3_aim_camera (+0x128). There the mouse turns the same fields from here, with
 * GOW3_MOUSE_EDGE_AIM=1 (aiming while hanging, off by default). */
static void __attribute__((ms_abi)) u3_aim_camera_update(uint64_t locator, uint64_t camera, uint64_t image, uint64_t r14) {
    (void)locator; (void)r14;
    if (!edge_aim_enabled() || (aim_request_us && now_us()-aim_request_us<100000)) return;
    /* the update leaves early (keeps the last locator) while the game's time scale is 0 (paused):
     * [[image+0x1b3a838]+0xa0]+0x14 (Opus); no turns then, so none pile up */
    const uint64_t clock=*(const uint64_t *)(uintptr_t)(image+0x1b3a838);
    if (clock>=0x10000) {
        const uint64_t at=*(const uint64_t *)(uintptr_t)(clock+0xa0);
        if (at>=0x10000 && *(const float *)(uintptr_t)(at+0x14)==0.0f) return;
    }
    /* here +0x11e0 is the state (the update rewrites +0x200 from its final locator, so the two differ):
     * it alone is checked and turned */
    aim_camera(camera,0x11e0,0x11e0,0,NULL,0,0);
}
static void __attribute__((ms_abi)) u3_aim_camera_100(uint64_t camera, uint64_t unused, uint64_t image, uint64_t r14) {
    (void)unused; (void)image; (void)r14;
    if (discover_on()) discover_call(19,camera,0);
    aim_camera(camera,0x200,0x11e0,0,NULL,0,0);
}
/* GOW3_MOUSE_AIM_U1_SETTLE_MS=N (test; default 600, 0-2000): how long after aiming starts U1's mouse
 * goes to the stick while the zoom in may still put its own direction back. */
static uint64_t u1_aim_settle_us(void) {
    static long ms=-1;
    if (ms<0) {
        const char *v=getenv("GOW3_MOUSE_AIM_U1_SETTLE_MS");
        char *end=NULL;
        ms=v && v[0] ? strtol(v,&end,10) : 600;
        if (v && v[0] && (*end || ms<0 || ms>2000)) ms=600;
        if (ms!=600) printf("Mouse: U1 aim settles for %ld ms (GOW3_MOUSE_AIM_U1_SETTLE_MS)\n",ms);
    }
    return (uint64_t)ms*1000;
}
static void __attribute__((ms_abi)) u1_aim_camera(uint64_t buffer, uint64_t camera, uint64_t image, uint64_t r14) {
    (void)image; (void)r14;
    if (discover_on()) discover_call(6,buffer,camera);
    static const size_t u1_more[]={0x1690,0x1830};
    aim_camera(camera,0x1680,0x16b0,0,u1_more,aim_u1_all() ? 2 : 0,aim_u1_all() ? u1_aim_settle_us() : 0);
}
static void __attribute__((ms_abi)) u2_aim_camera(uint64_t camera, uint64_t rsi, uint64_t image, uint64_t r14) {
    (void)image; (void)r14;
    if (discover_on()) discover_call(15,camera,rsi);
    aim_camera(camera,0x1f30,0xdc0,0xd90,NULL,0,0);
}
static void __attribute__((ms_abi)) u2_aim_camera_100(uint64_t camera, uint64_t rsi, uint64_t image, uint64_t r14) {
    (void)image; (void)r14;
    if (discover_on()) discover_call(20,camera,rsi);
    aim_camera(camera,0x1f30,0xdc0,0xd90,NULL,0,0);
}
/* U3 aiming while hanging on a climb (CameraEdgeAim, the update at big3 file 0x4dbf00 in 1.02; found
 * by the GOW3_MOUSE_DISCOVER dumps, read by Opus): its object in rsi, the previous Locator in rdx
 * (position, then a rotation quaternion at +0x10). The game turns the look vector at +0x200 (copies
 * at +0x210 and +0x230) by the right stick as a rate (45 degrees/s at most), around the previous
 * Locator's y axis, not the world's. GOW3_MOUSE_EDGE_AIM=1 (off by default): the mouse turns it
 * directly, yaw around that axis (within 90 degrees of where aiming started) and pitch toward it
 * (60 degrees at most either way, |dot| <= 0.87); like the other cameras, should the game not keep
 * the turns, the mouse goes back to the right stick. */
static volatile uint64_t edge_hook_us;
static volatile int edge_direct=1;
static int edge_pending;
static float edge_kept=1.0f, edge_before[3], edge_after[3], edge_start[3];
static uint64_t edge_log_us;
static int edge_aim_enabled(void) {
    static int on=-1;
    if (on<0) { const char *v=getenv("GOW3_MOUSE_EDGE_AIM"); on=v && v[0]=='1'; if (on) printf("Mouse: aiming on a climb turns with the mouse\n"); }
    return on;
}
static float dot3(const float *a, const float *b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static void __attribute__((ms_abi)) u3_edge_aim_camera(uint64_t locator, uint64_t camera, uint64_t image, uint64_t previous) {
    if (discover_on()) { discover_call(DISCOVER_EDGE,locator,camera); discover_ret("U3 climbing aim camera",1,image); }
    if (!edge_aim_enabled() || !camera || previous<0x10000) return;
    float *v=(float *)(uintptr_t)(camera+0x200);
    const float *q=(const float *)(uintptr_t)(previous+0x10);
    const float qq=q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3], vv=dot3(v,v);
    if (!(qq>0.96f && qq<1.04f) || !(vv>0.96f && vv<1.04f)) {
        if (fields_log_now(&edge_log_us)) printf("Mouse: climbing aim fields unexpected (%.3f %.3f %.3f, rotation %.3f %.3f %.3f %.3f), mouse aims with the stick\n",
                                                 v[0],v[1],v[2],q[0],q[1],q[2],q[3]);
        return;
    }
    /* up: the previous Locator's rotation applied to (0, 1, 0) */
    float up[3]={2.0f*(q[0]*q[1]-q[3]*q[2]),1.0f-2.0f*(q[0]*q[0]+q[2]*q[2]),2.0f*(q[1]*q[2]+q[3]*q[0])};
    const float ul=SDL_sqrtf(dot3(up,up));
    for (int i=0;i<3;++i) up[i]/=ul;
    const uint64_t last_us=edge_hook_us;
    edge_hook_us=now_us();
    float dx, dy;
    camera_take(4,&dx,&dy);
    if (!last_us || edge_hook_us-last_us>250000) { /* aiming on a climb starts */
        for (int i=0;i<3;++i) edge_start[i]=v[i];
        edge_pending=0;
    }
    if (edge_pending) { /* how much of the last turn is still there */
        float d[3], w[3];
        for (int i=0;i<3;++i) { d[i]=v[i]-edge_before[i]; w[i]=edge_after[i]-edge_before[i]; }
        float kept=dot3(d,w)/dot3(w,w);
        kept=kept<0.0f ? 0.0f : kept>1.0f ? 1.0f : kept;
        edge_kept+=0.2f*(kept-edge_kept);
        const int direct=edge_kept<0.12f ? 0 : edge_kept>0.3f ? 1 : edge_direct;
        if (direct!=edge_direct) printf("Mouse: climbing aim %s (kept %.2f)\n",direct ? "turns with the mouse" : "keeps no turns, mouse aims with the stick",edge_kept);
        edge_direct=direct;
        edge_pending=0;
    }
    float turn=dx*MOUSE_DEGREES_PER_PIXEL*mouse_sensitivity();
    float pitch=(mouse_cfg.invert_y ? dy : -dy)*MOUSE_DEGREES_PER_PIXEL*mouse_sensitivity();
    if (!edge_direct) { if (turn!=0.0f) turn=turn>0.0f ? 0.5f : -0.5f; pitch=0.0f; } /* a probe turn */
    if (turn==0.0f && pitch==0.0f) return;
    /* v = h cos(e) + up sin(e): h the horizontal direction (unit, across up), e the elevation */
    float e=dot3(v,up);
    e=e<-1.0f ? -1.0f : e>1.0f ? 1.0f : e;
    float h[3]={v[0]-e*up[0],v[1]-e*up[1],v[2]-e*up[2]};
    const float hl=SDL_sqrtf(dot3(h,h));
    if (hl<1e-3f) return;
    for (int i=0;i<3;++i) h[i]/=hl;
    if (turn!=0.0f) { /* the same sense as aim_rotate around the world's y */
        const float a=turn*0.017453293f, c=SDL_cosf(a), s=SDL_sinf(a);
        const float side[3]={up[1]*h[2]-up[2]*h[1],up[2]*h[0]-up[0]*h[2],up[0]*h[1]-up[1]*h[0]};
        float n[3];
        for (int i=0;i<3;++i) n[i]=h[i]*c-side[i]*s;
        float s0[3]={edge_start[0],edge_start[1],edge_start[2]};
        const float se=dot3(s0,up);
        for (int i=0;i<3;++i) s0[i]-=se*up[i];
        if (dot3(n,s0)>=0.0f || dot3(n,s0)>=dot3(h,s0)) for (int i=0;i<3;++i) h[i]=n[i]; /* within 90 degrees of the start */
    }
    const float el_before=SDL_asinf(e)*57.29578f;
    float el=el_before+pitch; /* never past 60 degrees by the mouse, but no snap from beyond */
    if (el>60.0f) el=el_before>60.0f ? el_before : 60.0f;
    if (el<-60.0f) el=el_before<-60.0f ? el_before : -60.0f;
    const float r=el*0.017453293f, ch=SDL_cosf(r), sh=SDL_sinf(r);
    float out[3];
    for (int i=0;i<3;++i) out[i]=h[i]*ch+up[i]*sh;
    for (int i=0;i<3;++i) edge_before[i]=v[i];
    float *copy1=(float *)(uintptr_t)(camera+0x210), *copy2=(float *)(uintptr_t)(camera+0x230);
    for (int i=0;i<3;++i) { v[i]=out[i]; copy1[i]=out[i]; copy2[i]=out[i]; edge_after[i]=out[i]; }
    float w[3]={out[0]-edge_before[0],out[1]-edge_before[1],out[2]-edge_before[2]};
    edge_pending=dot3(w,w)>=0.0026f*0.0026f;
    if (mouse_debug()) printf("Mouse: climbing aim t=%.4f dx=%.1f dy=%.1f up=%.3f %.3f %.3f dir=%.3f %.3f %.3f->%.3f %.3f %.3f kept=%.2f%s\n",
                              edge_hook_us*1e-6,dx,dy,up[0],up[1],up[2],edge_before[0],edge_before[1],edge_before[2],out[0],out[1],out[2],
                              edge_kept,edge_direct ? "" : " stick");
}
/* U3 cover (camera-cover.cpp, the update at file 0x4d9bd0 in 1.02, 0x119060 in 1.00, its camera in rsi; fields found
 * by the GOW3_MOUSE_DISCOVER dumps): the orbit's horizontal look direction (x, 0, z) at +0x2f0 and
 * its pitch in degrees at +0x2e0 (the game keeps it within about 43 degrees); U2 (cameracover.cpp,
 * the update at big2 file 0x41fc80 in 1.02, 0x168940 in 1.00, also in rsi) the same at +0x1c0 and +0x1b0 (-80 to 50
 * degrees); U1 (CameraControlCover, the update at eboot file 0xa9880 in 1.02, 0xa9980 in 1.00, also in
 * rsi) at +0x240 and +0x230 (-45 to 45 degrees). The mouse turns them
 * directly, checked like the others (30 bad frames in a row: off); should the game not keep the
 * turns, the mouse goes back to the right stick. GOW3_MOUSE_COVER=0 leaves cover to the stick. */
static volatile uint64_t cover_hook_us;
static volatile int cover_direct=1;
static int cover_bad, cover_off, cover_pending;
static float cover_kept=1.0f, cover_before, cover_after;
static int cover_enabled(void) {
    static int on=-1;
    if (on<0) { const char *v=getenv("GOW3_MOUSE_COVER"); on=!(v && v[0]=='0'); }
    return on;
}
/* The cover camera's horizontal direction at dir_at and pitch (degrees) at pitch_at, which the game
 * keeps within lo..hi. */
static void cover_camera(uint64_t camera, size_t dir_at, size_t pitch_at, float lo, float hi) {
    if (!cover_enabled() || cover_off || !camera) return;
    float *dir=(float *)(uintptr_t)(camera+dir_at), *pitch=(float *)(uintptr_t)(camera+pitch_at);
    /* usually horizontal, but tilted in some covers (slopes, ledges: y up to 0.1 seen); taken while
     * mostly horizontal, the tilt kept */
    const float len=dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2], flat=dir[0]*dir[0]+dir[2]*dir[2];
    if (!(len>0.92f && len<1.08f) || !(flat>=0.49f) || !(*pitch>=lo-7.0f && *pitch<=hi+7.0f)) {
        cover_good=0;
        if (++cover_bad==30) {
            cover_off=!cover_seen_ok;
            if (cover_off || fields_log_now(&cover_log_us)) printf("Mouse: cover camera fields unexpected (%.3f %.3f %.3f, pitch %.2f), cover uses the stick%s\n",
                   dir[0],dir[1],dir[2],*pitch,cover_off ? "" : " until they look right again");
        }
        return;
    }
    if (cover_bad>=30) {
        if (++cover_good<FIELDS_GOOD_AGAIN) return;
        if (fields_log_now(&cover_log_us)) printf("Mouse: cover camera fields as expected again\n");
    }
    cover_bad=cover_good=0;
    cover_seen_ok=1;
    cover_hook_us=now_us();
    float dx, dy;
    camera_take(3,&dx,&dy);
    const float heading=aim_heading(dir);
    if (cover_pending) {
        float kept=wrap_degrees(heading-cover_before)/wrap_degrees(cover_after-cover_before);
        kept=kept<0.0f ? 0.0f : kept>1.0f ? 1.0f : kept;
        cover_kept+=0.2f*(kept-cover_kept);
        const int direct=cover_kept<0.12f ? 0 : cover_kept>0.3f ? 1 : cover_direct;
        if (direct!=cover_direct) printf("Mouse: cover camera %s (kept %.2f)\n",direct ? "turns with the mouse" : "keeps no turns, cover uses the stick",cover_kept);
        cover_direct=direct;
        cover_pending=0;
    }
    if (aim_held) return; /* aiming from cover: the aiming camera takes the mouse */
    float turn=dx*MOUSE_DEGREES_PER_PIXEL*mouse_cfg.sensitivity;
    float up=(mouse_cfg.invert_y ? dy : -dy)*MOUSE_DEGREES_PER_PIXEL*mouse_cfg.sensitivity;
    if (!cover_direct) { if (turn!=0.0f) turn=turn>0.0f ? 0.3f : -0.3f; up=0.0f; } /* a probe turn */
    if (turn!=0.0f) {
        const float a=turn*0.017453293f, c=SDL_cosf(a), sn=SDL_sinf(a);
        float x=dir[0]*c-dir[2]*sn, z=dir[0]*sn+dir[2]*c;
        const float l=SDL_sqrtf(x*x+z*z), h=SDL_sqrtf(1.0f-dir[1]*dir[1]); /* the tilt (y) stays */
        dir[0]=x/l*h; dir[2]=z/l*h;
        if (SDL_fabsf(turn)>=0.15f) { cover_before=heading; cover_after=aim_heading(dir); cover_pending=1; }
    }
    if (up!=0.0f) {
        const float v=*pitch+up;
        *pitch=v<lo ? lo : v>hi ? hi : v;
    }
    if (mouse_debug()) printf("Mouse: cover t=%.4f dx=%.1f dy=%.1f heading=%.2f->%.2f pitch=%.2f kept=%.2f\n",
                              cover_hook_us*1e-6,dx,dy,heading,aim_heading(dir),*pitch,cover_kept);
}
/* the camera is the second argument (rsi); rdi is the caller's buffer for the result (Locator) */
static void __attribute__((ms_abi)) u3_cover_camera(uint64_t locator, uint64_t camera, uint64_t image, uint64_t r14) {
    (void)image; (void)r14;
    if (discover_on()) discover_call(3,locator,camera);
    cover_camera(camera,0x2f0,0x2e0,-43.0f,43.0f);
}
static void __attribute__((ms_abi)) u2_cover_camera(uint64_t locator, uint64_t camera, uint64_t image, uint64_t r14) {
    (void)image; (void)r14;
    if (discover_on()) discover_call(9,locator,camera);
    cover_camera(camera,0x1c0,0x1b0,-80.0f,50.0f);
}
static void __attribute__((ms_abi)) u1_cover_camera(uint64_t rdi, uint64_t camera, uint64_t image, uint64_t r14) {
    (void)image; (void)r14;
    if (discover_on()) discover_call(22,rdi,camera);
    cover_camera(camera,0x240,0x230,-45.0f,45.0f);
}
static const RuntimeGuestCallback guest_callbacks[]={
    {"U1 follow camera (mouse)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 28 01 00 00 44 89 8d 00 ff ff ff "
     "49 89 cd 48 89 d3 49 89 f7 49 89 fc 4c 8b 35 ?? ?? ?? ?? 49 8b",6,u1_follow_camera,NULL,0,0},
    {"U1 pad update (mouse)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec b8 00 00 00 49 89 fe 4c 8b 2d ?? ?? ?? ?? 49 8b",
     6,u1_pad_update,U1_STICKS,0x64e,0},
    {"U1 joypad sticks (mouse)",U1_STICKS,5,u1_joypad_sticks,NULL,0,0},
    {"U1 aim camera (mouse, 1.00 and 1.02)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 68 02 00 00 49 89 d7 48 89 f3 "
     "49 89 fe 4c ?? 2d",6,u1_aim_camera,NULL,0,0},
    {"U1 cover camera (mouse, 1.00 and 1.02)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 88 01 00 00 49 89 d7 48 89 f3 "
     "49 89 fe 4c ?? 25 ?? ?? ?? ?? 49 8b 04 24",6,u1_cover_camera,NULL,0,0},
    {"U2 follow camera (mouse, 1.00)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 48 01 00 00 44 89 8d 18 ff ff ff "
     "48 89 8d c8 fe ff ff 49 89 d5 4c 89 ad 38 ff ff ff 48 89 f3 49",6,u2_follow_camera,NULL,0,0},
    {"U2 follow camera (mouse, 1.02)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 28 01 00 00 45 89 cc 48 89 8d f8 fe ff ff "
     "49 89 d6 49 89 fd 48 8b 55 20 4c 8b 7d 18 48 8b 45 28",6,u2_follow_camera,NULL,0,0},
    {"U2 cover camera (mouse, 1.02)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 68 01 00 00 49 89 d7 48 89 f3 "
     "49 89 fe 4c ?? 2d",6,u2_cover_camera,NULL,0,0},
    {"U2 cover camera (mouse, 1.00)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec a8 01 00 00 49 89 d7 49 89 f5 "
     "49 89 fe 4c ?? 25",6,u2_cover_camera,NULL,0,0},
    {"U2 aim camera (mouse, 1.02)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec d8 01 00 00 49 89 f6 49 89 ff "
     "4c ?? 2d ?? ?? ?? ?? 49 8b 45",6,u2_aim_camera,NULL,0,0},
    {"U2 aim camera (mouse, 1.00)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 38 01 00 00 49 89 f6 49 89 ff "
     "4c ?? 2d ?? ?? ?? ?? 49 8b 45 00 48 89 45 d0 e8 92 1b",6,u2_aim_camera_100,NULL,0,0},
    {"U3 follow camera (mouse, 1.00)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec d8 01 00 00 44 89 8d a0 fe ff ff "
     "48 89 8d 48 fe ff ff 49 89 d5 48 89 f3 49 89 ff 4c 8b 65 20 4c",6,u3_follow_camera,NULL,0,0},
    {"U3 follow camera (mouse, 1.02)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec b8 01 00 00 45 89 cc 48 89 8d 68 fe ff ff "
     "49 89 d6 49 89 fd 48 8b 55 20 48 8b 45 28 48 8d 0d ?? ?? ?? ??",6,u3_follow_camera,NULL,0,0},
    {"U3 cover camera (mouse, 1.00)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec b8 02 00 00 48 89 d3 49 89 f4 "
     "49 89 fe 48 ?? 05",6,u3_cover_camera,NULL,0,0},
    {"U3 cover camera (mouse, 1.02)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 38 02 00 00 48 89 d3 49 89 f6 "
     "49 89 ff 48 8d 05 ?? ?? ?? ?? 48 8b 00 48",6,u3_cover_camera,NULL,0,0},
    {"U3 aim camera (mouse, 1.02)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec c8 00 00 00 49 89 f6 48 89 fb "
     "4c 8d 25 ?? ?? ?? ?? 49 8b 04 24",6,u3_aim_camera,NULL,0,0},
    {"U3 climbing aim camera (mouse, 1.02)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 18 02 00 00 48 89 d3 49 89 f6 "
     "49 89 fc 48 8d 15 ?? ?? ?? ?? 48 8b 02 48 89 45 d0 49 8b 86 b0 00",6,u3_edge_aim_camera,NULL,0,1},
    {"U3 aim camera update (mouse, 1.02)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec b8 02 00 00 49 89 d6 49 89 f7 48 89 fb "
     "4c 8d 2d ?? ?? ?? ?? 49 8b 45 00 48 89 45 d0 49 63 87 e4 00 00 00",6,u3_aim_camera_update,NULL,0,0},
    {"U3 aim camera (mouse, 1.00)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 49 89 f6 48 89 fb "
     "4c ?? 2d ?? ?? ?? ?? 49 8b 45 00 48 89 45 d0 e8 25",6,u3_aim_camera_100,NULL,0,0},
};
/* GOW3_MOUSE_DISCOVER=1 (diagnostics): finds the aiming and cover cameras. In 1.02: U3 A-E
 * (CameraControlZoom A-C, camera-cover.cpp D-E), U1 aiming F-I, U2 cover J-M and aiming N-S; in
 * 1.00: U3 aiming T, U2 aiming U-V; U1 cover W-Z in both (see discover_callbacks; the mouse hooks found since call it
 * too). Their functions log how often they run per second, aiming (L2) or not, with their first
 * argument; while aiming or with the follow camera not running (over 100 ms), each writes the
 * first 0x8000 bytes (as far as mapped) of its first and second arguments' objects to
 * GOW3_MOUSE_DISCOVER_FILE (default mouse_discover.bin) at most every 30 ms, 800 times per function.
 * UC3 1.02 hanging cameras 1-9 (CameraEdgeAim 1, now hooked; other hanging aim candidates 2-8:
 * MonkeyBars 2-3, FreeRope 4-5, Spider 6, CameraLookAround 7, CameraControlStick 8; CameraControlEdge
 * 9): 0x2000 bytes, every 50 ms, 1500 times; 2-8 only while L2 is held with the follow, aim and climbing
 * aim hooks all silent (over 100 ms), 1 and 9 also while 1 or 9 ran in the last 100 ms.
 * Record: "MDSC", function (| 0x40 for the second argument) | (aiming | follow off << 1) << 8 |
 * right stick x << 16 | y << 24 (U1: with the mouse in it), time (us), object address, size, bytes. */
static uint8_t last_right[2]={128,128};
#define DISCOVER_FUNCTIONS 35
static struct { uint64_t calls, aim_calls, rdi, last_dump_us; unsigned dumps; } discover[DISCOVER_FUNCTIONS];
static uint64_t discover_second_us, discover_edge_us;
static char discover_label(int f) { return f<DISCOVER_EDGE ? (char)('A'+f) : (char)('1'+f-DISCOVER_EDGE); }
static pthread_mutex_t discover_lock=PTHREAD_MUTEX_INITIALIZER;
/* One record of the object at `at` (0 when it is not mapped memory). */
static int discover_dump(FILE *out, int f, uint64_t at, uint64_t now, int follow_off_now) {
    uint32_t size=at>=0x10000 ? ((f&0x3f)>=DISCOVER_EDGE ? 0x2000 : 0x8000) : 0;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info;
    if (!size || !VirtualQuery((void *)(uintptr_t)at,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS|PAGE_GUARD))) size=0;
    else {
        const uint64_t end=(uint64_t)(uintptr_t)info.BaseAddress+info.RegionSize;
        if (at+size>end) size=(uint32_t)(end-at);
    }
#endif
    if (!out || !size) return 0;
    uint8_t right[2]={last_right[0],last_right[1]};
    if (u1_right_us && now-u1_right_us<100000) /* U1: the stick with the mouse in it */
        for (int i=0;i<2;++i) {
            const float v=128.0f+u1_right[i]*127.5f;
            right[i]=(uint8_t)(v<0.0f ? 0.0f : v>255.0f ? 255.0f : v);
        }
    const uint32_t head[2]={0x4353444du,(uint32_t)f|(uint32_t)(aim_held|follow_off_now<<1)<<8|(uint32_t)right[0]<<16|(uint32_t)right[1]<<24};
    fwrite(head,4,2,out); fwrite(&now,8,1,out); fwrite(&at,8,1,out); fwrite(&size,4,1,out);
    fwrite((const void *)(uintptr_t)at,1,size,out);
    return 1;
}
static void discover_call(int f, uint64_t rdi, uint64_t rsi) {
    const uint64_t now=now_us();
    pthread_mutex_lock(&discover_lock);
    ++discover[f].calls;
    if (aim_held) ++discover[f].aim_calls;
    discover[f].rdi=rdi;
    if (!discover_second_us) discover_second_us=now;
    if (now-discover_second_us>=1000000) {
        char line[1536];
        int n=snprintf(line,sizeof(line),"Mouse discover: aim=%d follow=%d",aim_held,camera_hook_us && now-camera_hook_us<100000);
        for (int i=0;i<DISCOVER_FUNCTIONS && n<(int)sizeof(line);++i) {
            if (!discover[i].calls && !discover[i].rdi) continue; /* not installed in this game */
            n+=snprintf(line+n,sizeof(line)-n,"  %c %llu/%llu rdi=%#llx",discover_label(i),(unsigned long long)discover[i].calls,
                        (unsigned long long)discover[i].aim_calls,(unsigned long long)discover[i].rdi);
        }
        puts(line);
        for (int i=0;i<DISCOVER_FUNCTIONS;++i) discover[i].calls=discover[i].aim_calls=0;
        discover_second_us=now;
    }
    static FILE *out;
    const int follow_off_now=!(camera_hook_us && now-camera_hook_us<100000);
    const int edge=f>=DISCOVER_EDGE;
    if (f==DISCOVER_EDGE || f==DISCOVER_EDGE+8) discover_edge_us=now;
    const int hanging=f>DISCOVER_EDGE && f<DISCOVER_EDGE+8; /* the other hanging aim candidates */
    const int want=hanging ? aim_held && follow_off_now && !(aim_hook_us && now-aim_hook_us<100000) &&
                             !(edge_hook_us && now-edge_hook_us<100000)
                           : aim_held || follow_off_now || (edge && now-discover_edge_us<100000);
    if (want && rdi && discover[f].dumps<(edge ? 1500u : 800u) && now-discover[f].last_dump_us>=(edge ? 50000u : 30000u)) {
        if (!out) {
            const char *path=getenv("GOW3_MOUSE_DISCOVER_FILE");
            out=fopen(path && *path ? path : "mouse_discover.bin","wb");
        }
        const int wrote=discover_dump(out,f,rdi,now,follow_off_now);
        if (discover_dump(out,f|0x40,rsi,now,follow_off_now) || wrote) {
            fflush(out);
            discover[f].last_dump_us=now;
            if (++discover[f].dumps==(edge ? 1500u : 800u)) printf("Mouse discover: function %c dumps done\n",discover_label(f));
        }
    }
    pthread_mutex_unlock(&discover_lock);
}
#define DISCOVER_CALLBACK(name,f) static void __attribute__((ms_abi)) name(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t r14) { (void)image; (void)r14; discover_call(f,rdi,rsi); }
DISCOVER_CALLBACK(discover_a,0) DISCOVER_CALLBACK(discover_b,1) DISCOVER_CALLBACK(discover_c,2) DISCOVER_CALLBACK(discover_d,3)
DISCOVER_CALLBACK(discover_e,4) DISCOVER_CALLBACK(discover_f,5) DISCOVER_CALLBACK(discover_g,6) DISCOVER_CALLBACK(discover_h,7)
DISCOVER_CALLBACK(discover_i,8) DISCOVER_CALLBACK(discover_j,9) DISCOVER_CALLBACK(discover_k,10) DISCOVER_CALLBACK(discover_l,11)
DISCOVER_CALLBACK(discover_m,12) DISCOVER_CALLBACK(discover_n,13) DISCOVER_CALLBACK(discover_o,14) DISCOVER_CALLBACK(discover_p,15)
DISCOVER_CALLBACK(discover_q,16) DISCOVER_CALLBACK(discover_r,17) DISCOVER_CALLBACK(discover_s,18)
DISCOVER_CALLBACK(discover_t,19) DISCOVER_CALLBACK(discover_u,20) DISCOVER_CALLBACK(discover_v,21)
DISCOVER_CALLBACK(discover_w,22) DISCOVER_CALLBACK(discover_x,23) DISCOVER_CALLBACK(discover_y,24) DISCOVER_CALLBACK(discover_z,25)
DISCOVER_CALLBACK(discover_1,26) DISCOVER_CALLBACK(discover_2,27) DISCOVER_CALLBACK(discover_3,28)
DISCOVER_CALLBACK(discover_4,29) DISCOVER_CALLBACK(discover_5,30) DISCOVER_CALLBACK(discover_6,31)
DISCOVER_CALLBACK(discover_7,32) DISCOVER_CALLBACK(discover_9,34)
/* 8 (CameraControlStick, a modifier other cameras call through their vtables): also counts its callers'
 * return addresses (image offsets) per second, apart while L2 is held with the follow, aim and
 * climbing aim hooks silent, and logs them with the image-pointing qwords just below its rdi. */
static void __attribute__((ms_abi)) discover_8(uint64_t rdi, uint64_t rsi, uint64_t image, uint64_t ret) {
    discover_call(33,rdi,rsi);
    /* the caller's callers: the guest keeps rbp frames (push rbp; mov rbp, rsp), [rbp+8] their returns */
    uint64_t chain[5]={0};
    uint64_t frame=runtime_guest_rbp;
    for (int d=0;d<5 && frame>=0x10000 && !(frame&7);++d) {
#ifdef _WIN32
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)(uintptr_t)frame,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS|PAGE_GUARD)) || frame+16>(uint64_t)(uintptr_t)info.BaseAddress+info.RegionSize) break;
#endif
        const uint64_t r=((const uint64_t *)(uintptr_t)frame)[1], next=((const uint64_t *)(uintptr_t)frame)[0];
        if (r<image || r>=image+0x4000000) break;
        chain[d]=r-image;
        if (next<=frame) break;
        frame=next;
    }
    static struct { uint64_t at, calls, slow, rdi, chain[5]; } seen[16];
    static uint64_t second_us;
    const uint64_t now=now_us();
    const int slow=aim_held && !(camera_hook_us && now-camera_hook_us<100000) && !(aim_hook_us && now-aim_hook_us<100000) &&
                   !(edge_hook_us && now-edge_hook_us<100000);
    pthread_mutex_lock(&discover_lock);
    const uint64_t at=ret-image;
    int i=0;
    while (i<16 && seen[i].at && (seen[i].at!=at || memcmp(seen[i].chain,chain,sizeof(chain)))) ++i;
    if (i<16) { seen[i].at=at; ++seen[i].calls; seen[i].slow+=slow; seen[i].rdi=rdi; memcpy(seen[i].chain,chain,sizeof(chain)); }
    if (!second_us) second_us=now;
    if (now-second_us>=1000000) {
        second_us=now;
        for (int j=0;j<16 && seen[j].at;++j) {
            if (!seen[j].calls) continue;
            char line[512];
            int n=snprintf(line,sizeof(line),"Mouse discover: 8 return img %#llx calls %llu slow %llu rdi %#llx",(unsigned long long)seen[j].at,
                           (unsigned long long)seen[j].calls,(unsigned long long)seen[j].slow,(unsigned long long)seen[j].rdi);
            n+=snprintf(line+n,sizeof(line)-n," chain");
            for (int d=0;d<5 && seen[j].chain[d];++d) n+=snprintf(line+n,sizeof(line)-n," %#llx",(unsigned long long)seen[j].chain[d]);
            uint64_t r=seen[j].rdi;
#ifdef _WIN32
            MEMORY_BASIC_INFORMATION info; /* only below rdi within its mapped region */
            if (r<0x10200 || !VirtualQuery((void *)(uintptr_t)(r-0x200),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
                (info.Protect & (PAGE_NOACCESS|PAGE_GUARD)) || (uint64_t)(uintptr_t)info.BaseAddress+info.RegionSize<r) r=0;
#endif
            for (uint64_t o=0x10;o<=0x200 && r>=0x10000+o && n<(int)sizeof(line)-40;o+=8) {
                const uint64_t q=*(const uint64_t *)(uintptr_t)(r-o);
                if (q>=image && q<image+0x4000000) n+=snprintf(line+n,sizeof(line)-n," [-%#llx]=img %#llx",(unsigned long long)o,(unsigned long long)(q-image));
            }
            puts(line);
            seen[j].calls=seen[j].slow=0;
        }
    }
    pthread_mutex_unlock(&discover_lock);
}
static const RuntimeGuestCallback discover_callbacks[]={
    {"U3 zoom camera A (discover, file 0x5161c0)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec b8 02 00 00 49 89 d5 49 89 f7 "
     "49 89 fc 48 8d 05 ?? ?? ?? ?? 48 8b 00 48",6,discover_a,NULL,0,0},
    {"U3 zoom camera B (discover, file 0x5197e0)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec c8 09 00 00 45 89 c6 48 89 b5 "
     "90 f6 ff ff 49 89 ff 4c 8d 25 ?? ?? ?? ??",6,discover_b,NULL,0,0},
    {"U3 zoom camera C (discover, file 0x515cc0)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec c8 00 00 00 49 89 f6 48 89 fb "
     "4c 8d 25 ?? ?? ?? ?? 49 8b 04 24",6,discover_c,NULL,0,0},
    {"U3 cover camera D (discover, file 0x4d9bd0)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 38 02 00 00 48 89 d3 49 89 f6 "
     "49 89 ff 48 8d 05 ?? ?? ?? ?? 48 8b 00 48",6,discover_d,NULL,0,0},
    {"U3 cover camera E (discover, file 0x4d85a0)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 58 49 89 d7 48 89 f3 49 89 fe "
     "4c 8d 25 ?? ?? ?? ?? 49 8b 04 24 48 89 45",6,discover_e,NULL,0,0},
    /* U1 aiming (camerazoom.cpp), eboot.bin 1.02 file offsets */
    {"U1 zoom camera F (discover, file 0xc6b30)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 08 09 00 00 89 95 0c f7 ff ff "
     "49 89 f7 49 89 fc 4c ?? 35",6,discover_f,NULL,0,0},
    {"U1 zoom camera G (discover, file 0xc95c0)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 68 02 00 00 49 89 d7 48 89 f3 "
     "49 89 fe 4c ?? 2d",6,discover_g,NULL,0,0},
    {"U1 zoom camera H (discover, file 0xc6140)","55 48 89 e5 41 57 41 56 53 48 83 ec 68 48 89 fb 4c ?? 3d ?? ?? ?? ?? "
     "49 8b 07 48 89 45 e0 48 c7 83 80 18 00",6,discover_h,NULL,0,0},
    {"U1 zoom camera I (discover, file 0xc2420)","55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 70 45 89 c7 48 89 d3 49 89 fe "
     "4c ?? 25 ?? ?? ?? ?? 49 8b 04 24 48",6,discover_i,NULL,0,0},
    /* U2 cover (cameracover.cpp and neighbours), big2 1.02 file offsets */
    {"U2 cover camera J (discover, file 0x41fc80)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 68 01 00 00 49 89 d7 48 89 f3 "
     "49 89 fe 4c ?? 2d",6,discover_j,NULL,0,0},
    {"U2 cover camera K (discover, file 0x41c150)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 e4 e0 ff ff ff 48 81 ec 80 02 00 00 "
     "49 89 f7 48 89 fb 4c ?? 2d",6,discover_k,NULL,0,0},
    {"U2 cover camera L (discover, file 0x418540)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 28 01 00 00 49 89 d7 48 89 f3 "
     "49 89 fe 48 ?? 05 ?? ?? ?? ?? 48 8b 00 48 89 45 d0 bf",6,discover_l,NULL,0,0},
    {"U2 cover camera M (discover, file 0x4198d0)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 e4 e0 ff ff ff 48 81 ec a0 02 00 00 "
     "48 89 f3 49 89 fe 48 ?? 05",6,discover_m,NULL,0,0},
    /* U2 aiming (camerazoom.cpp), big2 1.02 file offsets */
    {"U2 zoom camera N (discover, file 0x455020)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 98 01 00 00 48 89 f3 49 89 ff "
     "4c ?? 25 ?? ?? ?? ?? 49 8b 04",6,discover_n,NULL,0,0},
    {"U2 zoom camera O (discover, file 0x4579b0)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec e8 08 00 00 41 89 d7 48 89 fb "
     "4c ?? 35 ?? ?? ?? ?? 49 8b 06",6,discover_o,NULL,0,0},
    {"U2 zoom camera P (discover, file 0x459670)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec d8 01 00 00 49 89 f6 49 89 ff "
     "4c ?? 2d ?? ?? ?? ?? 49 8b 45",6,discover_p,NULL,0,0},
    {"U2 zoom camera Q (discover, file 0x45b480)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 98 01 00 00 49 89 d6 49 89 f7 "
     "48 89 fb 4c ?? 2d",6,discover_q,NULL,0,0},
    {"U2 zoom camera R (discover, file 0x456800)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec c8 00 00 00 48 89 f3 49 89 fe "
     "4c ?? 25 ?? ?? ?? ?? 49 8b 04 24 48 89 45 d0 c5 f8 57",6,discover_r,NULL,0,0},
    {"U2 zoom camera S (discover, file 0x45d1c0)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 78 01 00 00 48 89 cb 89 95 "
     "dc fe ff ff 41 89 f4 49 89 ff 4c",6,discover_s,NULL,0,0},
    /* 1.00 aiming candidates (base game file offsets) */
    {"U3 zoom camera T (discover, 1.00 file 0x13fa60)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 49 89 f6 48 89 fb "
     "4c ?? 2d ?? ?? ?? ?? 49 8b 45 00 48 89 45 d0 e8 25",6,discover_t,NULL,0,0},
    {"U2 zoom camera U (discover, 1.00 file 0x194270)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 38 01 00 00 49 89 f6 49 89 ff "
     "4c ?? 2d ?? ?? ?? ?? 49 8b 45 00 48 89 45 d0 e8 92 1b",6,discover_u,NULL,0,0},
    {"U2 zoom camera V (discover, 1.00 file 0x1947c0)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 88 02 00 00 48 89 d3 49 89 f7 "
     "49 89 fe 4c ?? 2d",6,discover_v,NULL,0,0},
    /* U1 cover (CameraControlCover), eboot.bin file offsets 1.02 / 1.00; each pattern fits both */
    {"U1 cover camera W (discover, file 0xa9880 / 0xa9980)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 88 01 00 00 49 89 d7 48 89 f3 "
     "49 89 fe 4c ?? 25 ?? ?? ?? ?? 49 8b 04 24",6,discover_w,NULL,0,0},
    {"U1 cover camera X (discover, file 0xa9150 / 0xa9250)","55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 30 49 89 f6 48 89 fb "
     "4c ?? 3d ?? ?? ?? ?? 49 8b 07 48 89 45 d8 49",6,discover_x,NULL,0,0},
    {"U1 cover camera Y (discover, file 0xa7f60 / 0xa8070)","55 48 89 e5 41 57 41 56 53 48 81 ec 88 00 00 00 48 89 fb "
     "4c ?? 3d ?? ?? ?? ?? 49 8b 07 48 89 45 e0 89 93 e0",6,discover_y,NULL,0,0},
    {"U1 cover camera Z (discover, file 0xaa910 / 0xaaa00)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 58 01 00 00 49 89 d7 49 89 f4 "
     "49 89 fe 48 ?? 05 ?? ?? ?? ?? 48 8b 00 48",6,discover_z,NULL,0,0},
    /* UC3 1.02 climbing aim (big3 1.02 file offsets): CameraEdgeAim, the player's edge aim, CameraControlEdge */
    {"U3 edge aim camera 1 (discover, file 0x4dbf00)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 18 02 00 00 48 89 d3 49 89 f6 "
     "49 89 fc 48 8d 15 ?? ?? ?? ?? 48 8b 02 48 89 45 d0 49 8b 86 b0 00",6,discover_1,NULL,0,0},
    {"U3 MonkeyBars 2 (discover, file 0x50a000)","55 48 89 e5 41 57 41 56 53 48 83 ec 78 49 89 f6 48 89 fb 48 8b 83 b0 00 00 00 "
     "48 8b 00 8b 48 7c 3b 8b b8 00 00 00 74 02 31 c0",6,discover_2,NULL,0,0},
    {"U3 MonkeyBars 3 (discover, file 0x509cf0)","55 48 89 e5 41 57 41 56 41 54 53 48 83 ec 10 48 89 d3 49 89 f7 49 89 fe "
     "4c 8d 25 ?? ?? ?? ?? 49 8b 04 24 48 89 45 d8 48 8d 7d d0 e8",6,discover_3,NULL,0,0},
    {"U3 FreeRope 4 (discover, file 0x4f93f0)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 68 48 89 f3 49 89 fe "
     "4c 8d 3d ?? ?? ?? ?? 49 8b 07 48 89 45 d0 48 8b 83 b0 00 00 00",6,discover_4,NULL,0,0},
    {"U3 FreeRope 5 (discover, file 0x4f64e0)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 38 02 00 00 48 89 fb "
     "4c 8d 25 ?? ?? ?? ?? 49 8b 04 24 48 89 45 d0 c5 f8 28 06 c5 f8",6,discover_5,NULL,0,0},
    {"U3 Spider 6 (discover, file 0x510550)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 e4 e0 ff ff ff 48 81 ec 60 01 00 00 "
     "4d 89 c7 49 89 ce 48 89 54 24 38 49 89 f5 48 89 7c",6,discover_6,NULL,0,0},
    {"U3 CameraLookAround 7 (discover, file 0x4fd350)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 e4 e0 ff ff ff 48 81 ec 60 02 00 00 "
     "48 89 d3 49 89 f7 49 89 fe 48 8d 05 ?? ?? ?? ?? 48",6,discover_7,NULL,0,0},
    {"U3 CameraControlStick 8 (discover, file 0x4fa880)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 78 48 89 cb 49 89 ff "
     "48 8d 3d ?? ?? ?? ?? 48 8b 07 48 89 45 d0 c5 f8 28 03 c4 c1 78",6,discover_8,NULL,0,2},
    {"U3 edge camera 9 (discover, file 0x4ddc10)","55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 ec 08 01 00 00 48 89 f3 "
     "48 8d 05 ?? ?? ?? ?? 48 8b 00 48 89 45 d0 48 8b 42 10 48 89 45 80 48 8b 02",6,discover_9,NULL,0,0},
};
/* GOW3_MOUSE_CAMERA_LOG=1 (diagnostics, UC3 1.02): which camera controls run. The blend tree (big3
 * file 0x10246f0, recursive) calls each active control's UpdateLocation through vtable slot +0x150 with
 * the control in r14; hooked at the two loads (file 0x102498b, read by Opus). Logs each update function
 * the first time it runs, then once a second every one that ran with its calls, how many of them came
 * while L2 was held with the follow, aim and climbing aim hooks silent ("slow"), and whether the mouse
 * has a hook for it. */
static void __attribute__((ms_abi)) u3_camera_census(uint64_t unused0, uint64_t unused1, uint64_t image, uint64_t control) {
    (void)unused0; (void)unused1;
    if (control<0x10000) return;
    const uint64_t vt=*(const uint64_t *)(uintptr_t)control;
    if (vt<image || vt>=image+0x4000000) return;
    const uint64_t fn=*(const uint64_t *)(uintptr_t)(vt+0x150);
    static struct { uint64_t fn, vt, calls, slow; } seen[32];
    static uint64_t second_us;
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    const uint64_t now=now_us();
    const int slow=aim_held && !(camera_hook_us && now-camera_hook_us<100000) && !(aim_hook_us && now-aim_hook_us<100000) &&
                   !(edge_hook_us && now-edge_hook_us<100000);
    /* the update functions the mouse hooks (image offsets in 1.02): follow, cover, climbing aim */
    static const uint64_t hooked[]={0x4e7880,0x4e7970,0x4ce080,0x4d03b0,0x4e8e40,0x4d4510,0x513890}; /* follow (0x4e8e40 calls it), cover, climbing aim, edge (calls follow), zoom update */
    pthread_mutex_lock(&lock);
    int i=0;
    while (i<32 && seen[i].fn && seen[i].fn!=fn-image) ++i;
    if (i<32) {
        if (!seen[i].fn) {
            seen[i].fn=fn-image; seen[i].vt=vt-image;
            int mouse=0;
            for (size_t h=0;h<sizeof(hooked)/sizeof(*hooked);++h) mouse|=seen[i].fn==hooked[h];
            printf("Mouse cameras: new update img %#llx (file %#llx) vtable img %#llx%s%s\n",(unsigned long long)seen[i].fn,
                   (unsigned long long)(seen[i].fn+0xbb50),(unsigned long long)seen[i].vt,mouse ? " mouse" : " NO MOUSE",slow ? " (slow)" : "");
        }
        ++seen[i].calls; seen[i].slow+=slow;
    }
    if (!second_us) second_us=now;
    if (now-second_us>=1000000) {
        second_us=now;
        char line[1024];
        int n=snprintf(line,sizeof(line),"Mouse cameras: aim=%d",aim_held);
        for (int j=0;j<32 && seen[j].fn && n<(int)sizeof(line)-64;++j) {
            if (!seen[j].calls) continue;
            n+=snprintf(line+n,sizeof(line)-n,"  %#llx %llu/%llu",(unsigned long long)seen[j].fn,(unsigned long long)seen[j].calls,(unsigned long long)seen[j].slow);
            seen[j].calls=seen[j].slow=0;
        }
        puts(line);
    }
    pthread_mutex_unlock(&lock);
}
static const RuntimeGuestCallback census_callback=
    {"U3 camera census (1.02, file 0x102498b)","49 8b 06 48 8b 80 50 01 00 00 c4 c1 7a 10 a6 08 01 00 00 c4 c1 7a 10 ae 0c 01 00 00",
     10,u3_camera_census,NULL,0,0};
const RuntimeGuestCallback *runtime_guest_callbacks(size_t *count) {
    static RuntimeGuestCallback all[sizeof(guest_callbacks)/sizeof(*guest_callbacks)+sizeof(discover_callbacks)/sizeof(*discover_callbacks)+1];
    size_t n=0;
    for (size_t i=0;i<sizeof(guest_callbacks)/sizeof(*guest_callbacks);++i)
        all[n++]=guest_callbacks[i];
    const char *on=getenv("GOW3_MOUSE_DISCOVER");
    if (on && on[0]=='1')
        for (size_t i=0;i<sizeof(discover_callbacks)/sizeof(*discover_callbacks);++i) all[n++]=discover_callbacks[i];
    const char *census=getenv("GOW3_MOUSE_CAMERA_LOG");
    if (census && census[0]=='1') all[n++]=census_callback;
    *count=n;
    return all;
}
/* The port menu (gow3_overlay.cpp): reads and changes input.ini lines. A change is written to the
 * file at once (the launcher takes it from there) and applies at the next pad read. */
int runtime_input_get(const char *key, char *value, size_t size) {
    int found=0;
    pthread_mutex_lock(&input_lock);
    for (size_t i=0;i<input_count && !found;++i) {
        if (strcmp(input_lines[i].key,key)) continue;
        snprintf(value,size,"%s",input_lines[i].value);
        found=1;
    }
    pthread_mutex_unlock(&input_lock);
    return found;
}
int runtime_input_set(const char *key, const char *value) {
    const char *path=getenv("GOW3_INPUT_CONFIG");
    if (!path || !*path) return 0;
    pthread_mutex_lock(&input_lock);
    size_t i=0;
    while (i<input_count && strcmp(input_lines[i].key,key)) ++i;
    if (i==input_count && input_count<sizeof(input_lines)/sizeof(*input_lines)) {
        snprintf(input_lines[i].key,sizeof(input_lines[0].key),"%s",key);
        ++input_count;
    }
    if (i<input_count) snprintf(input_lines[i].value,sizeof(input_lines[0].value),"%s",value);
    FILE *f=fopen(path,"w");
    if (f) {
        fputs("# Uncharted controls (written by the launcher's Controls window or the port menu)\n",f);
        for (size_t j=0;j<input_count;++j) fprintf(f,"%s=%s\n",input_lines[j].key,input_lines[j].value);
        fclose(f);
    }
    pthread_mutex_unlock(&input_lock);
    input_reload=1;
    return f!=NULL;
}
int runtime_input_available(void) {
    const char *path=getenv("GOW3_INPUT_CONFIG");
    return path && *path && input_count>0;
}
int runtime_pad_mouse_camera(void) {
    const char *off=getenv("GOW3_MOUSE_CAMERA");
    if (off && off[0]=='0') return 0;
    pthread_mutex_lock(&lock);
    load_button_maps();
    pthread_mutex_unlock(&lock);
    return mouse_cfg.look;
}

static void sample_host(PadData *d) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    d->timestamp=now_us();
    SDL_Gamepad *g=current_gamepad();
    load_button_maps();
    float mdx=0, mdy=0;
    uint32_t mouse=0;
    int wheel=0;
    const int captured=gow3gpu_mouse_sample(&mdx,&mdy,&mouse,&wheel); /* taken every call: no stale motion later */
    if (gow3gpu_overlay_captures_input()) return; /* settings menu open: neutral input */
    const bool *k=SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    const uint64_t now=d->timestamp;
    if (wheel>0) wheel_until_us[0]=now+80000;
    if (wheel<0) wheel_until_us[1]=now+80000;
    if (g) {
        /* A trigger mapped to another button (pad.r1=lefttrigger) no longer presses L2/R2; mapped to
         * its own (pad.l2=lefttrigger, as the launcher writes) it stays analog. */
        bool trigger_mapped[2]={false,false};
        uint8_t triggers[2]={trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)),
                             trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))};
        /* Menu shortcut L2+R2+L3+R3 (gow3_overlay.cpp): with both sticks clicked the triggers stay
         * out of the game, so forming the shortcut does not aim or fire. */
        if (SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_LEFT_STICK) &&
            SDL_GetGamepadButton(g,SDL_GAMEPAD_BUTTON_RIGHT_STICK)) triggers[0]=triggers[1]=0;
        for (size_t i=0;i<pad_map.count;++i) {
            const int source=pad_map.entries[i].source;
            if (source==SRC_TRIGGER_LEFT || source==SRC_TRIGGER_RIGHT) {
                const int t=source-SRC_TRIGGER_LEFT;
                if (pad_map.entries[i].ps==(t ? BTN_R2 : BTN_L2)) continue;
                trigger_mapped[t]=true;
                if (triggers[t]>30) d->buttons|=pad_map.entries[i].ps;
            } else if (SDL_GetGamepadButton(g,(SDL_GamepadButton)source)) d->buttons|=pad_map.entries[i].ps;
        }
        const uint32_t mapped=d->buttons;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        if (mouse_cfg.pad_invert_y) d->right_y=(uint8_t)(255-d->right_y);
        if (!trigger_mapped[0]) d->l2=triggers[0];
        if (!trigger_mapped[1]) d->r2=triggers[1];
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (mapped & BTN_L2) d->l2=255;
        if (mapped & BTN_R2) d->r2=255;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
    }
    /* The keyboard and mouse work with or without a gamepad: their buttons add to the pad's, and
     * a stick follows the keys only while one of them is held. */
    for (size_t i=0;i<key_map.count;++i) if (source_down(key_map.entries[i].source,k,mouse,now)) d->buttons|=key_map.entries[i].ps;
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    bool held[STICK_KEYS];
    for (int i=0;i<STICK_KEYS;++i) held[i]=stick_down(i,k,mouse,now);
    key_axis(&d->left_x,held[STICK_LS_LEFT],held[STICK_LS_RIGHT]);
    key_axis(&d->left_y,held[STICK_LS_UP],held[STICK_LS_DOWN]);
    key_axis(&d->right_x,held[STICK_RS_LEFT],held[STICK_RS_RIGHT]);
    key_axis(&d->right_y,held[STICK_RS_UP],held[STICK_RS_DOWN]);
    /* Where the game's camera or joypad state takes the mouse directly (their callbacks ran in the
     * last 100 ms), the motion goes there. U2 and U3 have no joypad callback: the follow camera turns
     * with the mouse, with the right stick held a little the same way (follow_push); aiming, other
     * cameras and a follow camera that keeps no turns get the mouse as a right stick here. */
    pthread_mutex_lock(&camera_lock);
    const int following=camera_hook_us && now-camera_hook_us<100000;
    const int joypad=sticks_hook_us && now-sticks_hook_us<100000;
    const int aiming=aim_hook_us && now-aim_hook_us<100000;
    const int covering=!cover_off && cover_hook_us && now-cover_hook_us<100000 && !following;
    const int edging=edge_hook_us && now-edge_hook_us<100000;
    for (int q=0;q<5;++q) {
        if (captured && (q==2 ? aiming : q==3 ? covering : q==4 ? edging : following || joypad)) { camera_dx[q]+=mdx; camera_dy[q]+=mdy; }
        else { camera_dx[q]=camera_dy[q]=0; }
    }
    pthread_mutex_unlock(&camera_lock);
    /* GOW3_MOUSE_DEBUG: the mouse is a stick while aiming (aim camera hook silent or keeping no
     * turns), with the age of each camera hook; at most once a second. */
    if (mouse_debug() && captured && aim_held && !(aiming && aim_direct) && !(edging && edge_direct)) {
        static uint64_t stick_log_us;
        if (!stick_log_us || now-stick_log_us>=1000000) {
            stick_log_us=now;
            const long long ms_follow=camera_hook_us ? (long long)(now-camera_hook_us)/1000 : -1;
            const long long ms_aim=aim_hook_us ? (long long)(now-aim_hook_us)/1000 : -1;
            const long long ms_cover=cover_hook_us ? (long long)(now-cover_hook_us)/1000 : -1;
            const long long ms_edge=edge_hook_us ? (long long)(now-edge_hook_us)/1000 : -1;
            printf("Mouse: aiming with the stick: hooks follow %lld ms, aim %lld ms (direct %d, bad %d, off %d), "
                   "cover %lld ms, climbing aim %lld ms (direct %d) ago\n",ms_follow,ms_aim,(int)aim_direct,aim_bad,aim_off,ms_cover,
                   ms_edge,(int)edge_direct);
        }
    }
    const int aim_turns=(aim_held && aiming && aim_direct) || (!aim_held && covering && cover_direct) || (edging && edge_direct);
    const int direct_x=joypad || aim_turns || (following && !aim_held && follow_direct);
    const int direct_y=joypad || aim_turns || (following && !aim_held && follow_direct && follow_has_arc);
    /* Mouse look as a stick: the mouse's speed, smoothed over 12 ms (reports arrive in bursts), as
     * a turn rate on the right stick, unless a right stick key is held. */
    if (!captured || (direct_x && direct_y)) { mouse_vx=mouse_vy=0; mouse_last_us=0; return; }
    float dt=mouse_last_us ? (float)(now-mouse_last_us)*1e-6f : 0.0f;
    mouse_last_us=now;
    if (dt>0.0f) {
        if (dt>0.1f) dt=0.1f;
        const float alpha=1.0f-SDL_expf(-dt/0.012f), scale=MOUSE_DEGREES_PER_PIXEL*mouse_sensitivity();
        mouse_vx+=alpha*(mdx/dt*scale-mouse_vx);
        mouse_vy+=alpha*((mouse_cfg.invert_y ? -mdy : mdy)/dt*scale-mouse_vy);
        if (mdx==0 && mouse_vx>-1.0f && mouse_vx<1.0f) mouse_vx=0;
        if (mdy==0 && mouse_vy>-1.0f && mouse_vy<1.0f) mouse_vy=0;
    }
    if (mouse_vx!=0 && !held[STICK_RS_LEFT] && !held[STICK_RS_RIGHT] && !direct_x) d->right_x=mouse_axis(mouse_vx);
    if (mouse_vy!=0 && !held[STICK_RS_UP] && !held[STICK_RS_DOWN] && !direct_y) d->right_y=mouse_axis(mouse_vy);
}

/* GOW3_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad touchpad_left touchpad_right up down left right) are held while listed;
 * touchpad is the left side; lx= ly= rx= ry= (0..255) override the sticks. An empty file
 * releases everything. */
static struct { uint32_t buttons; int stick[4]; } injected={0,{-1,-1,-1,-1}};
static int replay_armed;      /* 1 while a GOW3_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("GOW3_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    injected.buttons=0;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* GOW3_PAD_REPLAY */
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!strcmp(token,ps_names[i].name)) injected.buttons|=ps_names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
    }
    fclose(f);
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3]);
}
/* GOW3_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; GOW3_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in GOW3_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("GOW3_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("GOW3_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}
/* After the menu or the text dialog closes, buttons still held (the Cross that accepted a
 * name) stay hidden until released: the game would take them as a new press. */
static int hold_after_capture;
static void sample(PadData *d) {
    sample_host(d);
    if (gow3gpu_overlay_captures_input()) { hold_after_capture=1; click.side=-1; return; }
    record_sample(d); /* the click sources as they are: a replay plays them again */
    read_inject();
    replay_sample(d);
    d->buttons|=injected.buttons;
    if (injected.buttons & BTN_L2) d->l2=255;
    if (injected.buttons & BTN_R2) d->r2=255;
    uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
    for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    aim_held=(d->buttons & BTN_L2)!=0;
    /* U2, U3 (no joypad callback): the right stick held a little while the mouse turns the follow
     * camera's direction (follow_push, see follow_camera), unless the stick is in use */
    const uint64_t now=now_us();
    if (follow_push!=0.0f && camera_hook_us && now-camera_hook_us<100000 &&
        !(sticks_hook_us && now-sticks_hook_us<100000) && !aim_held && d->right_x>=118 && d->right_x<=138)
        d->right_x=(uint8_t)(128.0f+follow_push*127.5f);
    last_right[0]=d->right_x; last_right[1]=d->right_y;
    play_click(d);
    if (hold_after_capture) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture=0;
    }
}

static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user!=1) return ERR_INVALID_ARG;
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    int already=opened; opened=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    puts("Runtime: pad opened for user 1 (SDL gamepad or keyboard)");
    return PAD_HANDLE;
}
static ABI int32_t pad_close(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    opened=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    sample(data); ++reads;
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    pthread_mutex_lock(&lock);
    current_gamepad();
    info->connected_count=connected_count ? connected_count : 1;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    if (g) SDL_RumbleGamepad(g,(uint16_t)(param[0]*257),(uint16_t)(param[1]*257),1000);
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return handle==PAD_HANDLE && opened ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    (void)rgb; return 0;
}
static const RuntimeExport exports[]={
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_ok_handle},
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) { printf("Runtime: pad reads=%zu, gamepad=%s\n",reads,gamepad ? SDL_GetGamepadName(gamepad) : "none"); }
