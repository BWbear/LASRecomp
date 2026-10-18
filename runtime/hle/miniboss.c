// Mini-bosses on the overworld (enemy randomizer, "Include mini-bosses"; kinds in LAS enemies.py FIELD_MINIBOSSES).
// Armos Knight, Giant Buzz Blob and Dodongo Snakes fight on their own outside their arenas. Hinox sits in its idle
// state until its dungeon room starts it: entering the room's camera zone (zone manager -> f_7000029520 -> actor
// vfunc +0x100) hands a callback to the room's AreaEventBox, Link inside the box fires it, the actor's next state
// requests the HinoxStart event (BGM, camera, shutters) and that event's end moves it into its fight state. The Field
// has no zones or boxes, so here the runtime moves it into the fight state itself (no start event: no boss music or
// shutter steps) once Link comes near. Actors are seen through a per-frame type check (tools/patches.py
// MB_SEEN_SITE) and picked out by class vtable.
#include "../game_addresses.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

void guest_call(Cpu *c, uint64_t addr);
uint64_t doorjump_player(uint64_t frame);  // PlayerCollision component: position at +0x80

// GA_PLAYER_STATUS_PTR: PlayerStatus*: current level name at +0

// per class: vtable, its state machine's change(sm, state) (+0xd current state, +0xe previous; states 0x30 apart
// from +0x40: ctx, enter, exec, exit member functions), the state machine's offset in the actor, idle state, fight
// state, position offset (x y z floats: the spawn point), rest state (0xff: none) - one it stays in after a move
// that ends its fight in its own room (Blaino's knock-out): woken again like idle, then (0: none) - a state it goes
// on to a few frames after waking (Master Stalfos: 13 shows him, 1 is his fight), at_once: woken as soon as he's
// loaded, wherever Link is (an idle Master Stalfos is invisible), start (main offset, 0: none) - its own start action,
// called with the actor instead of the state change (Lanmola: what his screen's event runs - up out of the sand),
// arm (bosses): instead, the start of its dungeon room - the zone manager's room-entry call (f_7000029520) once it's
// there; Link walking into its AreaEventBox then starts its fight - or, with chg, straight into its fight state
typedef struct {
    uint64_t vt, chg; uint32_t sm, pos; uint8_t idle, fight; const char *name; uint8_t rest, then, at_once; uint32_t start; uint8_t arm;
} Kind;
static const Kind g_kinds[] = {
    {GA_HINOX_VTABLE, GA_HINOX_CHANGE_STATE, 0xff0, 0x260, 0, 1, "Hinox", 0xff},                          // 0x53: 10 = request HinoxStart
    // (found 2026-10-12 with tools placing each one on Field_13D: LARECOMP_MB_SCAN_NAME / _FORCE)
    {GA_SMASHER_VTABLE, GA_SMASHER_CHANGE_STATE, 0x1110, 0x260, 0, 1, "Smasher", 0xff},                   // 0x5c (+ iron ball)
    {GA_AVALAUNCH_VTABLE, GA_AVALAUNCH_CHANGE_STATE, 0xff0, 0x260, 0, 1, "Avalaunch", 0xff},              // 0x60
    {GA_ROLLING_BONES_VTABLE, GA_ROLLING_BONES_CHANGE_STATE, 0xff0, 0x260, 0, 1, "Rolling Bones", 0xff},  // 0x51 (+ spike bar)
    {GA_GOHMA_VTABLE, GA_GOHMA_CHANGE_STATE, 0x1200, 0x260, 13, 0, "Gohma", 0xff},                        // 0x5a (heads for Link: GOHMA_*)
    {GA_BLAINO_VTABLE, GA_BLAINO_CHANGE_STATE, 0x11a0, 0x260, 10, 1, "Blaino", 5},                        // 0x5f (5: after a knock-out)
    {GA_CUE_BALL_VTABLE, GA_CUE_BALL_CHANGE_STATE, 0xff0, 0x260, 0, 3, "Cue Ball", 0xff},                 // 0x58 (+ a rail fitted)
    // 0x57: 0 = under the sand until his screen's event (AreaEventBox - not copied) runs his start; back in 0, again
    {GA_LANMOLA_VTABLE, GA_LANMOLA_CHANGE_STATE, 0xff0, 0x260, 0, 1, "Lanmola", 0, 0, 0, (GA_LANMOLA_START - MAIN_BASE)},
    // 0x59: 12 = hidden until his room's event (AreaEventBox - not copied: intro, music) drops him in; 13 shows him
    {GA_MASTER_STALFOS_VTABLE, GA_MASTER_STALFOS_CHANGE_STATE, 0x1230, 0x260, 12, 13, "Master Stalfos", 0xff, 1, 1},
    // bosses (enemy randomizer "Include bosses"): started by their own event (arm)
    {MAIN_BASE + 0x1b71f38, MAIN_BASE + 0x2a0840, 0xff0, 0x260, 6, 4, "Moldorm", 0xff, 0, 0, 0, 1},  // 0x63: 6 = waits for
    // its start box (armed); 4 = what the box's callback (main+0x2a1200) does: its Start event (an overworld copy's
    // only shows it - tools/rando/boss_guard.py), then it crawls (0). 17 = paused while Link is screens away
    {MAIN_BASE + 0x1b9fe60, 0, 0x1340, 0x260, 0, 0, "Genie", 0xff, 0, 0, 0, 1},      // 0x64
    {MAIN_BASE + 0x1b91228, 0, 0xff0, 0x260, 0, 0, "Facade", 0xff, 0, 0, 0, 1},      // 0x68
    {MAIN_BASE + 0x1b70940, 0, 0xff0, 0x260, 0, 0, "Hot Head", 0xff, 0, 0, 0, 1},    // 0x6a
    {MAIN_BASE + 0x1b792e8, 0, 0xff0, 0x260, 0, 0, "Hardhit Beetle", 0xff, 0, 0, 0, 1},  // 0x62 (14: paused while Link is away)
};
#define NKINDS (int)(sizeof g_kinds / sizeof g_kinds[0])
#define WAKE_DIST 7.0f  // (a Field screen is ~15 x 12 units)

