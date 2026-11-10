/* Standalone Windows integration checks against the runtime sources.
 * SDL virtual pads are process-local; synthetic outputs use API spies. */
#include "libs/input/cellPad.c"


#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

/* Unused guest API entry points in these translation units still need their
 * link dependencies. Fail if a test accidentally enters guest-memory code. */
uint8_t* vm_base;
uint32_t ppu_vm_size, g_ww_lo, g_ww_hi;
uint32_t cellGcm_flip_request_count(void) { return 0; }
int g_resv_store_active;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { CHECK(0); }
/* The legacy GT5 VM consults reservations even for plain synthetic writes. */
int spu_coh_is_reserved(uint32_t a) { CHECK(a >= 0x10000 && a < 0x20000); return 0; }
void spu_lockline_lock(void) { CHECK(0); }
void spu_lockline_unlock(void) { CHECK(0); }
void spu_coh_notify_write(uint32_t a) { CHECK(0); }
void ppu_resv_break_store(uint64_t a) { CHECK(0); }
uint32_t sys_event_queue_create_direct(uint64_t k, int32_t n) { CHECK(0); return 0; }
uint32_t sys_event_find_queue_by_key(uint64_t k) { CHECK(0); return 0; }
int sys_event_queue_push_by_id(uint32_t q, uint64_t s, uint64_t a, uint64_t b, uint64_t c)
{ CHECK(0); return 0; }

static int (*attach_virtual)(int, int, int, int);
static int (*detach_virtual)(int);
static void* (*joystick_open)(int);
static void (*joystick_close)(void*);
static int (*set_button)(void*, int, unsigned char);
static int (*set_axis)(void*, int, short);

static void load_virtual_api(void)
{
#define LOAD(dst, name) *(FARPROC*)&dst = GetProcAddress(s_sdl.dll, name); CHECK(dst)
    LOAD(attach_virtual, "SDL_JoystickAttachVirtual");
    LOAD(detach_virtual, "SDL_JoystickDetachVirtual");
    LOAD(joystick_open, "SDL_JoystickOpen");
    LOAD(joystick_close, "SDL_JoystickClose");
    LOAD(set_button, "SDL_JoystickSetVirtualButton");
    LOAD(set_axis, "SDL_JoystickSetVirtualAxis");
#undef LOAD
}

static int virtual_port(int id)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++)
        if (s_sdl_pad[i] && s_sdl.JoystickInstanceID(
            s_sdl.GameControllerGetJoystick(s_sdl_pad[i])) == id) return i;
    return -1;
}

static void test_pad_ports(void)
{
    CHECK(pad_sdl_dyn_init());
    load_virtual_api();
    pad_poll_sdl_dyn();
    int index = attach_virtual(1 /* GAMECONTROLLER */, 6, 15, 0);
    CHECK(index >= 0);
    int id = s_sdl.JoystickGetDeviceInstanceID(index);
    void* stick = joystick_open(index);
    CHECK(stick);
    Sleep(1050);
    pad_poll_sdl_dyn();
    int port = virtual_port(id);
    CHECK(port >= 0); /* Need at least one free port on this machine. */
    CHECK(set_button(stick, 0, 1) == 0);
    CHECK(set_axis(stick, 0, 32767) == 0);
    pad_poll_sdl_dyn();
    CHECK(s_sdl_state[port].buttons & CELL_PAD_CTRL_CROSS);
    CHECK(s_sdl_state[port].analog_lx == 255);
    CHECK(detach_virtual(index) == 0);
    joystick_close(stick);
    pad_poll_sdl_dyn();
    CHECK(!s_sdl_state[port].connected);
    CHECK(!s_sdl_state[port].buttons && s_sdl_state[port].analog_lx == 128);
    /* Reconnect, then replace an attached handle without an intervening poll.
     * The old scan-before-close order assigned the replacement to a new port. */
    for (int cycle = 0; cycle < 5; cycle++) {
        index = attach_virtual(1, 6, 15, 0);
        CHECK(index >= 0);
        id = s_sdl.JoystickGetDeviceInstanceID(index);
        Sleep(1050);
        pad_poll_sdl_dyn();
        CHECK(virtual_port(id) == port);
        CHECK(detach_virtual(index) == 0);
    }
    pad_poll_sdl_dyn();
    CHECK(!s_sdl_state[port].connected);
    puts("PASS: SDL input, neutral disconnect, five replacements keep their port");
}

static int snapshot_has_cross(void)
{
    CHECK(pad_sdl_dyn_read());
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected && (s_host_state[i].buttons & CELL_PAD_CTRL_CROSS)) return 1;
    return 0;
}

