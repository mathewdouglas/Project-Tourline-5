/* Standalone regression against production cellAudio.c, using fake COM only. */
#include <mmdeviceapi.h>
#include <windows.h>
static HRESULT WINAPI fake_create(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
static HRESULT WINAPI fake_com_init(LPVOID, DWORD);
static ULONGLONG WINAPI fake_tick(void);
#define CoCreateInstance fake_create
#define CoInitializeEx fake_com_init
#define GetTickCount64 fake_tick
#include "libs/audio/cellAudio.c"
#undef CoCreateInstance
#undef CoInitializeEx
#undef GetTickCount64
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x);                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
uint8_t *vm_base;
PPU_THREAD_LOCAL ppu_context *g_active_ctx;
uint32_t ppu_vm_size, g_ww_lo, g_ww_hi;
int g_resv_store_active;
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { abort(); }
int spu_coh_is_reserved(uint32_t a) { abort(); }
void spu_lockline_lock(void) { abort(); }
void spu_lockline_unlock(void) { abort(); }
void spu_coh_notify_write(uint32_t a) { abort(); }
void ppu_resv_break_store(uint64_t a) { abort(); }
uint32_t sys_event_queue_create_direct(uint64_t k, int32_t n) { abort(); }
uint32_t sys_event_find_queue_by_key(uint64_t k) { abort(); }
int sys_event_queue_push_by_id(uint32_t q, uint64_t s, uint64_t a, uint64_t b,
                               uint64_t c) {
  abort();
}
static ULONGLONG ticks = 100;
static HRESULT create_result = S_OK, com_result = S_OK;
static HRESULT STDMETHODCALLTYPE fake_default(IMMDeviceEnumerator *s,
                                              EDataFlow f, ERole r,
                                              IMMDevice **out) {
  *out = NULL;
  return E_NOTFOUND;
}
static HRESULT STDMETHODCALLTYPE fake_register(IMMDeviceEnumerator *s,
                                               IMMNotificationClient *n) {
  return E_NOTIMPL;
}
static IMMDeviceEnumeratorVtbl enum_vt = {
    .GetDefaultAudioEndpoint = fake_default,
    .RegisterEndpointNotificationCallback = fake_register};
static IMMDeviceEnumerator enumerator = {&enum_vt};
static HRESULT WINAPI fake_create(REFCLSID c, LPUNKNOWN u, DWORD d, REFIID i,
                                  LPVOID *out) {
  *out = SUCCEEDED(create_result) ? &enumerator : NULL;
  return create_result;
}
static HRESULT WINAPI fake_com_init(LPVOID p, DWORD d) { return com_result; }
static ULONGLONG WINAPI fake_tick(void) { return ticks; }
static void put_be(float *dst, uint32_t bits) {
  bits = _byteswap_ulong(bits);
  memcpy(dst, &bits, 4);
}
int main(int argc, char **argv) {
  CHECK(argc == 2);
  if (!strcmp(argv[1], "mix") || !strcmp(argv[1], "mix-off") ||
      !strcmp(argv[1], "mix-healthy") || !strcmp(argv[1], "mix-inf")) {
    int scrub = strcmp(argv[1], "mix-off") != 0;
    int healthy = !strcmp(argv[1], "mix-healthy");
    int inf = !strcmp(argv[1], "mix-inf");
    _putenv_s("AUDIO_NONFINITE_SCRUB", scrub ? "1" : "0");
    _putenv_s("AUDIO_JITTER", "0");
    _putenv_s("AUDIO_READ_LAG", "0");
    static float broken[8 * 256 * 8], movie[8 * 256 * 2];
    mutex_init(&s_audio_mutex);
    for (int p = 0; p < 2; p++) {
      s_ports[p].in_use = s_ports[p].running = 1;
      s_ports[p].param.nBlock = 8;
      s_ports[p].param.level = 1;
      s_ports[p].param.nChannel = p ? 2 : 8;
      s_ports[p].buffer = p ? movie : broken;
    }
    for (unsigned f = 0; f < 256; f++) {
      put_be(&broken[f * 8],
             healthy
                 ? 0x3e000000
                 : ((f & 1) ? (inf ? 0x7f800000 : 0x7fc00001) : 0xbf400000));
      put_be(&broken[f * 8 + 1],
             healthy
                 ? 0x3e800000
                 : ((f & 1) ? (inf ? 0xff800000 : 0xffc00000) : 0xbf400000));
      put_be(&movie[f * 2], 0x3e800000);
      put_be(&movie[f * 2 + 1], 0xbe000000);
    }
    audio_mix_one_block();
    CHECK(s_input_nan[0] == ((healthy || inf) ? 0 : 256) &&
          s_input_nan[1] == 0);
    CHECK(s_input_inf[0] == (inf ? 256 : 0) && s_input_inf[1] == 0);
    AudioBlockStats a = audio_check_block(s_mix_buffer, 512, 0);
    if (scrub) {
      CHECK(!a.nan && !a.inf && a.nonzero == 512);
      for (unsigned f = 0; f < 256; f++)
        CHECK(s_mix_buffer[f * 2] == (healthy ? .375f : .25f) &&
              s_mix_buffer[f * 2 + 1] == (healthy ? .125f : -.125f));
    } else
      CHECK(a.nan == 256 && a.nonzero == 256);
    mutex_destroy(&s_audio_mutex);
    puts("PASS: production mixer preserves healthy ports, drops corrupt-port "
         "tone, and rollback reproduces poisoning");
    return 0;
  }
  if (!strcmp(argv[1], "stats")) {
    uint32_t words[] = {0,          0x80000000, 0x3f000000, 0x7fc00001,
                        0xffc00000, 0x7f800001, 0x7f800000, 0xff800000};
    float input[8], original[8];
    memcpy(input, words, sizeof words);
    memcpy(original, input, sizeof input);
    AudioBlockStats a = audio_check_block(input, 8, 0);
    CHECK(a.nan == 3 && a.inf == 2 && a.nonzero == 1 && a.peak == .5f);
    CHECK(!memcmp(input, original, sizeof input));
    a = audio_check_block(input, 8, 1);
    CHECK(a.nan == 3 && a.inf == 2 && a.nonzero == 1 && a.peak == .5f);
    CHECK(!memcmp(input, original, 3 * sizeof(float)));
    for (int i = 3; i < 8; i++)
      CHECK(input[i] == 0);
    a = audio_check_block(input, 8, 1);
    CHECK(!a.nan && !a.inf && a.nonzero == 1 && a.peak == .5f);
    puts("PASS: NaN/Inf accounting, scrub and rollback, finite sample "
         "preservation");
    return 0;
  }
  if (!strcmp(argv[1], "enum"))
    create_result = E_ACCESSDENIED;
  else if (!strcmp(argv[1], "endpoint")) {
  } else if (!strcmp(argv[1], "com"))
    com_result = E_ACCESSDENIED;
  else
    return 2;
  CHECK(audio_backend_init() == -1);
  if (strcmp(argv[1], "com")) {
    s_wasapi_next_check = 0;
    ticks = 1000;
    audio_backend_refresh();
    s_wasapi_next_check = 0;
    ticks = 6100;
    audio_backend_refresh();
  }
  puts("PASS: fake startup failure exercised without opening a device");
  return 0;
}
