/* Guest C++ exceptions delivered to chosen handlers (scripts/guest_eh_patch.py).
 *
 * The game throws C++ exceptions (libsupc++ / libgcc unwinder, lifted with the
 * rest of the code). The unwinder ends each handler search by installing the
 * handler frame's registers and branching into its landing pad
 * (__builtin_eh_return). A static recompiler cannot branch into the middle of
 * another lifted function, so that branch is lifted as a plain return:
 * _Unwind_RaiseException returns, __cxa_throw calls terminate and the game
 * exits with code 1. That is how GT5 2.11's script errors ended the game
 * (#55-#58, #64): the script engine's native-call wrapper catches them on a
 * console and the script carries on.
 *
 * This does the delivery for landing pads the patch has prepared ("catch
 * sites", `guest_eh.catch_sites` in config/targets.json):
 *
 *   - the patch puts an armed host setjmp around the try region's call in the
 *     lifted function, with a snapshot of the guest's callee-saved registers
 *     (at a call they are what the landing pad expects), and a label at the
 *     landing pad;
 *   - at _Unwind_RaiseException's entry, gt5p_eh_raise searches the guest
 *     frames (back chain, saved LR) with the game's own .eh_frame and LSDA
 *     tables, as the personality routine's search phase would: no FDE, or a
 *     call site missing from its table (terminate), or an exception
 *     specification: leave it to the old path;
 *   - when the first frame with a matching catch clause is an armed site, it
 *     fills the exception header's handler fields as the personality routine
 *     does, restores the snapshot, sets r3 (the _Unwind_Exception) and r4
 *     (the selector), and longjmps to the site, which goes to the pad.
 *
 * Not done: cleanup landing pads in the frames in between (destructors) are
 * skipped, so their objects leak; the count is logged. The frames between
 * must be lifted guest code; a host runtime frame there (a callback) is
 * jumped over too.
 *
 * GT5P_GUEST_EH=0: nothing is delivered (the old exit). Each delivery prints
 * one `[guest-eh]` line (at most 64 a run).
 */
#include <windows.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <mutex>
#include <vector>

#include "ppu_context.h"

extern "C" {
uint8_t  vm_read8(uint64_t);
uint32_t vm_read32(uint64_t);
uint64_t vm_read64(uint64_t);
void     vm_write32(uint64_t, uint32_t);
int  ppu_indirect_depth_get(void);
void ppu_indirect_depth_set(int d);
int  gt5p_jmp_depth(void);
void gt5p_jmp_unwind(int depth);
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
}

namespace {

/* __cxa_exception as this toolchain lays it out (32-bit pointers), relative to
 * the header; the _Unwind_Exception that the unwinder and the landing pad see
 * is at +0x30 and the thrown object at +0x50 (__cxa_throw 2.11 0x00BE9C90,
 * __cxa_begin_catch 0x00BE8A64). */
constexpr uint32_t HDR_TYPE = 0x00, HDR_SWITCH = 0x18, HDR_ACTION = 0x1C, HDR_LSDA = 0x20,
                   HDR_LANDING = 0x24, HDR_ADJUSTED = 0x28, HDR_UNWIND = 0x30, HDR_OBJECT = 0x50;

struct site {
    jmp_buf  host;
    uint32_t sp, func;
    int      jmp_depth, indirect_depth;
    uint64_t gpr[32];
    double   fpr[32];
    alignas(16) uint8_t vr[12 * 16];  /* v20-v31 */
    uint32_t cr;
};

constexpr int SITE_MAX = 32;
thread_local site t_site[SITE_MAX];
thread_local int  t_sites;

bool enabled()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GT5P_GUEST_EH"); on = !(e && e[0] == '0'); }
    return on != 0;
}

uint8_t  r8(uint32_t a)  { return vm_read8(a); }
uint32_t r32(uint32_t a) { return vm_read32(a); }
uint64_t r64(uint32_t a) { return vm_read64(a); }

uint32_t uleb(uint32_t& p)
{
    uint32_t v = 0; int s = 0; uint8_t b;
    do { b = r8(p++); if (s < 32) v |= (uint32_t)(b & 0x7F) << s; s += 7; } while (b & 0x80);
    return v;
}