static void test_pad_worker(void)
{
    CHECK(pad_sdl_dyn_read());
    load_virtual_api();
    for (int cycle = 0; cycle < 3; cycle++) {
        int index = attach_virtual(1, 6, 15, 0);
        CHECK(index >= 0);
        void* stick = joystick_open(index);
        CHECK(stick && set_button(stick, 0, 1) == 0);
        ULONGLONG deadline = GetTickCount64() + 2500;
        while (!snapshot_has_cross() && GetTickCount64() < deadline) Sleep(5);
        CHECK(snapshot_has_cross());
        CHECK(detach_virtual(index) == 0);
        joystick_close(stick);
        deadline = GetTickCount64() + 2500;
        while (snapshot_has_cross() && GetTickCount64() < deadline) Sleep(5);
        CHECK(!snapshot_has_cross());
    }
    puts("PASS: SDL worker discovers three virtual reconnects and releases input");
}


static int body_calls, trigger_calls, effect_calls, fail_effect, fail_body;
static u32 got_ms;
static u16 got_left, got_right;
static u8 got_packet[47];
static int fake_body(void* p,u16 l,u16 r,u32 ms) { body_calls++; CHECK(ms==1000 || ms==250); got_ms=ms; got_left=l;got_right=r;return fail_body?-1:0; }
static int fake_triggers(void* p,u16 l,u16 r,u32 ms) { trigger_calls++; CHECK(ms==1000);return 0; }
static int fake_has(void* p) { return 1; }
static int fake_effect(void* p,const void* b,int n) { effect_calls++;CHECK(n==47);memcpy(got_packet,b,47);return fail_effect?-1:0; }
static void test_effects(void)
{
    s_sdl_pad[0]=(void*)1;s_sdl_type[0]=7;
    s_sdl.GameControllerRumble=fake_body;s_sdl.GameControllerRumbleTriggers=fake_triggers;
    s_sdl.GameControllerHasRumbleTriggers=fake_has;s_sdl.GameControllerSendEffect=fake_effect;
    s_effect_request[0]=0x12345678;
    pad_sdl_effects(0,1000,1);
    CHECK(body_calls==1 && trigger_calls==1 && effect_calls==1);
    CHECK(got_left==0x5678 && got_right==0x1234);
    CHECK(got_packet[0]==12 && got_packet[10]==1 && got_packet[12]==110 && got_packet[21]==1 && got_packet[23]==110);
    for(int j=1;j<47;j++) if(j!=10 && j!=12 && j!=21 && j!=23) CHECK(got_packet[j]==0);
    pad_sdl_effects(0,1399,1);CHECK(body_calls==1 && effect_calls==1);
    pad_sdl_effects(0,1400,1);CHECK(body_calls==2 && trigger_calls==2);
    pad_sdl_effects(0,1401,0);CHECK(!got_left && !got_right && !s_effect_request[0]);
    CHECK(got_packet[10]==5 && got_packet[21]==5 && effect_calls==2);
    pad_sdl_effects(0,1500,1);CHECK(effect_calls==3 && body_calls==3);
    pad_sdl_effects(0,1501,0);fail_effect=1;pad_sdl_effects(0,1502,1);
    int attempts=effect_calls;pad_sdl_effects(0,1600,1);CHECK(effect_calls==attempts);
    fail_effect=0;pad_sdl_effects(0,1601,0);CHECK(effect_calls==attempts+1 && got_packet[10]==5);
    s_sdl.GameControllerRumble=NULL;s_sdl.GameControllerRumbleTriggers=NULL;s_sdl.GameControllerSendEffect=NULL;
    pad_sdl_effects(0,2000,0);s_sdl_pad[0]=NULL;
    puts("PASS: effect packet, game motor levels, 400ms renewal, stop, resume, failure suppression, missing exports");
}
static void test_capability_size(void) {
    CHECK(sizeof(CellPadCapabilityInfo)==128);
    vm_base=(uint8_t*)malloc(0x20000);CHECK(vm_base);ppu_vm_size=0x20000;s_pad_initialized=1;
    for(unsigned p=0;p<CELL_PAD_MAX_PORT_NUM;p++) {
        memset(vm_base,0xA5,0x20000);
        CHECK(cellPadGetCapabilityInfo(p,(CellPadCapabilityInfo*)(uintptr_t)0x10000)==CELL_OK);
        CHECK(vm_read32(0x10000)&CELL_PAD_CAPABILITY_ACTUATOR);
        for(unsigned i=4;i<128;i++)CHECK(vm_base[0x10000+i]==0);
        for(unsigned i=0;i<0x20000;i++)if(i<0x10000 || i>=0x10080)CHECK(vm_base[i]==0xA5);
    }
    _putenv_s("PAD_CAPABILITY_INFO_FIX","0");memset(vm_base,0xA5,0x20000);
    CHECK(cellPadGetCapabilityInfo(0,(CellPadCapabilityInfo*)(uintptr_t)0x10000)==CELL_OK);
    for(unsigned i=128;i<256;i++)CHECK(vm_base[0x10000+i]==0); // Reproduce the previous overwrite.
    _putenv_s("PAD_CAPABILITY_INFO_FIX","");
    free(vm_base);vm_base=NULL;
    puts("PASS: 128-byte capability ABI for all seven ports, big-endian actuator flag, guard bytes intact; legacy rollback reproduces overwrite");
}
static int hint_calls;
static int fake_hint(const char* key,const char* value) {
    CHECK(!strcmp(value,"1"));
    CHECK(!strcmp(key,"SDL_JOYSTICK_HIDAPI_PS5_RUMBLE") || !strcmp(key,"SDL_JOYSTICK_HIDAPI_PS4_RUMBLE"));
    hint_calls++;return 1;
}
static void test_feedback_hints(void) {
    s_sdl.SetHint=fake_hint;
    _putenv_s("PAD_SDL_RUMBLE","");_putenv_s("PAD_ADAPTIVE_TRIGGERS","");
    pad_feedback_hints();CHECK(hint_calls==0);
    _putenv_s("PAD_SDL_RUMBLE","1");pad_feedback_hints();CHECK(hint_calls==2);
    _putenv_s("PAD_SDL_ENHANCED_REPORTS","0");pad_feedback_hints();CHECK(hint_calls==2);
    _putenv_s("PAD_SDL_ENHANCED_REPORTS","");_putenv_s("PAD_CONTROLS_POLISH","0");pad_feedback_hints();CHECK(hint_calls==2);
    _putenv_s("PAD_CONTROLS_POLISH","");_putenv_s("PAD_SDL_RUMBLE","");_putenv_s("PAD_ADAPTIVE_TRIGGERS","1");pad_feedback_hints();CHECK(hint_calls==4);
    puts("PASS: enhanced reports only with feedback opt-in; dedicated and master rollbacks");
}
static void test_guest_feedback(void) {
    vm_base=(uint8_t*)calloc(1,0x20000);CHECK(vm_base);ppu_vm_size=0x20000;
    s_pad_initialized=1;s_sdl_ok=1;s_sdl_pad[0]=(void*)1;s_sdl_type[0]=0;
    s_sdl.GameControllerRumble=fake_body;s_sdl.GameControllerRumbleTriggers=fake_triggers;
    s_sdl.GameControllerHasRumbleTriggers=fake_has;
    vm_base[0x10000]=1;vm_base[0x10001]=128;
    CHECK(cellPadSetActDirect(0,(CellPadActParam*)(uintptr_t)0x10000)==CELL_OK);
    CHECK(s_effect_request[0]==0xffff8080u);
    pad_sdl_effects(0,1000,1);CHECK(got_left==32896 && got_right==65535 && body_calls==1);
    pad_sdl_effects(0,1001,2);CHECK(!got_left && !got_right && !s_effect_request[0]);
    int triggered=trigger_calls;
    CHECK(pad_feedback_test()==1);unsigned long long until=s_feedback_test_until;
    pad_sdl_effects(0,until-249,2);CHECK(pad_feedback_test_status()==2 && got_left==24576 && got_right==24576 && got_ms==250);
    CHECK(trigger_calls==triggered); // Test is body only, even with trigger vibration enabled.
    pad_sdl_effects(0,until,2);CHECK(!got_left && !got_right && !s_feedback_test_until);
    CHECK(pad_feedback_test()==1);pad_sdl_effects(0,s_feedback_test_until-249,0);CHECK(pad_feedback_test_status()==-5 && !s_feedback_test_until);
    CHECK(pad_feedback_test()==1);s_sdl_pad[0]=NULL;pad_sdl_effects(0,s_feedback_test_until-249,2);CHECK(pad_feedback_test_status()==-2);
    s_sdl_pad[0]=(void*)1;CHECK(pad_feedback_test()==1);pad_sdl_effects(0,s_feedback_test_until+1,2);CHECK(pad_feedback_test_status()==-6);
    fail_body=1;CHECK(pad_feedback_test()==1);pad_sdl_effects(0,s_feedback_test_until-249,2);CHECK(pad_feedback_test_status()==-3 && s_body_error[0]);
    pad_sdl_effects(0,s_feedback_test_until,2);CHECK(!got_left && !got_right);fail_body=0;
    _putenv_s("PAD_CONTROLS_POLISH","0");CHECK(pad_feedback_test()==-4);_putenv_s("PAD_CONTROLS_POLISH","");
    _putenv_s("PAD_FEEDBACK_TEST","0");CHECK(pad_feedback_test()==-7);
    s_sdl_pad[0]=NULL;s_sdl_ok=0;free(vm_base);vm_base=NULL;
    puts("PASS: real guest actuator bytes to SDL, F1-only 250ms body test, timed stop, focus loss, no device, expiry, API failure and rollbacks");
}
static void test_shapes(void)
{
    int cases=0;
    for(int dz=0;dz<=20;dz++)for(int curve=0;curve<3;curve++) {
        int prev=0;
        for(int x=0;x<=255;x++) { int y=pad_trigger_response((u8)x,dz,curve);CHECK(y>=prev);CHECK(y<=255);if(x<=dz*255/100)CHECK(y==0);prev=y;cases++; }
        CHECK(prev==255);
    }
    CHECK(pad_trigger_response(128,0,0)==128);
    CHECK(pad_trigger_response(128,0,1)==64);
    CHECK(pad_trigger_response(128,0,2)==192);
    printf("PASS: %d exhaustive monotonic pressure cases (neutral default and curved endpoints)\n",cases);
}

