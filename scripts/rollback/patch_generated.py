# Rollback write tracking (branch rollback2): every guest store of the generated code marks its 4 KB
# page in rb2_dirty (bit 1 = main thread, 2 = other threads; src/burstlimit_rb2.cpp clears them).
# Idempotent; run after codegen:  python -I patch_generated.py <generated/default>
import pathlib, re, sys

MARK = '// ---- rb2 write tracking v10c ----'
BLOCK = MARK + r'''
extern "C" unsigned char rb2_dirty[0x100000];
extern "C" thread_local unsigned char rb2_tbit;
extern "C" unsigned char rb2_sfine[0x2000000];  // per 16-byte block: written by sound library code
extern "C" unsigned char rb2_ffine[0x2000000];  // per 16-byte block: written by other (foreign) threads since the page's last log
static constexpr unsigned char rb2_kind = 0;  // per function: 16 = sound library code (RB2_KIND)
#define RB2_KIND(k) [[maybe_unused]] constexpr unsigned char rb2_kind = (k)
// v5 (rollback3): the CRI call boundary. The outermost call into the CRI library on the main thread
// (rb2_cri_scope) is recorded in live frames and answered from the record in resimulated frames
// (src/burstlimit_rb2.cpp: rb2_cri_enter / rb2_cri_leave); while a call is recorded, the 16-byte blocks
// it writes outside the library's own frames are noted (rb2_cri_store).
extern "C" int rb2_cri_on;
extern "C" thread_local int rb2_cri_depth;
extern "C" thread_local unsigned char rb2_cri_rec;
extern "C" thread_local unsigned char rb2_main;
extern "C" thread_local unsigned char rb2_cri_live;  // the entered function's live argument mask (see arg_liveness)
extern "C" int rb2_cri_enter(unsigned int a, PPCContext& ctx);
extern "C" void rb2_cri_leave(unsigned int a, PPCContext& ctx);
extern "C" void rb2_cri_store(unsigned int x, unsigned int n, unsigned char kind);
extern "C" int rb2_d3d_watch;
extern "C" void rb2_d3d_hit(unsigned int a);
// v9: nothing but one global load while no rollback engine has run in this process (rb2_cri_on == 0)
struct rb2_cri_scope {
  PPCContext* c; unsigned int a; int act;
  rb2_cri_scope(unsigned int a_, unsigned char live, PPCContext& c_) : c(&c_), a(a_), act(-1) {
    if (!rb2_cri_on) return;
    act = 0;
    if (rb2_cri_depth++ == 0 && rb2_main) { rb2_cri_live = live; act = 1 + rb2_cri_enter(a_, c_); }
  }
  ~rb2_cri_scope() { if (act < 0) return; --rb2_cri_depth; if (act == 1) rb2_cri_leave(a, *c); }
};
#define RB2_CRI_ENTRY(a, live) rb2_cri_scope rb2_cri_sc_((a), (live), ctx); if (rb2_cri_sc_.act == 2) return;
#define RB2_D3D_ENTRY(a) if (rb2_d3d_watch && (rb2_main || rb2_d3d_watch > 1)) rb2_d3d_hit(a);
extern "C" unsigned char rb2_track_on;  // set once a rollback engine starts in this process
// v10 (2026-11-13): a store costs one global byte test and, only while tracking, a call - v9 inlined the whole
// mark (+ the store watch) at every guest store, .text grew from 27 MB (v0.7.3) to 84 MB and rollback-off
// fights paid 5-30 % CPU. The work is in src/burstlimit_rb2.cpp (rb2_store_note: dirty page, sound / foreign
// 16-byte blocks, CRI call records, the store watch). kn = kind << 16 | bytes.
extern "C" void rb2_store_note(unsigned int x, unsigned int kn, unsigned long long y);
static inline void rb2_mark_k(unsigned int x, unsigned char kind, unsigned int n = 16) {
  if (__builtin_expect(rb2_track_on, 0)) rb2_store_note(x, ((unsigned int)kind << 16) | n, 0);
}
static inline void rb2_mark_host(const void* p, const unsigned char* b, unsigned char kind) {
  if (!__builtin_expect(rb2_track_on, 0)) return;
  unsigned long long o = (unsigned long long)((const unsigned char*)p - b);
  if (o < 0x100000000ull) {
    if (o >= 0xE0001000ull) o -= 0x1000ull;
    rb2_store_note((unsigned int)o, ((unsigned int)kind << 16) | 16, 0);
  }
}
static inline void rb2_watch_host(const void*, const unsigned char*, const char*) {}  // v10: in rb2_store_note
#define RB2_STORE(x, y, n) (__builtin_expect(rb2_track_on, 0) ? rb2_store_note((unsigned int)(x), ((unsigned int)rb2_kind << 16) | (n), (unsigned long long)(y)) : (void)0)
#define RB2_MARK(x) rb2_mark_k((unsigned int)(x), rb2_kind)
#define RB2_MARKN(x, n) rb2_mark_k((unsigned int)(x), rb2_kind, (n))
#define RB2_MARK_HOST(p) rb2_mark_host((const void*)(p), (const unsigned char*)base, rb2_kind)
#undef REX_STORE_U8
#undef REX_STORE_U16
#undef REX_STORE_U32
#undef REX_STORE_U64
// v11b (2026-11-14): stores into the function's OWN stack frame are not noted - thread stacks are never tracked
// (0x70000000..). Only provably own-frame stores: negative offsets from r1, stwu's back chain, and positive offsets
// below the frame size after the function's stwu. A store at r1 + N above that can be the CALLER's frame (parameter
// save area): a CRI call answered from its record must write those bytes back (v11 made every r1 store raw -> an
// answered call left the caller's stack stale: crash in 82115920 reading a garbage pointer)
#define REX_STORE_RAW_U8(x, y)  (*(volatile u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))
#define REX_STORE_RAW_U16(x, y) (*(volatile u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))
#define REX_STORE_RAW_U32(x, y) (*(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))
#define REX_STORE_RAW_U64(x, y) (*(volatile u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))
#define REX_STORE_U8(x, y)  (RB2_STORE(x, y, 1), *(volatile u8*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))
#define REX_STORE_U16(x, y) (RB2_STORE(x, y, 2), *(volatile u16*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap16(y))
#define REX_STORE_U32(x, y) (RB2_STORE(x, y, 4), *(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))
#define REX_STORE_U64(x, y) (RB2_STORE(x, y, 8), *(volatile u64*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap64(y))
#define simde_mm_store_si128(p, v) (RB2_MARK_HOST(p), rb2_watch_host((const void*)(p), (const unsigned char*)base, __func__), simde_mm_store_si128(p, v))
#define simde_mm_store_ps(p, v) (RB2_MARK_HOST(p), rb2_watch_host((const void*)(p), (const unsigned char*)base, __func__), simde_mm_store_ps(p, v))
#define __sync_bool_compare_and_swap(p, o, n) (RB2_MARK_HOST(p), __sync_bool_compare_and_swap(p, o, n))
'''