typedef struct { uint64_t obj, first, last, woken_at; int kind, woken; float px, pz; int stuck; uint64_t flip_at; } Seen;
static Seen g_seen[32];
RT_ON_RESET(miniboss_reset) { memset(g_seen, 0, sizeof g_seen); }
extern uint64_t renderer_present_count(void);

static void log_new_vtable(uint64_t o, uint64_t vt) {  // LARECOMP_MB_LOG (RE): every actor class seen, once
    static uint64_t vts[512];
    static int n;
    for (int i = 0; i < n; i++)
        if (vts[i] == vt) return;
    if (n < 512) vts[n++] = vt;
    char name[40] = "";  // (the actor's level name, "EnemyRola-<key>", when it keeps one at +0x398 like item actors)
    for (uint64_t off = 0; off < 0x3000 && !name[0]; off += 8) {
        const char *nm = (const char *)(uintptr_t)(o + off);
        if (memcmp(nm, "Enemy", 5) && memcmp(nm, "Obj", 3) && memcmp(nm, "Npc", 3)) continue;
        for (int i = 0; i < 39 && nm[i] >= 0x20 && nm[i] < 0x7f; i++) name[i] = nm[i], name[i + 1] = 0;
        snprintf(name + strlen(name), sizeof name - strlen(name), "@%llx", (unsigned long long)off);
    }
    LOG("[mb] new class vtable main+%#llx (actor %llx, +0x100 main+%#llx) %s\n", (unsigned long long)(vt - MAIN_BASE),
        (unsigned long long)o, (unsigned long long)(LD64(vt + 0x100) - MAIN_BASE), name);
}

// LARECOMP_MB_SCAN=<vtable offsets, hex, comma-separated> (RE): finds the state machines inside actors of those classes
// (ctx pointer at +0x40 = the actor, state 0's enter function at +0x48 / +0x50), lists their states and logs changes
static uint64_t g_scan_vt[16], g_scan_obj[8], g_scan_sm[8][8];
static uint8_t g_scan_last[8][8];
static int g_scan_nv = -1;

static bool code_ptr(uint64_t p) { return p >= MAIN_BASE && p < GA_MAIN_CODE_SCAN_END; }

static void scan_seen(uint64_t o, uint64_t vt) {
    if (g_scan_nv < 0) {
        g_scan_nv = 0;
        for (const char *p = ENV("LARECOMP_MB_SCAN"); p && *p && g_scan_nv < 16;) {
            g_scan_vt[g_scan_nv++] = MAIN_BASE + strtoull(p, (char **)&p, 16);
            while (*p == ',') p++;
        }
    }
    int k = 0;
    while (k < g_scan_nv && g_scan_vt[k] != vt) k++;
    // LARECOMP_MB_SCAN_NAME=<text>: (also) actors whose level name (+0x18, "EnemyOctorok-<key>") contains it - a
    // placed test actor keeps the name of the one it replaced; classes share base vtables
    const char *want = ENV("LARECOMP_MB_SCAN_NAME");
    bool named = want && strstr((const char *)(uintptr_t)(o + 0x18), want);
    if (k == g_scan_nv && !named) return;
    int fr = -1;
    for (int i = 0; i < 8; i++) {
        if (g_scan_obj[i] == o) return;
        if (fr < 0 && !g_scan_obj[i]) fr = i;
    }
    if (fr < 0) return;
    g_scan_obj[fr] = o;
    memset(g_scan_sm[fr], 0, sizeof g_scan_sm[fr]);
    int n = 0;
    for (uint64_t off = 0; off < 0x2400 && n < 8; off += 8)
        if (LD64(o + off + 0x40) == o && (code_ptr(LD64(o + off + 0x48)) || code_ptr(LD64(o + off + 0x50)))) {
            g_scan_sm[fr][n++] = off;
            g_scan_last[fr][n - 1] = *(uint8_t *)(uintptr_t)(o + off + 0xd);
            LOG("[mb-scan] actor %llx (main+%#llx): state machine at +%#llx, state %u\n", (unsigned long long)o,
                (unsigned long long)(vt - MAIN_BASE), (unsigned long long)off, g_scan_last[fr][n - 1]);
            for (int s = 0; s < 24; s++) {  // (entries: ctx, then enter / exec / exit member pointers, 16 bytes each)
                uint64_t e = o + off + 0x40 + s * 0x30, f1 = LD64(e + 8), f2 = LD64(e + 0x18), f3 = LD64(e + 0x28);
                if (!code_ptr(f1) && !code_ptr(f2) && code_ptr(LD64(e + 0x10))) f1 = LD64(e + 0x10), f2 = LD64(e + 0x20), f3 = 0;
                if (code_ptr(f1) || code_ptr(f2) || code_ptr(f3))
                    LOG("[mb-scan]   state %d: enter %llx exec %llx exit %llx\n", s, (unsigned long long)f1,
                        (unsigned long long)f2, (unsigned long long)f3);
            }
        }
}

