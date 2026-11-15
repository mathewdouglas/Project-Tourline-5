#define GT5P_CONTROLS_CORE 0x10000u
#define GT5P_CONTROLS_VTABLE 0x2000u
#define GT5P_CONTROLS_GET 1u
#define GT5P_CONTROLS_SET 2u
#define GT5P_CONTROLS_LOAD 3u
#define GT5P_CONTROLS_SAVE 4u
#include "../../src/gt5p_controls.cpp"
#include <vector>
#include <algorithm>
#include <thread>
#include <future>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);} }while(0)
extern "C" {
uint8_t* vm_base;
void original(ppu_context*) {}
const gt5p_controls_entry function_table[]={{1,original,"get"},{2,original,"set"},{3,original,"load"},{4,original,"save"}};
const uint64_t function_table_count=4;
void (*hooks[5])(ppu_context*);
void ppu_register_function(uint64_t a,void(*f)(ppu_context*)){CHECK(a>=1 && a<=4);hooks[a]=f;}
void (*g_pad_controls_poll_hook)(void);
uint32_t vm_read32(uint64_t a){return (vm_base[a]<<24)|(vm_base[a+1]<<16)|(vm_base[a+2]<<8)|vm_base[a+3];}
uint16_t vm_read16(uint64_t a){return (vm_base[a]<<8)|vm_base[a+1];}
uint8_t vm_read8(uint64_t a){return vm_base[a];}
void vm_write8(uint64_t a,uint8_t v){vm_base[a]=v;}
}
void w32(uint32_t a,uint32_t v){for(int i=0;i<4;i++)vm_base[a+i]=(uint8_t)(v>>(24-i*8));}
void vec(uint32_t h,uint32_t b,uint32_t bytes){w32(h+4,b);w32(h+8,b+bytes);w32(h+12,b+bytes);}
void fixture(){
 std::fill(vm_base,vm_base+0x40000,0);
 w32(GT5P_CONTROLS_CORE,GT5P_CONTROLS_VTABLE);w32(GT5P_CONTROLS_CORE+4,0x11000);vec(0x11000,0x20000,40);
 vm_base[0x20001]=1;vm_base[0x20015]=3;vec(0x20004,0x21000,72);
 w32(0x21000,7);w32(0x21024,9);vec(0x21004,0x22000,16);vec(0x21014,0x23000,16);
 for(int i=0;i<4;i++){
  vm_base[0x22000+i*2]=defaults[i];vm_base[0x22001+i*2]=analog_keys[i];
  vm_base[0x23001+i*2]=button_keys[i];
  vm_base[0x22008+i*2]=17;vm_base[0x22009+i*2]=i+1;
  vm_base[0x23008+i*2]=21;vm_base[0x23009+i*2]=i+1;
 }
 request=-1;mapping_state=-1;profile_ready=true;enabled=true;next_sample={};
}
int main(){
 vm_base=(uint8_t*)calloc(1,0x40000);CHECK(vm_base);
 fixture();poll();CHECK(gt5p_modern_controls_state()==0);
 std::vector<uint8_t> before(vm_base,vm_base+0x40000);
 CHECK(gt5p_modern_controls_request(1));poll();CHECK(gt5p_modern_controls_state()==1);
 for(int i=0;i<4;i++)CHECK(vm_base[0x22000+i*2]==modern[i] && vm_base[0x23000+i*2]==0);
 for(unsigned i=0;i<before.size();i++)if(i<0x22000 || i>=0x22008 || (i&1))CHECK(vm_base[i]==before[i]);
 CHECK(gt5p_modern_controls_request(0));poll();CHECK(gt5p_modern_controls_state()==0);
 CHECK(std::equal(before.begin(),before.end(),vm_base));
 /* A game-side edit is observed, without a PC request or a TOML flag. */
 for(int i=0;i<4;i++)vm_base[0x22000+i*2]=modern[i];next_sample={};poll();CHECK(gt5p_modern_controls_state()==1);
 vm_base[0x23000]=4;next_sample={};poll();CHECK(gt5p_modern_controls_state()==0);
 /* Explicit No restores defaults even from a custom, already-No mapping. */
 CHECK(gt5p_modern_controls_request(0));poll();CHECK(std::equal(before.begin(),before.end(),vm_base));
 /* GT5's default digital table has none of these four keys. */
 fixture();vec(0x21014,0x23008,8);
 before.assign(vm_base,vm_base+0x40000);
 CHECK(gt5p_modern_controls_request(1));poll();CHECK(gt5p_modern_controls_state()==1);
 CHECK(gt5p_modern_controls_request(0));poll();CHECK(std::equal(before.begin(),before.end(),vm_base));
 /* Invalid wrapper pointer must retain the request and perform no writes. */
 fixture();w32(GT5P_CONTROLS_CORE+4,0);before.assign(vm_base,vm_base+0x40000);
 CHECK(gt5p_modern_controls_request(1));poll();CHECK(gt5p_modern_controls_pending());
 CHECK(std::equal(before.begin(),before.end(),vm_base));
 /* A guest hook holding the map lock cannot deadlock a pad poll. */
 fixture();CHECK(gt5p_modern_controls_request(1));
 std::promise<void> locked,release;auto released=release.get_future();
 std::thread owner([&]{std::lock_guard<std::recursive_mutex> guard(mapping_mu);locked.set_value();released.wait();});
 locked.get_future().wait();poll();CHECK(gt5p_modern_controls_pending());release.set_value();owner.join();
 poll();CHECK(gt5p_modern_controls_state()==1);
 /* Install actual hooks and defer the explicit command until profile import. */
 fixture();profile_ready=false;_putenv_s("PAD_MODERN_CONTROLS","1");
 gt5p_controls_install();CHECK(gt5p_modern_controls_pending());CHECK(g_pad_controls_poll_hook==poll);
 poll();CHECK(gt5p_modern_controls_state()==-1);hooks[3](nullptr);CHECK(gt5p_modern_controls_state()==1);
 CHECK(hooks[1] && hooks[2] && hooks[4]);_putenv_s("PAD_MODERN_CONTROLS","");
 int rejected=0;
 for(int bad=0;bad<5;bad++){
  fixture();
  if(bad==0)w32(GT5P_CONTROLS_CORE,0);
  if(bad==1)w32(0x21004+8,0x22001); // odd pair count
  if(bad==2)vm_base[0x22003]=analog_keys[0]; // duplicate/missing key
  if(bad==3)w32(0x21004+12,0); // invalid capacity
  if(bad==4)w32(0x21000,9); // missing SIXAXIS
  before.assign(vm_base,vm_base+0x40000);CHECK(gt5p_modern_controls_request(1));poll();
  CHECK(gt5p_modern_controls_state()==-1 && gt5p_modern_controls_pending());
  CHECK(std::equal(before.begin(),before.end(),vm_base));rejected++;
 }
 fixture();profile_ready=false;before.assign(vm_base,vm_base+0x40000);CHECK(gt5p_modern_controls_request(1));poll();CHECK(std::equal(before.begin(),before.end(),vm_base));
 enabled=false;CHECK(!gt5p_modern_controls_request(1));
#ifdef _WIN32
 _putenv_s("GT5P_NATIVE_CONTROLS","0");gt5p_controls_install();CHECK(gt5p_modern_controls_state()==-2 && !gt5p_modern_controls_request(1));
 _putenv_s("GT5P_NATIVE_CONTROLS","");_putenv_s("PAD_CONTROLS_POLISH","0");gt5p_controls_install();CHECK(gt5p_modern_controls_state()==-2 && !gt5p_modern_controls_request(1));
 _putenv_s("PAD_CONTROLS_POLISH","");
#endif
 printf("PASS: native mapping Yes/No, live game detection, custom-to-default, %d malformed layouts rejected atomically, deferred load and disabled link\n",rejected);
 free(vm_base);vm_base=nullptr;
}