int32_t sleb(uint32_t& p)
{
    int32_t v = 0; int s = 0; uint8_t b;
    do { b = r8(p++); if (s < 32) v |= (int32_t)(b & 0x7F) << s; s += 7; } while (b & 0x80);
    if (s < 32 && (b & 0x40)) v |= -(1 << s);
    return v;
}

/* The encodings the game's tables use: 0xFF omitted, 0x01 uleb128, 0x04
 * udata8, 0x14
 * pc-relative udata8, 0x94 the same through a 32-bit pointer. A raw 0 stays 0
 * (libgcc's read_encoded_value). Returns false for anything else. */
bool enc(uint32_t& p, uint8_t e, uint32_t& out)
{
    if (e == 0xFF) { out = 0; return true; }
    if (e == 0x01) { out = uleb(p); return true; }
    if (e == 0x04) { out = (uint32_t)r64(p); p += 8; return true; }   /* FDE ranges */
    if ((e & 0x7F) != 0x14) return false;
    const uint32_t at = p;
    const uint64_t raw = r64(p);
    p += 8;
    if (!raw) { out = 0; return true; }
    out = (uint32_t)(raw + at);
    if (e & 0x80) out = r32(out);
    return true;
}

struct fde { uint32_t start, end, lsda; };
std::vector<fde> g_fdes;
std::once_flag   g_fdes_once;

void build_fdes(uint32_t eh, uint32_t size)
{
    struct cie { uint32_t at; uint8_t fde_enc, lsda_enc; };
    std::vector<cie> cies;
    for (uint32_t a = eh; a + 8 <= eh + size;) {
        const uint32_t len = r32(a);
        if (!len || len == 0xFFFFFFFFu) break;
        const uint32_t id = r32(a + 4);
        if (id == 0) {
            cie c{a, 0, 0xFF};   /* an FDE's CIE pointer leads to the CIE's length word */
            uint32_t p = a + 9;
            char aug[8] = {};
            for (int i = 0; ; i++) { const char ch = (char)r8(p++); if (!ch) break; if (i < 7) aug[i] = ch; }
            uleb(p); sleb(p); p++;  /* code/data alignment, return register (version 1) */
            if (aug[0] == 'z') {
                uleb(p);
                for (int i = 1; aug[i]; i++) {
                    if (aug[i] == 'P') { const uint8_t pe = r8(p++); uint32_t v; if (!enc(p, pe, v)) break; }
                    else if (aug[i] == 'L') c.lsda_enc = r8(p++);
                    else if (aug[i] == 'R') c.fde_enc = r8(p++);
                }
            }
            cies.push_back(c);
        } else {
            const uint32_t at = a + 4 - id;
            const cie* c = nullptr;
            for (size_t i = cies.size(); i-- > 0;) if (cies[i].at == at) { c = &cies[i]; break; }
            if (c) {
                uint32_t p = a + 8, start, range, lsda = 0;
                if (enc(p, c->fde_enc, start) && enc(p, c->fde_enc & 0x0F, range)) {
                    if (c->lsda_enc != 0xFF) { uleb(p); if (!enc(p, c->lsda_enc, lsda)) lsda = 0; }
                    g_fdes.push_back({start, start + range, lsda});
                }
            }
        }
        a += 4 + len;
    }
    std::sort(g_fdes.begin(), g_fdes.end(), [](const fde& x, const fde& y) { return x.start < y.start; });
    fprintf(stderr, "[guest-eh] %zu FDEs from .eh_frame 0x%08X\n", g_fdes.size(), eh);
}

const fde* find_fde(uint32_t ip)
{
    auto it = std::upper_bound(g_fdes.begin(), g_fdes.end(), ip,
                               [](uint32_t v, const fde& f) { return v < f.start; });
    if (it == g_fdes.begin()) return nullptr;
    --it;
    return ip < it->end ? &*it : nullptr;
}

/* Does a catch clause of type `want` take an exception of type `thrown`?
 * Single-inheritance class chains (the si_class_type_info vtable) and exact
 * matches only; `adjusted` is what __cxa_begin_catch hands the handler. */