// LARECOMP_MB_DIFF (RE, with LARECOMP_MB_SCAN): at each state change of a scanned actor, its 32-bit fields (first
// 0x2000 bytes) that went down by 1..64 since the last change - its hit points among them
static void scan_diff(int i, uint64_t o) {
    static uint32_t snap[8][0x800];
    static uint64_t snap_obj[8];
    const uint32_t *now = (const uint32_t *)(uintptr_t)o;
    if (snap_obj[i] == o)
        for (int w = 0; w < 0x800; w++)
            if (now[w] < snap[i][w] && snap[i][w] - now[w] <= 64 && snap[i][w] < 0x10000)
                LOG("[mb-diff]   +%#x: %u -> %u\n", w * 4, snap[i][w], now[w]);
    memcpy(snap[i], now, sizeof snap[i]);
    snap_obj[i] = o;
}

static void scan_tick(uint64_t frame) {
    for (int i = 0; i < 8; i++) {
        uint64_t o = g_scan_obj[i];
        if (!o) continue;
        int alive = ENV("LARECOMP_MB_SCAN_NAME") && strstr((const char *)(uintptr_t)(o + 0x18), ENV("LARECOMP_MB_SCAN_NAME"));
        for (int k = 0; k < g_scan_nv; k++) alive |= LD64(o) == g_scan_vt[k];
        if (!alive) { g_scan_obj[i] = 0; continue; }
        for (int j = 0; j < 8 && g_scan_sm[i][j]; j++) {
            uint8_t st = *(uint8_t *)(uintptr_t)(o + g_scan_sm[i][j] + 0xd);
            if (st != g_scan_last[i][j]) {
                LOG("[mb-scan] f%llu actor %llx sm +%#llx: %u -> %u\n", (unsigned long long)frame, (unsigned long long)o,
                    (unsigned long long)g_scan_sm[i][j], g_scan_last[i][j], st);
                if (ENV("LARECOMP_MB_DIFF") && j == 0) scan_diff(i, o);
            }
            g_scan_last[i][j] = st;
        }
    }
}

void rt_mb_seen(uint64_t o) {
    uint64_t vt = LD64(o);
    if (ENV("LARECOMP_MB_LOG")) log_new_vtable(o, vt);
    if (ENV("LARECOMP_MB_SCAN") || ENV("LARECOMP_MB_SCAN_NAME")) scan_seen(o, vt);
    int k = 0;
    while (k < NKINDS && g_kinds[k].vt != vt) k++;
    if (k == NKINDS) return;
    uint64_t f = renderer_present_count();
    int free_i = -1;
    for (int i = 0; i < 32; i++) {
        if (g_seen[i].obj == o) { g_seen[i].last = f; return; }
        if (free_i < 0 && (!g_seen[i].obj || f - g_seen[i].last > 120)) free_i = i;
    }
    if (free_i >= 0) g_seen[free_i] = (Seen){o, f, f, 0, k, 0, 0, 0, 0, 0};
}

// LARECOMP_MB_FORCE=<frame>:<sm offset hex>:<state>[:<change fn main offset hex>] (RE, with LARECOMP_MB_SCAN_NAME):
// at that frame the scanned actor's state machine changes to that state (default change: Hinox's, main+0x3bfc60)
static uint64_t scan_latest(void) {  // (the newest scanned actor: a reloaded room leaves stale copies in the list)
    for (int i = 7; i >= 0; i--)
        if (g_scan_obj[i]) return g_scan_obj[i];
    return 0;
}

static void force_tick(Cpu *c, uint64_t frame) {
    static long at = -2;
    static uint64_t sm, chg;
    static int state;
    if (at == -2) {
        at = -1;
        const char *e = ENV("LARECOMP_MB_FORCE");
        unsigned long long smo = 0, ch = GA_HINOX_CHANGE_STATE - MAIN_BASE;
        if (e && sscanf(e, "%ld:%llx:%d:%llx", &at, &smo, &state, &ch) >= 3) sm = smo, chg = MAIN_BASE + ch;
        else at = -1;
    }
    static long zat = -2;  // LARECOMP_MB_ZONE=<frame>: the dungeon zone manager's room-entry call on the scanned actor
    if (zat == -2) zat = ENV("LARECOMP_MB_ZONE") ? atol(ENV("LARECOMP_MB_ZONE")) : -1;
    if (zat >= 0 && (long)frame == zat && scan_latest()) {
        uint64_t saved[31], sp = c->sp;
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = scan_latest();
        guest_call(c, GA_AREA_EVENT_TRIGGER);
        memcpy(c->x, saved, sizeof saved);
        c->sp = sp;
        LOG("[mb-zone] f%llu actor %llx: zone entry call\n", (unsigned long long)frame, (unsigned long long)scan_latest());
    }
    if (at < 0 || (long)frame != at || !scan_latest()) return;
    uint64_t saved[31], sp = c->sp;
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = scan_latest() + sm;
    c->x[1] = (uint64_t)state;
    guest_call(c, chg);
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp;
    LOG("[mb-force] f%llu actor %llx sm +%#llx -> state %d\n", (unsigned long long)frame, (unsigned long long)scan_latest(),
        (unsigned long long)sm, state);
}