static void test_packets(void)
{
    vm_base=(uint8_t*)calloc(1,0x20000);CHECK(vm_base);ppu_vm_size=0x20000;
    s_pad_initialized=1;s_max_connect=4;s_port_setting[0]=CELL_PAD_SETTING_PRESS_ON;
    pad_lock(); /* Freeze host sampling; test the actual guest emission/fix-up. */
    for(int n=1;n<256;n++) {
        pad_host_release(&s_host_state[0]);s_host_state[0].connected=1;
        s_host_state[0].trigger_l2=(u8)n;s_host_state[0].trigger_r2=(u8)n;
        s_host_state[0].buttons=CELL_PAD_CTRL_L2|CELL_PAD_CTRL_R2;s_data_fresh[0]=1;
        CHECK(cellPadGetData(0,(CellPadData*)(uintptr_t)0x10000)==0);
        CHECK(vm_read16(0x10000+4+CELL_PAD_BTN_OFFSET_PRESS_L2*2)==n);
        CHECK(vm_read16(0x10000+4+CELL_PAD_BTN_OFFSET_PRESS_R2*2)==n);
    }
    pad_unlock();free(vm_base);vm_base=NULL;
    puts("PASS: 255 partial/full trigger guest packets survive fix-up");
}

static void test_hardware(void)
{
    CHECK(pad_sdl_dyn_init());pad_sdl_dyn_scan();
    int found=0;
    for(int i=0;i<PAD_MAX_HOST_PORTS;i++) if(s_sdl_pad[i] && s_sdl_type[i]==7) {
        found++;
        CHECK(s_sdl.GameControllerSendEffect);
        u8 packet[47];pad_dualsense_packet(packet,1);
        int on=s_sdl.GameControllerSendEffect(s_sdl_pad[i],packet,47);
        Sleep(350);
        pad_dualsense_packet(packet,0);
        int off=s_sdl.GameControllerSendEffect(s_sdl_pad[i],packet,47);
        int triggers=s_sdl.GameControllerHasRumbleTriggers ? s_sdl.GameControllerHasRumbleTriggers(s_sdl_pad[i]) : 0;
        printf("HARDWARE DualSense port=%d enable=%d clear=%d trigger_rumble_capability=%d\n",i,on,off,triggers);
        CHECK(on==0 && off==0);
    }
    CHECK(found);puts("PASS: DualSense accepted a 350ms fixed-resistance request and clear; tactile feel unverified");
}
int main(int argc,char** argv)
{
    if(argc!=2)return 2;
    if(!strcmp(argv[1],"shapes"))test_shapes();
    else if(!strcmp(argv[1],"packets"))test_packets();
    else if(!strcmp(argv[1],"rollback")) { CHECK(pad_control_int("PAD_MODERN_CONTROLS",1)==0);CHECK(pad_control_int("PAD_TRIGGER_CURVE",2)==0);puts("PASS: master rollback"); }
    else if(!strcmp(argv[1],"fallback")) { CHECK(!pad_sdl_dyn_read());pad_poll_xinput();puts("PASS: no-SDL XInput fallback"); }
    else if(!strcmp(argv[1],"hardware"))test_hardware();
    else if(!strcmp(argv[1],"effects"))test_effects();
    else if(!strcmp(argv[1],"capability-size"))test_capability_size();
    else if(!strcmp(argv[1],"feedback-hints"))test_feedback_hints();
    else if(!strcmp(argv[1],"guest-feedback"))test_guest_feedback();
    else if(!strcmp(argv[1],"ports"))test_pad_ports();
    else if(!strcmp(argv[1],"worker"))test_pad_worker();
    else if(!strcmp(argv[1],"devices")) { CHECK(pad_sdl_dyn_init());pad_sdl_dyn_scan();printf("DEVICES %d\n",s_sdl.NumJoysticks()); }
    else return 2;
    return 0;
}