# Which argument registers a CRI function reads before writing them (bit 0..3 = r3..r6, bit 4 = f1),
# from the generated text in order; a call before the first write counts as a read (conservative).
# The call boundary compares only those (leftover registers differ between live and resim calls).
# rollback3: the game's sound manager API (game code 0x82109C00-0x8210C3A0; its state at 0x8362xxxx-0x837Dxxxx
# is in the never-rewound range 0x83600000-0x838C0000) - entry points called from gameplay code.
SOUND_API = {0x82109C00, 0x8210A200, 0x8210A780, 0x8210B740, 0x8210BAA0, 0x8210C0B0, 0x8210C198, 0x8210C3A0,
             0x8210AD30, 0x8210B0D0, 0x8210AC30, 0x8210ACE0, 0x8210BE30}

# rollback3: the sound engine's lower layer at 0x824E6BD8-0x824F21F0 (calls CRI, reached from the game only
# through the sound manager and vtables; its objects live in the never-rewound sound range): same as CRI.
# List = closure computed from the generated call graph (scripts/sound_lib2.txt).
SOUND_LIB2 = set(int(x, 16) for x in (pathlib.Path(__file__).with_name('sound_lib2.txt').read_text().split()))

def arg_liveness(text, ranges):
    out = {}
    fpat2 = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{$', re.M)
    starts = [(m.start(), int(m.group(1), 16)) for m in fpat2.finditer(text)]
    regs = ['r3', 'r4', 'r5', 'r6', 'f1']
    for idx, (pos, a) in enumerate(starts):
        if not any(lo <= a < hi for lo, hi in ranges):
            continue
        end = starts[idx + 1][0] if idx + 1 < len(starts) else len(text)
        body = text[pos:end]
        mask = 0
        state = {r: None for r in regs}
        for line in body.split(chr(10))[1:]:
            t = line.strip()
            if not t or t.startswith('//'):
                continue
            is_call = re.search(r'sub_[0-9A-F]{8}\(ctx, base\)|PPC_CALL_INDIRECT|REX_CALL', t) is not None
            for bit, r in enumerate(regs):
                if state[r] is not None:
                    continue
                tok = 'ctx.' + r + '.'
                if tok in t:
                    lhs, eq, rhs = t.partition(' = ')
                    if eq and tok in lhs and tok not in rhs and lhs.strip().startswith(tok):
                        state[r] = 'w'
                    else:
                        state[r] = 'r'
                elif is_call or t.startswith('return'):
                    state[r] = 'r' if is_call else 'w'
            if all(v is not None for v in state.values()):
                break
        for bit, r in enumerate(regs):
            if state[r] == 'r':
                mask |= 1 << bit
        out[a] = mask
    return out

def main(d):
    d = pathlib.Path(d)
    pch = d / 'burstlimit_pch.h'
    t = pch.read_text(encoding='utf-8')
    if MARK not in t:
        old = t.find('// ---- rb2 write tracking')
        if old >= 0:
            t = t[:old]
        pch.write_text(t.rstrip('\n') + '\n\n' + BLOCK, encoding='utf-8')
        print('pch patched')
    n = 0
    pat = re.compile(r'(?<!RB2_MARK\(ea\), )(?<!RB2_MARKN\(ea, \d\d\), )(?<!RB2_MARKN\(ea, \d\d\d\), )memset\(\(void\*\)REX_RAW_ADDR\(ea\), 0, (\d+)\)')
    for f in sorted(d.glob('burstlimit_recomp.*.cpp')):
        s = f.read_text(encoding='utf-8')
        s = s.replace('RB2_MARK(ea), memset(', 'memset(')
        s = re.sub(r'RB2_MARKN\(ea, \d+\), memset\(', 'memset(', s)
        s2, k = pat.subn(r'RB2_MARKN(ea, \1), memset((void*)REX_RAW_ADDR(ea), 0, \1)', s)
        if k:
            f.write_text(s2, encoding='utf-8')
            n += k
    print('dcbz sites patched', n)
    # v10b: no raw stack stores (v11 / v11b made own-frame stack stores raw: both broke the offline stress - v11 lost
    # caller-frame writes of answered CRI calls, v11b still crashed 2 of 3 Frieza vs Cell runs and corrupted an effect
    # command's params; the CPU gain wasn't measurable). Undo any earlier rewrite.
    nr = 0
    for f in sorted(d.glob('burstlimit_recomp.*.cpp')):
        s0 = f.read_text(encoding='utf-8')
        if 'REX_STORE_RAW_U' in s0:
            f.write_text(s0.replace('REX_STORE_RAW_U', 'REX_STORE_U'), encoding='utf-8')
            nr += 1
    print('files with raw stack stores restored', nr)
    # sound library code (CRI): its stores are the sound engine's, never rewound
    # D3D library 0x820A0000-0x820BFA38, XAudio / XMA after it, CRI 0x82440000-0x824A7000
    # (tried 2026-10-24: also D3D 0x820A0000-0x820BFA38 and 0x820BFA38-0x820D0000 - the fight RNG and the
    # job pool broke at once: those ranges hold code whose writes are frame state)
    ranges = [(0x82440000, 0x824A7000)]
    fpat = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{$', re.M)
    k = 0
    for f in sorted(d.glob('burstlimit_recomp.*.cpp')):
        s = f.read_text(encoding='utf-8')
        s = s.replace(chr(10) + chr(9) + 'RB2_KIND(16);', '')  # re-tag from scratch
        s = re.sub(chr(10) + chr(9) + r'RB2_(CRI|D3D)_ENTRY\(0x[0-9A-F]{8}(, 0x[0-9A-F]+)?\);', '', s)
        live = arg_liveness(s, ranges + [(a, a + 1) for a in SOUND_API | SOUND_LIB2])
        def rep(mo):
            nonlocal k
            a = int(mo.group(1), 16)
            if any(lo <= a < hi for lo, hi in ranges) or a in SOUND_LIB2:
                k += 1
                return mo.group(0) + chr(10) + chr(9) + 'RB2_KIND(16);' + chr(10) + chr(9) + 'RB2_CRI_ENTRY(0x%08X, 0x%X);' % (a, live.get(a, 0x1F))
            if a in SOUND_API:  # the game's sound manager (state never rewound): same call boundary, game-code writes
                k += 1
                return mo.group(0) + chr(10) + chr(9) + 'RB2_CRI_ENTRY(0x%08X, 0x%X);' % (a, live.get(a, 0x1F))
            if 0x820A0000 <= a < 0x820BFA38:  # D3D library: counted when the main thread enters it in a resim
                return mo.group(0) + chr(10) + chr(9) + 'RB2_D3D_ENTRY(0x%08X);' % a
            return mo.group(0)
        s2 = fpat.sub(rep, s)
        if s2 != s:
            f.write_text(s2, encoding='utf-8')
    print('sound functions tagged', k)

main(sys.argv[1])