// A copy of a mini-boss whose flow gives a check (Lanmola, Armos Knight) skips the item when its switch 2 - the seed's
// OverworldMinibossMarker flag (settings.json "miniboss-marker-flag", tools/rando/miniboss_guard.py) - is set: set
// every frame on the Field (an Armos Knight copy fights on its own, nothing wakes it). The originals have no switch 2
// set: they give it as before. An overworld boss's end event skips its dungeon's boss item the same way: its switch 2
// is the seed's OverworldBossMarker flag ("boss-marker-flag", tools/rando/boss_guard.py), set whenever one is armed.
// Written through the flag object (main+0x1CC1388, its byte capacity +0x1CC1390): the flag may be past the game's
// 2048 (runtime/hle/flags_ext.c).
static int g_markers[2] = {-2, -2};  // (mini-boss, boss)
RT_ON_RESET(miniboss_marker_reset) { g_markers[0] = g_markers[1] = -2; }
static void marker_set(int boss) {
    int *g = &g_markers[boss];
    if (*g == -2) {
        *g = -1;
        extern const char *rando_seed_dir(void);
        const char *dir = rando_seed_dir();
        char p[512];
        FILE *f = dir ? (snprintf(p, sizeof p, "%s/settings.json", dir), fopen(p, "rb")) : NULL;
        if (f) {
            static char buf[1 << 16];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
            const char *key = boss ? "\"boss-marker-flag\":" : "\"miniboss-marker-flag\":", *k = strstr(buf, key);
            if (k) *g = atoi(k + strlen(key));
        }
    }
    int g_marker = *g;
    if (g_marker < 0) return;
    uint8_t *fl = (uint8_t *)(uintptr_t)LD64(GA_FLAG_PTR);
    if (!fl || (uint32_t)(g_marker >> 3) >= LD32(GA_FLAG_CAP)) return;
    fl[g_marker >> 3] |= (uint8_t)(1 << (g_marker & 7));
}

// Gohma on the Field (tools/patches.py GOHMA_*): its moves head for Link (angle units: 2^31 = pi, 0 = +z) - or, with
// a wall that way, the nearest open direction - and have no track end. Its arena's walls are the track ends; here the
// screen's walls (rando/<seed>/roam.tsv, tools/rando/roam.py) end a move the way its own timer does (gohma_walls).
// g_link: Link's spot (miniboss_tick).
static float g_link[3];
static bool g_have_link;

typedef struct { float x0, z0, ts; int w, h; char cells[96]; } WallScreen;
static WallScreen *g_walls;
static int g_nwalls = -1;
RT_ON_RESET(walls_reset) { free(g_walls), g_walls = NULL, g_nwalls = -1; }