bool type_matches(uint32_t want, uint32_t thrown, uint32_t obj, uint32_t si_vptr, uint32_t& adjusted)
{
    if (!want) { adjusted = obj; return true; }
    const bool pointer = r8(r32(want + 4)) == 'P';
    for (uint32_t t = thrown, n = 0; t && n < 32; n++) {
        if (t == want) { adjusted = pointer ? r32(obj) : obj; return true; }
        if (pointer || r32(t) != si_vptr) break;
        t = r32(t + 8);
    }
    return false;
}

enum class frame_result { next, handler, give_up };

/* One frame of the search phase. */
frame_result search_frame(uint32_t ip, uint32_t thrown, uint32_t obj, uint32_t si_vptr,
                          uint32_t& lp, int32_t& selector, uint32_t& action_at, uint32_t& lsda_out,
                          uint32_t& adjusted, uint32_t& func, int& cleanups)
{
    const fde* f = find_fde(ip);
    if (!f) return frame_result::give_up;
    func = f->start;
    if (!f->lsda) return frame_result::next;
    uint32_t p = f->lsda, lpstart = f->start, ttbase = 0, v;
    const uint8_t lpe = r8(p++);
    if (lpe != 0xFF) { if (!enc(p, lpe, lpstart)) return frame_result::give_up; }
    const uint8_t tte = r8(p++);
    if (tte != 0xFF) { const uint32_t off = uleb(p); ttbase = p + off; }
    const uint8_t cse = r8(p++);
    const uint32_t cslen = uleb(p), end = p + cslen;
    while (p < end) {
        uint32_t s, n, pad;
        if (!enc(p, cse, s) || !enc(p, cse, n) || !enc(p, cse, pad)) return frame_result::give_up;
        const uint32_t act = uleb(p);
        if (ip < f->start + s) break;
        if (ip >= f->start + s + n) continue;
        if (!pad) return frame_result::next;
        if (!act) { cleanups++; return frame_result::next; }
        uint32_t a = end + act - 1;
        for (int guard = 0; guard < 64; guard++) {
            const uint32_t here = a;
            const int32_t filter = sleb(a);
            uint32_t q = a;
            const int32_t disp = sleb(q);
            if (filter < 0) return frame_result::give_up;   /* exception specification */
            if (filter == 0) cleanups++;
            else {
                if (tte == 0xFF) return frame_result::give_up;
                uint32_t e = ttbase - (uint32_t)filter * 8u;
                if (!enc(e, tte, v)) return frame_result::give_up;
                if (type_matches(v, thrown, obj, si_vptr, adjusted)) {
                    lp = lpstart + pad; selector = filter; action_at = here; lsda_out = f->lsda;
                    return frame_result::handler;
                }
            }
            if (!disp) break;
            a += disp;
        }
        return frame_result::next;
    }
    return frame_result::give_up;   /* not in the call-site table: terminate */
}

} // namespace

extern "C" site* gt5p_eh_arm(ppu_context* ctx, uint32_t func)
{
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    /* Entries whose frames are gone (left by a guest longjmp over them) are at
     * or below this frame's stack pointer. */
    while (t_sites > 0 && t_site[t_sites - 1].sp <= sp) t_sites--;
    site* s = &t_site[t_sites < SITE_MAX ? t_sites++ : SITE_MAX - 1];
    s->sp = sp;
    s->func = func;
    s->jmp_depth = gt5p_jmp_depth();
    s->indirect_depth = ppu_indirect_depth_get();
    memcpy(s->gpr, ctx->gpr, sizeof s->gpr);
    memcpy(&s->fpr[14], &ctx->fpr[14], 18 * sizeof(double));
    memcpy(s->vr, &ctx->vr[20], sizeof s->vr);
    s->cr = ctx->cr;
    return s;
}

extern "C" jmp_buf* gt5p_eh_host(site* s) { return &s->host; }

extern "C" void gt5p_eh_disarm(site* s)
{
    const int i = (int)(s - t_site);
    if (i >= 0 && i < t_sites) t_sites = i;
}

