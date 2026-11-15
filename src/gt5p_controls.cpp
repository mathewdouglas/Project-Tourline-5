/* The game's player-one SIXAXIS mapping. Numeric layout facts are measured on
 * BCUS98114-v104 / v211 and pinned in config/targets.json. No save offsets or game code.
 * Requests execute on guest controller/pad threads; the UI only publishes intent.
 * The game's getConfig/setConfig and profile-load methods share this lock so
 * an input poll cannot observe or change half of a menu assignment commit. */
#include "gt5p_controls.h"
#include "ppu_context.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#ifndef GT5P_CONTROLS_CORE
#define GT5P_CONTROLS_CORE 0
#define GT5P_CONTROLS_VTABLE 0
#define GT5P_CONTROLS_GET 0
#define GT5P_CONTROLS_SET 0
#define GT5P_CONTROLS_LOAD 0
#define GT5P_CONTROLS_SAVE 0
#endif
extern "C" {
typedef struct { uint64_t addr; void (*func)(ppu_context*); const char* name; } gt5p_controls_entry;
extern const gt5p_controls_entry function_table[];
extern const uint64_t function_table_count;
extern uint8_t* vm_base;
void ppu_register_function(uint64_t, void (*)(ppu_context*));
uint32_t vm_read32(uint64_t);
uint16_t vm_read16(uint64_t);
uint8_t vm_read8(uint64_t);
void vm_write8(uint64_t,uint8_t);
extern void (*g_pad_controls_poll_hook)(void);
}
namespace {
using Fn=void(*)(ppu_context*);
Fn getter, setter, loader, saver;
std::recursive_mutex mapping_mu;
std::atomic<int> mapping_state{-1}, request{-1};
std::atomic<bool> profile_ready{false};
bool enabled;
std::chrono::steady_clock::time_point next_sample;
/* Pressure channels Cross, Square, L2, R2; matching digital button codes. */
constexpr uint8_t analog_keys[4]={29,31,34,35};
constexpr uint8_t button_keys[4]={10,12,25,26};
constexpr uint8_t modern[4]={9,10,5,4};
constexpr uint8_t defaults[4]={4,5,10,9};
struct Mapping { uint32_t analog[4]{},button[4]{}; };
bool range(uint32_t a,uint32_t bytes) {
 if(!vm_base || a<0x10000 || a>=0xf0000000 || !bytes || bytes>0xf0000000u-a)return false;
#ifdef _WIN32
 uintptr_t p=(uintptr_t)vm_base+a,end=p+bytes;
 while(p<end) {
  MEMORY_BASIC_INFORMATION m{};
  if(!VirtualQuery((void*)p,&m,sizeof m) || m.State!=MEM_COMMIT || (m.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
  uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;if(next<=p)return false;p=next;
 }
#endif
 return true;
}
bool vector_range(uint32_t header,uint32_t stride,uint32_t max,uint32_t& begin,uint32_t& end) {
 if(!range(header,16))return false;
 begin=vm_read32(header+4);end=vm_read32(header+8);
 uint32_t cap=vm_read32(header+12);
 return end>=begin && cap>=end && (end-begin)%stride==0 && (cap-begin)%stride==0 &&
        (cap-begin)/stride<=max &&
        (begin==end || range(begin,end-begin));
}
bool find_channels(uint32_t header,const uint8_t keys[4],uint32_t result[4],bool required=true) {
 uint32_t b,e;if(!vector_range(header,2,128,b,e))return false;
 for(uint32_t p=b;p<e;p+=2)for(int i=0;i<4;i++)if(vm_read8(p+1)==keys[i]) {
  if(result[i])return false;result[i]=p;
 }
 for(int i=0;i<4;i++)if(required && !result[i])return false;
 return true;
}
bool locate(Mapping& found) {
 constexpr uint32_t core=GT5P_CONTROLS_CORE;
 if(!core || !range(core,8) || vm_read32(core)!=GT5P_CONTROLS_VTABLE)return false;
 /* GT5 wraps a pointer to the mode vector, unlike Prologue. */
 uint32_t modes=vm_read32(core+4);
 uint32_t b,e;if(!vector_range(modes,20,16,b,e))return false;
 uint32_t device=0;
 for(uint32_t m=b;m<e;m+=20)if(vm_read16(m)==1) { /* PLAY_NORMAL */
  uint32_t db,de;if(!vector_range(m+4,36,32,db,de))return false;
  for(uint32_t d=db;d<de;d+=36)if(vm_read32(d)==7) { /* SIXAXIS */
   if(device)return false;device=d;
  }
 }
 return device && find_channels(device+4,analog_keys,found.analog) && find_channels(device+20,button_keys,found.button,false);
}
int detect(const Mapping& m) {
 for(int i=0;i<4;i++)if(vm_read8(m.analog[i])!=modern[i] || (m.button[i] && vm_read8(m.button[i])!=0))return 0;
 return 1;
}
void poll() {
 if(!profile_ready.load(std::memory_order_acquire))return;
 /* cellPad owns its poll lock here. Never wait for a guest hook that may
  * itself call cellPad while holding mapping_mu. Retry on the next poll. */
 std::unique_lock<std::recursive_mutex> lock(mapping_mu,std::try_to_lock);
 if(!lock.owns_lock())return;
 auto now=std::chrono::steady_clock::now();
 if(request.load(std::memory_order_acquire)<0 && now<next_sample)return;
 next_sample=now+std::chrono::milliseconds(100);
 Mapping m;
 if(!locate(m)){mapping_state.store(-1,std::memory_order_release);return;}
 int desired=request.exchange(-1,std::memory_order_acq_rel);
 if(desired>=0) {
  /* Resolve every address before the first write. Same byte fields updated
   * by MController::setConfig; do not replace vectors or other bindings. */
  for(int i=0;i<4;i++) {
   vm_write8(m.analog[i],desired?modern[i]:defaults[i]);if(m.button[i])vm_write8(m.button[i],0);
  }
  printf("[controls] game player-one mapping = %s (R2/L2/Cross/Square)\n",desired?"modern":"default");
 }
 int observed=detect(m);
 if(mapping_state.exchange(observed,std::memory_order_acq_rel)!=observed)
  printf("[controls] observed modern=%d; Cross/Square/L2/R2 actions=%u/%u/%u/%u\n",observed,
         vm_read8(m.analog[0]),vm_read8(m.analog[1]),vm_read8(m.analog[2]),vm_read8(m.analog[3]));
}
Fn find(uint32_t address) {
 uint64_t lo=0,hi=function_table_count;
 while(lo<hi){auto m=lo+(hi-lo)/2;auto a=function_table[m].addr;if(a==address)return function_table[m].func;if(a<address)lo=m+1;else hi=m;}
 return nullptr;
}
void get_hook(ppu_context* c){std::lock_guard<std::recursive_mutex> lock(mapping_mu);getter(c);profile_ready.store(true,std::memory_order_release);}
void set_hook(ppu_context* c){std::lock_guard<std::recursive_mutex> lock(mapping_mu);setter(c);profile_ready.store(true,std::memory_order_release);next_sample={};poll();}
void load_hook(ppu_context* c){std::lock_guard<std::recursive_mutex> lock(mapping_mu);loader(c);profile_ready.store(true,std::memory_order_release);next_sample={};poll();}
void save_hook(ppu_context* c){std::lock_guard<std::recursive_mutex> lock(mapping_mu);poll();saver(c);}
bool off(const char* key){const char* e=getenv(key);return e && !strcmp(e,"0");}
}
int gt5p_modern_controls_state(){return mapping_state.load(std::memory_order_acquire);}
bool gt5p_modern_controls_pending(){return request.load(std::memory_order_acquire)>=0;}
bool gt5p_modern_controls_request(int yes) {
 if(!enabled || (yes!=0 && yes!=1))return false;
 request.store(yes,std::memory_order_release);return true;
}
void gt5p_controls_install() {
 enabled=false;mapping_state.store(-2);
 if(!GT5P_CONTROLS_CORE || off("GT5P_NATIVE_CONTROLS") || off("PAD_CONTROLS_POLISH"))return;
 getter=find(GT5P_CONTROLS_GET);setter=find(GT5P_CONTROLS_SET);loader=find(GT5P_CONTROLS_LOAD);saver=find(GT5P_CONTROLS_SAVE);
 if(!getter || !setter || !loader || !saver){fprintf(stderr,"[controls] native mapping link unavailable for this build\n");return;}
 mapping_state.store(-1);
 enabled=true;
 ppu_register_function(GT5P_CONTROLS_GET,get_hook);ppu_register_function(GT5P_CONTROLS_SET,set_hook);ppu_register_function(GT5P_CONTROLS_LOAD,load_hook);ppu_register_function(GT5P_CONTROLS_SAVE,save_hook);
 g_pad_controls_poll_hook=poll;
 /* A stored observed value is not a command: never overwrite a player's
  * custom save at boot. Only F1 or an explicit environment command applies it. */
 if(const char* e=getenv("PAD_MODERN_CONTROLS")) {
  if(!strcmp(e,"0") || !strcmp(e,"1"))request.store(*e-'0');
 }
 printf("[controls] Modern controls follows the game's player-one mapping\n");
}