static void walls_load(void) {
    extern const char *rando_seed_dir(void);
    g_nwalls = 0;
    const char *dir = rando_seed_dir();
    char path[1024];
    if (!dir) return;
    snprintf(path, sizeof path, "%s/roam.tsv", dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    g_walls = calloc(512, sizeof *g_walls);
    char cells[128];
    WallScreen w;
    while (g_nwalls < 512 && fscanf(f, "%f\t%f\t%f\t%d\t%d\t%127s", &w.x0, &w.z0, &w.ts, &w.w, &w.h, cells) == 6)
        if (w.w * w.h <= (int)sizeof w.cells && (int)strlen(cells) == w.w * w.h)
            memcpy(w.cells, cells, (size_t)(w.w * w.h)), g_walls[g_nwalls++] = w;
    fclose(f);
}

#define GOHMA_BODY 1.0f  // (half its width)
static bool wall_at(float x, float z) {  // (Gohma's body around (x, z) touches a wall or the island's edge)
    if (g_nwalls < 0) walls_load();
    if (!g_nwalls) return false;
    for (int k = 0; k < 5; k++) {
        float px = x + (k == 1 ? GOHMA_BODY : k == 2 ? -GOHMA_BODY : 0), pz = z + (k == 3 ? GOHMA_BODY : k == 4 ? -GOHMA_BODY : 0);
        bool open = false;
        for (int i = 0; i < g_nwalls && !open; i++) {
            const WallScreen *r = &g_walls[i];
            int cx = (int)floorf((px - r->x0) / r->ts), cz = (int)floorf((pz - r->z0) / r->ts);
            if (cx >= 0 && cx < r->w && cz >= 0 && cz < r->h) open = r->cells[cz * r->w + cx] == '.';
        }
        if (!open) return true;
    }
    return false;
}
int rt_gohma_free(void) {
    extern const char *rando_seed_dir(void);
    uint64_t ps = LD64(GA_PLAYER_STATUS_PTR);
    return rando_seed_dir() && ps && !strcmp((const char *)(uintptr_t)ps, "Field");
}

uint32_t rt_gohma_heading(uint64_t gohma, uint32_t track) {
    uint64_t comp = LD64(gohma + 0x80);
    if (!rt_gohma_free() || !g_have_link || !comp) return track;
    const float *p = (const float *)(uintptr_t)(comp + 0x80);
    float dx = g_link[0] - p[0], dz = g_link[2] - p[2];
    if (dx * dx + dz * dz < 0.01f) return track;
    float a = atan2f(dx, dz);
    for (int t = 0; t < 8; t++) {  // (toward Link, else the nearest open way: 45 degree steps either side)
        float b = a + (t + 1) / 2 * 0.7853982f * (t % 2 ? 1 : -1);
        if (!wall_at(p[0] + sinf(b) * 1.5f, p[2] + cosf(b) * 1.5f)) { a = b; break; }
    }
    if (ENV("LARECOMP_LOG_ROAM")) LOG("[roam] Gohma at %.1f %.1f heads %.0f deg (Link %.1f %.1f)\n", p[0], p[2], a * 57.29578f, g_link[0], g_link[2]);
    return (uint32_t)(int32_t)(a * (2147483648.0f / 3.14159265f));
}

// Each frame: a wall just ahead on Gohma's heading ends its move (its timer +0x15a4 reaches its limit +0x15a6, so its
// own next state follows), and this frame's step is dropped so it doesn't push in.
static void gohma_walls(uint64_t gohma) {
    uint64_t comp = LD64(gohma + 0x80);
    if (!comp) return;
    const float *p = (const float *)(uintptr_t)(comp + 0x80);
    float speed = *(float *)(uintptr_t)(gohma + 0x15e4);
    if (speed <= 0) return;
    float a = (float)*(int32_t *)(uintptr_t)(gohma + 0x15ec) * (3.14159265f / 2147483648.0f);
    float ahead = speed + 0.1f;
    if (!wall_at(p[0] + sinf(a) * ahead, p[2] + cosf(a) * ahead)) return;
    *(float *)(uintptr_t)(gohma + 0x15e4) = 0;
    *(int16_t *)(uintptr_t)(gohma + 0x15a4) = *(int16_t *)(uintptr_t)(gohma + 0x15a6);
    if (ENV("LARECOMP_LOG_ROAM")) LOG("[roam] Gohma at %.1f %.1f: wall ahead, move ends\n", p[0], p[2]);
}

// Cue Ball on the Field rolls the loop of four corners fitted to its screen (enemies.py fit_rail): state 2 rolls to
// its first corner, state 7 to the next, state 8 turns there - the next corner one step along its direction (+0x1731: 0 forward, else
// back; corners at +0x1410, the one it heads for +0x1760). Here something in its way (a bush) would hold it forever:
// stuck a moment, or now and then at random, it turns back - its own turn with its direction flipped.
static void set_state(Cpu *c, const Seen *s, const Kind *k, int state) {  // (its own state change)
    uint64_t saved[31], sp = c->sp;
    memcpy(saved, c->x, sizeof saved);
    c->x[0] = s->obj + k->sm;
    c->x[1] = (uint64_t)state;
    guest_call(c, k->chg);
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp;
}

// Stuck a moment (moved under 0.02 a frame for 20 frames) or due for a change (every 3 - 9 s): tracked per mini-boss.
static bool stuck_or_due(Seen *s, const float *p, bool moving, uint64_t frame) {
    float dx = p[0] - s->px, dz = p[2] - s->pz;
    s->px = p[0], s->pz = p[2];
    if (!s->flip_at) s->flip_at = frame + 180 + (uint32_t)(s->obj >> 4) % 360;
    if (!moving) { s->stuck = 0; return false; }
    s->stuck = dx * dx + dz * dz < 0.0004f ? s->stuck + 1 : 0;
    if (s->stuck < 20 && frame < s->flip_at) return false;
    s->stuck = 0;
    s->flip_at = frame + 180 + (uint32_t)(frame * 2654435761u >> 8) % 360;
    return true;
}

// Avalaunch rolls out from its spawn point on a random spoke (state 2) and back to it (state 3). Now and then - or
// stuck - it switches: out turns back home early, home heads out on a new spoke.
static void avalaunch_tick(Cpu *c, Seen *s, const Kind *k, uint64_t frame) {
    uint64_t comp = LD64(s->obj + 0x80);
    if (!comp) return;
    const float *p = (const float *)(uintptr_t)(comp + 0x80);
    uint8_t st = *(uint8_t *)(uintptr_t)(s->obj + k->sm + 0xd);
    if (!stuck_or_due(s, p, st == 2 || st == 3, frame)) return;
    set_state(c, s, k, st == 2 ? 3 : 2);
    if (ENV("LARECOMP_LOG_ROAM")) LOG("[roam] Avalaunch at %.1f %.1f switches (%s)\n", p[0], p[2], st == 2 ? "home" : "out");
}

static void cueball_tick(Cpu *c, Seen *s, const Kind *k, uint64_t frame) {
    uint64_t comp = LD64(s->obj + 0x80);
    if (!comp) return;
    const float *p = (const float *)(uintptr_t)(comp + 0x80);
    uint8_t st = *(uint8_t *)(uintptr_t)(s->obj + k->sm + 0xd);
    // (2: rolling to its first corner - only stuck turns it there; 7: to the next corner)
    bool due_ok = st == 7;
    int was_stuck = s->stuck;
    if (!stuck_or_due(s, p, st == 7 || st == 2, frame)) return;
    if (!due_ok && was_stuck < 19) { s->flip_at = 0; return; }  // (state 2: a random turn waits for the loop)
    *(uint8_t *)(uintptr_t)(s->obj + 0x1731) ^= 1;
    set_state(c, s, k, 8);
    if (ENV("LARECOMP_LOG_ROAM")) LOG("[roam] Cue Ball at %.1f %.1f turns back\n", p[0], p[2]);
}

// Moldorm on the Field crawls at its own height wherever its heading takes it (its arena is a flat floor in walls):
// here a wall, water or the edge of its own screen just ahead (rando/<seed>/roam.tsv) turns it back - its heading
// (+0x148c, angle units as Gohma's) mirrored off that side, as off its arena's walls.
static bool blocked_at(float x, float z, float sx0, float sz0) {  // (its screen: x sx0 .. +15, z sz0 .. +12)
    if (x < sx0 + 0.5f || x > sx0 + 14.5f || z < sz0 + 0.5f || z > sz0 + 11.5f) return true;
    if (g_nwalls < 0) walls_load();
    for (int i = 0; i < g_nwalls; i++) {
        const WallScreen *r = &g_walls[i];
        int cx = (int)floorf((x - r->x0) / r->ts), cz = (int)floorf((z - r->z0) / r->ts);
        if (cx >= 0 && cx < r->w && cz >= 0 && cz < r->h) return r->cells[cz * r->w + cx] != '.';
    }
    return false;
}

#define MOLDORM_AHEAD 1.0f  // (its head's reach)
static void moldorm_walls(Seen *s, const Kind *k, uint64_t frame) {
    uint64_t comp = LD64(s->obj + 0x80);
    if (!comp || frame < s->flip_at) return;
    const float *p = (const float *)(uintptr_t)(comp + 0x80), *home = (const float *)(uintptr_t)(s->obj + k->pos);
    float sx0 = floorf(home[0] / 15.0f) * 15.0f, sz0 = floorf(home[2] / 12.0f) * 12.0f;
    int32_t *heading = (int32_t *)(uintptr_t)(s->obj + 0x148c);
    float a = (float)*heading * (3.14159265f / 2147483648.0f), dx = sinf(a), dz = cosf(a);
    if (!blocked_at(p[0] + dx * MOLDORM_AHEAD, p[2] + dz * MOLDORM_AHEAD, sx0, sz0)) return;
    bool side_x = blocked_at(p[0] + dx * MOLDORM_AHEAD, p[2], sx0, sz0),  // (which way the wall is)
         side_z = blocked_at(p[0], p[2] + dz * MOLDORM_AHEAD, sx0, sz0);
    if (side_x || !side_z) dx = -dx;
    if (side_z || !side_x) dz = -dz;
    *heading = (int32_t)(atan2f(dx, dz) * (2147483648.0f / 3.14159265f));
    s->flip_at = frame + 8;  // (a moment to turn away before the next check)
    if (ENV("LARECOMP_LOG_ROAM")) LOG("[roam] Moldorm at %.1f %.1f turns back\n", p[0], p[2]);
}

// Moldorm's five parts each have a CharaBodyCollision (actor +0x1548 .. +0x1588, every 0x10): in his arena he shoves
// Link around with them, on the Field they were a wall Link couldn't damage-boost through. Switched off there (the
// component's own enable, vfunc +0x58 with false) as he starts; his body segments hurt instead (rt_moldorm_attacks).
static void moldorm_solid_off(Cpu *c, uint64_t actor) {
    uint64_t saved[31], sp = c->sp;
    memcpy(saved, c->x, sizeof saved);
    for (int part = 0; part < 5; part++) {
        uint64_t comp = LD64(actor + 0x1548 + part * 0x10);
        if (!comp || LD64(comp) != MAIN_BASE + 0x1c4fed0) continue;
        c->x[0] = comp, c->x[1] = 0;
        guest_call(c, LD64(LD64(comp) + 0x58));
    }
    memcpy(c->x, saved, sizeof saved);
    c->sp = sp;
}

// Blaino's knock-out (tools/patches.py CRASHOUT_SITE): on the Field Link just falls back down - no fade, no restart
// warp (its start only exists in his ring).
int rt_crashout_skip(void) { return rt_gohma_free(); }

static void moldorm_attacks_tick(Cpu *c);
void miniboss_tick(Cpu *c, uint64_t frame) {
    if (ENV("LARECOMP_MB_SCAN") || ENV("LARECOMP_MB_SCAN_NAME")) scan_tick(frame);
    force_tick(c, frame);
    extern const char *rando_seed_dir(void);
    if (!rando_seed_dir()) return;  // (only seeds put mini-bosses on the Field)
    uint64_t ps = LD64(GA_PLAYER_STATUS_PTR);
    if (!ps || strcmp((const char *)(uintptr_t)ps, "Field")) return;  // (dungeon arenas start their own)
    uint64_t pc = doorjump_player(frame);
    if (!pc) return;
    const float *lp = (const float *)(uintptr_t)(pc + 0x80);
    memcpy(g_link, lp, sizeof g_link), g_have_link = true;
    marker_set(0);
    moldorm_attacks_tick(c);
    if (ENV("LARECOMP_LOG_MB_ST") && frame % 120 == 0)  // (RE: which are still updated)
        for (int i = 0; i < 32; i++)
            if (g_seen[i].obj)
                LOG("[mb-seen] f%llu %s %llx last seen f%llu vt %s, st %u, flags +0x43 %02x\n", (unsigned long long)frame,
                    g_kinds[g_seen[i].kind].name, (unsigned long long)g_seen[i].obj, (unsigned long long)g_seen[i].last,
                    LD64(g_seen[i].obj) == g_kinds[g_seen[i].kind].vt ? "ok" : "gone",
                    *(uint8_t *)(uintptr_t)(g_seen[i].obj + g_kinds[g_seen[i].kind].sm + 0xd), LD8(g_seen[i].obj + 0x43));
    for (int i = 0; i < 32; i++) {
        Seen *s = &g_seen[i];
        if (!s->obj || frame - s->last > 2 || LD64(s->obj) != g_kinds[s->kind].vt) continue;
        const Kind *k = &g_kinds[s->kind];
        if (ENV("LARECOMP_LOG_MB_ST") && LD64(s->obj + 0x80)) {  // (RE: state changes against Link's distance)
            static uint8_t last[32];
            uint8_t st = *(uint8_t *)(uintptr_t)(s->obj + k->sm + 0xd);
            const float *q = (const float *)(uintptr_t)(LD64(s->obj + 0x80) + 0x80);
            if (st != last[i] || frame % 120 == 0)
                LOG("[mb-st] f%llu %s %llx st %u at %.1f %.1f Link %.1f %.1f (%.1f away)\n", (unsigned long long)frame,
                    k->name, (unsigned long long)s->obj, st, q[0], q[2], lp[0], lp[2],
                    sqrtf((q[0] - lp[0]) * (q[0] - lp[0]) + (q[2] - lp[2]) * (q[2] - lp[2])));
            last[i] = st;
        }
        // (a boss is armed as soon as it exists - the game loads the next screen's enemies before Link walks onto it,
        // so he enters its screen-wide box after that and it starts at once; others wait half a second)
        if (!k->arm && frame - s->first < 30) continue;
        if (k->arm) {
            if (s->woken && ENV("LARECOMP_LOG_MB_Y") && frame % 10 == 0 && LD64(s->obj + 0x80)) {  // (RE: its moves)
                const float *q = (const float *)(uintptr_t)(LD64(s->obj + 0x80) + 0x80);
                LOG("[mb-y] f%llu %s %llx st %u at %.2f %.2f %.2f\n", (unsigned long long)frame, k->name,
                    (unsigned long long)s->obj, *(uint8_t *)(uintptr_t)(s->obj + k->sm + 0xd), q[0], q[1], q[2]);
            }
            if (s->woken && !strcmp(k->name, "Moldorm")) moldorm_walls(s, k, frame);
            if (s->woken) continue;
            s->woken = 1;
            s->woken_at = frame;
            uint64_t saved[31], sp = c->sp;
            memcpy(saved, c->x, sizeof saved);
            c->x[0] = s->obj;
            guest_call(c, MAIN_BASE + 0x29520);  // (the zone manager's room-entry call: f_7000029520 -> vfunc +0x100)
            memcpy(c->x, saved, sizeof saved);
            c->sp = sp;
            if (k->chg) {  // (then straight into its fight - without waiting for Link to walk into its start box)
                c->x[0] = s->obj + k->sm;
                c->x[1] = k->fight;
                guest_call(c, k->chg);
                memcpy(c->x, saved, sizeof saved);
                c->sp = sp;
            }
            if (!strcmp(k->name, "Moldorm")) moldorm_solid_off(c, s->obj);
            LOG("[miniboss] f%llu %s armed%s\n", (unsigned long long)frame, k->name,
                k->chg ? " and started" : " (its start event plays when Link enters its box)");
            marker_set(1);
            continue;
        }
        if (!strcmp(k->name, "Gohma")) gohma_walls(s->obj);
        if (!strcmp(k->name, "Cue Ball")) cueball_tick(c, s, k, frame);
        if (!strcmp(k->name, "Avalaunch")) avalaunch_tick(c, s, k, frame);
        uint8_t st = *(uint8_t *)(uintptr_t)(s->obj + k->sm + 0xd);
        if (k->then && s->woken && st == k->fight && frame - s->woken_at >= 2) set_state(c, s, k, k->then);
        if (ENV("LARECOMP_LOG_MB_Y") && frame % 5 == 0 && s->woken && LD64(s->obj + 0x80)) {  // (RE: height by state)
            const float *q = (const float *)(uintptr_t)(LD64(s->obj + 0x80) + 0x80);
            LOG("[mb-y] f%llu %s %llx st %u at %.2f %.2f %.2f\n", (unsigned long long)frame, k->name,
                (unsigned long long)s->obj, st, q[0], q[1], q[2]);
        }
        // first wake from idle; again from its rest state (a while after the last wake: it went there itself)
        bool again = s->woken && k->rest != 0xff && st == k->rest && frame - s->woken_at > 180;
        if (s->woken ? !again : st != k->idle) continue;
        const float *p = (const float *)(uintptr_t)(s->obj + k->pos);
        // (Yarna Desert's own Lanmola, Field_13O at 217.5, 150: his screen's event starts him)
        if (!strcmp(k->name, "Lanmola") && fabsf(p[0] - 217.5f) < 1.5f && fabsf(p[2] - 150.0f) < 1.5f) continue;
        float dx = p[0] - lp[0], dz = p[2] - lp[2];
        if (!k->at_once && dx * dx + dz * dz > WAKE_DIST * WAKE_DIST) continue;
        s->woken = 1;
        s->woken_at = frame;
        uint64_t saved[31], sp = c->sp;
        memcpy(saved, c->x, sizeof saved);
        if (k->start) {
            c->x[0] = s->obj;
            guest_call(c, MAIN_BASE + k->start);
        } else {
            c->x[0] = s->obj + k->sm;
            c->x[1] = k->fight;
            guest_call(c, k->chg);
        }
        memcpy(c->x, saved, sizeof saved);
        c->sp = sp;
        LOG("[miniboss] f%llu %s woken (Link %.1f away; it stood at %.1f %.1f)\n", (unsigned long long)frame, k->name,
            sqrtf(dx * dx + dz * dz), p[0], p[2]);
    }
}

// Moldorm on the Field: his body segments get the head's attack (tools/patches.py MOLDORM_*): touching any part hurts
// and the hit's invincibility lets Link through, as past other overworld enemies. Built as the head's is: its collider
// description (kept by rt_moldorm_shape as the head's is made) on the segment's model and its "root" bone, the
// component (f_7000043220, named AttackColComp1..3), the attack data DeguTail:body_attack (vfunc +0x80), enabled
// (+0xa8). Our references are dropped once he's gone (moldorm_attacks_tick).
#define SP_RELEASE (MAIN_BASE + 0xbae080)
static uint8_t g_mshape[0x100], g_mshape_s[0x20];
static bool g_mshape_ok;
static struct { uint64_t actor, ctrl[3]; } g_mextra[16];

void rt_moldorm_shape(Cpu *c) {
    g_mshape_ok = false;
    if (!rt_gohma_free()) return;
    memcpy(g_mshape, (void *)(uintptr_t)c->x[0], sizeof g_mshape);
    memcpy(g_mshape_s, (void *)(uintptr_t)c->x[1], sizeof g_mshape_s);
    g_mshape_ok = true;
}

void rt_moldorm_attacks(Cpu *c) {
    if (!g_mshape_ok || !rt_gohma_free()) return;
    g_mshape_ok = false;
    static uint64_t mem;  // (guest scratch: block, shape, name / model pairs, out, strings)
    if (!mem) mem = (uint64_t)(uintptr_t)hle_alloc(0x400);
    uint64_t blk = mem, shp = mem + 0x100, nm = mem + 0x140, mdl = mem + 0x160, out = mem + 0x180, root = mem + 0x1a0,
             cname = mem + 0x1b0;
    strcpy((char *)(uintptr_t)root, "root");
    uint64_t actor = c->x[20], saved[31], sp = c->sp, pc = c->pc;
    __uint128_t v0 = c->v[0], v1 = c->v[1];
    memcpy(saved, c->x, sizeof saved);
    int slot = -1;
    for (int i = 0; i < 16 && slot < 0; i++) if (!g_mextra[i].actor) slot = i;
    for (int part = 1; part <= 3; part++) {
        uint64_t model = LD64(actor + 0x14a8 + part * 0x10), mctrl = LD64(actor + 0x14b0 + part * 0x10);
        if (!model) continue;
        memcpy((void *)(uintptr_t)blk, g_mshape, sizeof g_mshape);
        ST64(blk + 0x10, model), ST64(blk + 0x18, mctrl);
        memcpy((void *)(uintptr_t)shp, g_mshape_s, sizeof g_mshape_s);
        ST64(shp + 0x10, shp + 0x18);
        c->x[0] = blk, c->x[1] = shp, c->x[2] = root;
        guest_call(c, MAIN_BASE + 0xcdae0);
        snprintf((char *)(uintptr_t)cname, 16, "AttackColComp%d", part);
        ST64(nm, cname), ST64(nm + 8, strlen((const char *)(uintptr_t)cname));
        ST64(mdl, model), ST64(mdl + 8, mctrl);
        ST64(out, 0), ST64(out + 8, 0);
        c->x[0] = out, c->x[1] = actor, c->x[2] = nm, c->x[3] = mdl, c->x[4] = blk;
        guest_call(c, MAIN_BASE + 0x43220);
        uint64_t comp = LD64(out);
        if (comp) {
            c->x[0] = comp, c->x[1] = MAIN_BASE + 0x16c2147;  // "DeguTail:body_attack"
            c->pc = LD64(LD64(comp) + 0x80), guest_call(c, c->pc);
            c->x[0] = comp, c->x[1] = 1;
            guest_call(c, LD64(LD64(comp) + 0xa8));
        }
        if (slot >= 0) g_mextra[slot].actor = actor, g_mextra[slot].ctrl[part - 1] = LD64(out + 8);
        LOG("[miniboss] Moldorm %llx: body segment %d attack %s\n", (unsigned long long)actor, part, comp ? "on" : "failed");
    }
    memcpy(c->x, saved, sizeof saved);
    c->v[0] = v0, c->v[1] = v1, c->sp = sp, c->pc = pc;
}

static void moldorm_attacks_tick(Cpu *c) {  // (he's gone - deleted or another actor in his place: our references go)
    for (int i = 0; i < 16; i++) {
        if (!g_mextra[i].actor || LD64(g_mextra[i].actor) == MAIN_BASE + 0x1b71f38) continue;
        uint64_t saved[31], sp = c->sp;
        memcpy(saved, c->x, sizeof saved);
        for (int k = 0; k < 3; k++)
            if (g_mextra[i].ctrl[k]) { c->x[0] = g_mextra[i].ctrl[k]; guest_call(c, SP_RELEASE); }
        memcpy(c->x, saved, sizeof saved);
        c->sp = sp;
        memset(&g_mextra[i], 0, sizeof g_mextra[i]);
    }
}