/* _Unwind_RaiseException's entry: r3 the _Unwind_Exception, LR the return into
 * __cxa_throw, r1 __cxa_throw's frame. Returns only when nothing is delivered. */
extern "C" void gt5p_eh_raise(ppu_context* ctx, uint32_t eh, uint32_t eh_size, uint32_t si_vptr)
{
    if (!t_sites || !enabled()) return;
    std::call_once(g_fdes_once, build_fdes, eh, eh_size);
    const uint32_t unwind = (uint32_t)ctx->gpr[3];
    const uint32_t hdr = unwind - HDR_UNWIND, obj = hdr + HDR_OBJECT;
    const uint32_t thrown = r32(hdr + HDR_TYPE);
    uint32_t ra = (uint32_t)ctx->lr, sp = (uint32_t)ctx->gpr[1];
    int cleanups = 0;
    static volatile long printed = 0;
    for (int depth = 0; depth < 256 && ra; depth++) {
        uint32_t lp = 0, action = 0, lsda = 0, adjusted = 0, func = 0;
        int32_t selector = 0;
        const frame_result r = search_frame(ra - 1, thrown, obj, si_vptr, lp, selector, action, lsda,
                                            adjusted, func, cleanups);
        if (r == frame_result::give_up) {
            /* no FDE, terminate region or exception specification: as before */
            if (InterlockedIncrement(&printed) <= 64)
                fprintf(stderr, "[guest-eh] throw of type 0x%08X not delivered: frame %d (0x%08X in 0x%08X) ends the search\n",
                        thrown, depth, ra, func);
            return;
        }
        if (r == frame_result::handler) {
            int i = t_sites;
            while (i > 0 && t_site[i - 1].sp != sp) i--;
            const uint32_t tname = thrown ? r32(thrown + 4) : 0;
            char tn[64] = {};
            for (int k = 0; tname && k < 63; k++) { const char c = (char)r8(tname + k); if (!c) break; tn[k] = c; }
            if (!i || t_site[i - 1].func != func) {
                if (InterlockedIncrement(&printed) <= 64)
                    fprintf(stderr, "[guest-eh] %s: handler in 0x%08X (pad 0x%08X) is not a catch site; not delivered\n",
                            tn, func, lp);
                return;
            }
            site* s = &t_site[i - 1];
            vm_write32(hdr + HDR_SWITCH, (uint32_t)selector);
            vm_write32(hdr + HDR_ACTION, action);
            vm_write32(hdr + HDR_LSDA, lsda);
            vm_write32(hdr + HDR_LANDING, lp);
            vm_write32(hdr + HDR_ADJUSTED, adjusted);
            if (InterlockedIncrement(&printed) <= 64) {
                fprintf(stderr, "[guest-eh] %s caught by 0x%08X at pad 0x%08X (selector %d, %d frame(s) up, %d cleanup(s) skipped)\n",
                        tn, func, lp, (int)selector, depth, cleanups);
                fflush(stderr);
            }
            ctx->gpr[1] = s->gpr[1];
            ctx->gpr[2] = s->gpr[2];
            memcpy(&ctx->gpr[13], &s->gpr[13], 19 * sizeof(uint64_t));
            memcpy(&ctx->fpr[14], &s->fpr[14], 18 * sizeof(double));
            memcpy(&ctx->vr[20], s->vr, sizeof s->vr);
            ctx->cr = s->cr;
            ctx->gpr[3] = unwind;
            ctx->gpr[4] = (uint64_t)(int64_t)selector;
            ctx->lr = lp;
            g_trampoline_fn = nullptr;
            gt5p_jmp_unwind(s->jmp_depth);
            ppu_indirect_depth_set(s->indirect_depth);
            t_sites = i - 1;   /* the site is consumed; the patch's else-branch goes to the pad */
            longjmp(s->host, 1);
        }
        /* the caller's frame: its LR was saved at 16 above this frame's back chain */
        const uint32_t back = (uint32_t)r64(sp);
        if (!back || back <= sp) return;
        ra = (uint32_t)r64(back + 16);
        sp = back;
    }
}
