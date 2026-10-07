// 検証用ハーネス（Linux）: ウインドウを出さずに走らせ、画面と音をファイルへ出す
#include "../core/player.h"
#include "../core/hostfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <set>
#include <time.h>

static std::vector<uint8_t> g_font;
static void fk(void*, uint16_t jis, uint8_t out[32]) {
    int hi = jis >> 8, lo = jis & 0xFF;
    size_t idx = (size_t)((hi - 0x21) * 94 + (lo - 0x21)) * 32;
    if (idx + 32 <= 94 * 94 * 32) memcpy(out, &g_font[idx], 32); else memset(out, 0, 32);
}
static void fa(void*, uint8_t c, uint8_t out[16]) { memcpy(out, &g_font[94 * 94 * 32 + c * 16], 16); }

struct KeyEv { int frame; int sc; int down; };
struct MouseEv { int frame, dx, dy, b; };
static std::vector<MouseEv> mev;
static std::vector<std::pair<int,int>> jev;   // --joy f:bits（16 進）
static int save_f = -1, save_slot = 0, load_f = -1, load_slot = 0;

#include "../core/floppy.h"
#include "../core/hdimage.h"
int main(int argc, char** argv) {
    std::vector<std::pair<int, std::string>> fdev;
    std::vector<std::pair<int, std::pair<std::string, std::string>>> cpev;   // --cpfile フレーム:元:先（ファイルを書き換える。実機のディスク交換の代わり）   // --fd フレーム:イメージ（フロッピーの入れ替え）
    if (argc >= 4 && std::string(argv[1]) == "--hdx") {   // ハードディスクイメージの展開: --hdx イメージ 展開先
        hdimage::Info in; std::string e;
        if (!hdimage::probe(argv[2], &in, &e)) { fprintf(stderr, "probe NG: %s\n", e.c_str()); return 1; }
        fprintf(stderr, "%s ss=%d spt=%d heads=%d cyls=%d parts=%zu\n", in.format.c_str(), in.ssize, in.spt, in.heads, in.cyls, in.parts.size());
        hdimage::Result r;
        if (!hdimage::extract(argv[2], argv[3], &r, &e)) { fprintf(stderr, "extract NG: %s\n", e.c_str()); return 1; }
        fprintf(stderr, "files=%d dirs=%d skipped=%d errors=%d bytes=%llu\n", r.files, r.dirs, r.skipped, r.errors, (unsigned long long)r.bytes);
        for (auto& n : r.notes) fprintf(stderr, "  %s\n", n.c_str());
        return 0;
    }
    if (argc < 2) { fprintf(stderr, "usage: harness gamedir [--start cmd] [--frames n] [--shot a,b] [--key f:sc[:u]] [--wav f] [--out prefix]\n"); return 1; }
    std::string dir = argv[1];
    std::string start, out = "shot", wav, ini_path;
    int frames = 600, mhz = 0;
    std::set<int> shots;
    std::vector<KeyEv> keys;
    int trace = 0;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return std::string(i + 1 < argc ? argv[++i] : ""); };
        if (a == "--start") start = next();
        else if (a == "--frames") frames = atoi(next().c_str());
        else if (a == "--shot") { std::string s = next(); char* p = &s[0]; while (*p) { shots.insert((int)strtol(p, &p, 10)); if (*p == ',') p++; } }
        else if (a == "--key") { std::string s = next(); KeyEv k; k.down = 2; sscanf(s.c_str(), "%d:%x", &k.frame, &k.sc); if (s.find(":u") != std::string::npos) k.down = 0; else if (s.find(":d") != std::string::npos) k.down = 1; keys.push_back(k); }
        else if (a == "--mouse") { std::string s2 = next(); MouseEv e; sscanf(s2.c_str(), "%d:%d:%d:%d", &e.frame, &e.dx, &e.dy, &e.b); mev.push_back(e); }
        else if (a == "--joy") { std::string s2 = next(); int f = 0; unsigned b = 0; sscanf(s2.c_str(), "%d:%x", &f, &b); jev.push_back({f, (int)b}); }
        else if (a == "--save") { std::string s2 = next(); sscanf(s2.c_str(), "%d:%d", &save_f, &save_slot); }
        else if (a == "--load") { std::string s2 = next(); sscanf(s2.c_str(), "%d:%d", &load_f, &load_slot); }
        else if (a == "--wav") wav = next();
        else if (a == "--fd") { std::string s2 = next(); size_t c = s2.find(':'); fdev.push_back({atoi(s2.substr(0, c).c_str()), s2.substr(c + 1)}); }
        else if (a == "--cpfile") { std::string s2 = next(); size_t c1 = s2.find(':'), c2 = s2.find(':', c1 + 1); cpev.push_back({atoi(s2.substr(0, c1).c_str()), {s2.substr(c1 + 1, c2 - c1 - 1), s2.substr(c2 + 1)}}); }
        else if (a == "--out") out = next();
        else if (a == "--mhz") mhz = atoi(next().c_str());
        else if (a == "--trace") trace = 1; else if (a == "--trace2") trace = 2;
        else if (a == "--ini") ini_path = next();
    }
    extern uint32_t g_watch_lo, g_watch_hi;
    extern int g_itr_stop_cs; if (getenv("ITRACE_STOP")) g_itr_stop_cs = (int)strtol(getenv("ITRACE_STOP"), 0, 16);
    extern int g_trace_int; if (getenv("TRACEINT")) g_trace_int = (int)strtol(getenv("TRACEINT"), 0, 16);
    extern int g_trace_port_lo, g_trace_port_hi; if (getenv("TRACEPORT")) sscanf(getenv("TRACEPORT"), "%x-%x", &g_trace_port_lo, &g_trace_port_hi);
    if (getenv("WATCH")) { sscanf(getenv("WATCH"), "%x-%x", &g_watch_lo, &g_watch_hi); }
    FILE* ff = fopen("/home/claude/g/pc98player/tools/font.bin", "rb");
    g_font.resize(286848); fread(g_font.data(), 1, g_font.size(), ff); fclose(ff);
    FontSource fs; fs.kanji = fk; fs.ank = fa; fs.narrow = nullptr; fs.user = nullptr;
    video_set_font(fs);

    if (getenv("FONTSHEET")) {
        // ANK 256 字（上）と 8〜13 区（下）を並べた一覧
        const int W = 16 * 20, H = 16 * 20 + 6 * 6 * 20;
        std::vector<uint8_t> img(W * 2 * H * 3, 30);
        auto px = [&](int x, int y, int v) { if (x < 0 || y < 0 || x >= W * 2 || y >= H) return; uint8_t* q = &img[(y * W * 2 + x) * 3]; q[0] = q[1] = q[2] = (uint8_t)v; };
        for (int c = 0; c < 256; c++) {
            const uint8_t* g = font_get_ank((uint8_t)c);
            int ox = (c % 16) * 20 * 2, oy = (c / 16) * 20;
            for (int y = 0; y < 16; y++) for (int x = 0; x < 8; x++) { int v = (g[y] & (0x80 >> x)) ? 255 : 0; px(ox + x * 2, oy + y, v); px(ox + x * 2 + 1, oy + y, v); }
        }
        int rows[6] = {0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D};
        for (int ri = 0; ri < 6; ri++) for (int cell = 0x21; cell < 0x7F; cell++) {
            const uint8_t* g = font_get_kanji((uint16_t)((rows[ri] << 8) | cell));
            int i = cell - 0x21;
            int ox = (i % 32) * 20, oy = 16 * 20 + ri * 6 * 20 / 1 + (i / 32) * 20;
            for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { uint16_t v = (uint16_t)((g[y * 2] << 8) | g[y * 2 + 1]); px(ox + x, oy + y, (v & (0x8000 >> x)) ? 255 : 0); }
        }
        FILE* o = fopen(getenv("FONTSHEET"), "wb"); fprintf(o, "P6\n%d %d\n255\n", W * 2, H); fwrite(img.data(), 1, img.size(), o); fclose(o);
        return 0;
    }
    Ini ini;
    PlayerSettings ps;
    if (ini_path.empty()) ini_path = hostfs::join(dir, "PC98PLAYER.INI");
    ini.load(ini_path);
    player_settings_from_ini(ini, dir, &ps);
    if (!start.empty()) ps.cfg.start = start;
    if (mhz) ps.cfg.cpu_mhz = mhz;
    if (trace) ps.cfg.trace = trace;
    Player* p = new Player();
    std::string err;
    if (getenv("MIDI")) ps.cfg.midi = 1;
    if (getenv("NOEMS")) ps.cfg.ems = false;
    if (getenv("NOXMS")) ps.cfg.xms_kb = 0;
    p->init(ps, &err);
    if (getenv("MIDI")) p->midi_sink = [](void*, const uint8_t* msg, int len) { fprintf(stderr, "[midi]"); for (int i = 0; i < len && i < 16; i++) fprintf(stderr, " %02X", msg[i]); fprintf(stderr, "\n"); };
    std::vector<int16_t> pcm;
    clock_t c0 = clock();
    for (int f = 0; f < frames; f++) {
        for (auto& k : keys) {
            if (k.down == 2) { if (k.frame == f) machine_key(p->m, (uint8_t)k.sc, true); if (k.frame + 6 == f) machine_key(p->m, (uint8_t)k.sc, false); }
            else if (k.frame == f) machine_key(p->m, (uint8_t)k.sc, k.down != 0);
        }
        extern unsigned g_opna_hist[2][256];
        if (f == (getenv("HISTF") ? atoi(getenv("HISTF")) : 2100)) memset(g_opna_hist, 0, sizeof(g_opna_hist));
        { extern int g_prof_cs; extern unsigned* g_prof; if (f == (getenv("PROFF") ? atoi(getenv("PROFF")) : 2100) && getenv("PROFCS")) { g_prof = (unsigned*)calloc(65536, 4); g_prof_cs = (int)strtol(getenv("PROFCS"), 0, 16); } }
        if (f == save_f) { std::string e2; fprintf(stderr, "save slot%d: %s %s\n", save_slot, p->save_state(save_slot, &e2) ? "ok" : "NG", e2.c_str()); }
        if (f == load_f) { std::string e2; bool lok = p->load_state(load_slot, &e2); fprintf(stderr, "load slot%d: %s %s\n", load_slot, lok ? "ok" : "NG", e2.c_str());
            fprintf(stderr, "keyon:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %02X", p->m->opna.keyon[i]);
            fprintf(stderr, " A0-A6:"); for (int i = 0xA0; i < 0xA7; i++) fprintf(stderr, " %02X", p->m->opna.reg[0][i]);
            fprintf(stderr, " 40-4E:"); for (int i = 0x40; i < 0x4F; i++) fprintf(stderr, " %02X", p->m->opna.reg[0][i]);
            fprintf(stderr, " B0-B6:"); for (int i = 0xB0; i < 0xB7; i++) fprintf(stderr, " %02X", p->m->opna.reg[0][i]); fprintf(stderr, "\n"); }
        if (getenv("REBOOTF") && f == atoi(getenv("REBOOTF"))) { std::string er; fprintf(stderr, "reboot: %s %s\n", p->reboot(&er) ? "ok" : "NG", er.c_str()); }
        for (auto& e : mev) if (e.frame == f) machine_mouse(p->m, e.dx, e.dy, e.b);
        for (auto& e : jev) if (e.first == f) machine_joystick(p->m, (uint8_t)e.second);
        for (auto& e : cpev) if (e.first == f) {
            FILE* i = fopen(e.second.first.c_str(), "rb"); FILE* o = fopen(e.second.second.c_str(), "r+b");
            if (i && o) { std::vector<char> b(1 << 20); size_t n; while ((n = fread(b.data(), 1, b.size(), i)) > 0) fwrite(b.data(), 1, n, o); }
            if (i) fclose(i); if (o) fclose(o);
            fprintf(stderr, "cpfile %s -> %s\n", e.second.first.c_str(), e.second.second.c_str());
        }
        for (auto& e : fdev) if (e.first == f) {
            std::string er, path = e.second; int unit = 0;
            if (path.size() > 2 && path[0] == '#' && path[2] == '#') { unit = path[1] - '0'; path = path.substr(3); }   // --fd f:#1#path = 2 台目
            if (path == "-") { floppy::eject_unit(unit); fprintf(stderr, "fd eject u%d\n", unit); }   // --fd f:#0#- = 取り出す
            else fprintf(stderr, "fd insert u%d %s: %s\n", unit, path.c_str(), floppy::insert_unit(unit, path, &er) ? "ok" : er.c_str());
            fprintf(stderr, "  now u0=%s u1=%s\n", floppy::current_path_unit(0).c_str(), floppy::current_path_unit(1).c_str());
        }
        { extern int g_trace_port_lo, g_trace_port_hi; static int saved_lo = -1, saved_hi = -1;
          if (getenv("TRACEPORT_FROM")) { int from = atoi(getenv("TRACEPORT_FROM"));
            if (f == 0) { saved_lo = g_trace_port_lo; saved_hi = g_trace_port_hi; g_trace_port_lo = 1; g_trace_port_hi = 0; }
            if (f == from) { g_trace_port_lo = saved_lo; g_trace_port_hi = saved_hi; } } }
        bool shot = shots.count(f) > 0 || f == frames - 1; bool rend = shot || getenv("RENDERALL");
        p->run_frame(rend, !getenv("NOAUDIO"));
        if (!wav.empty()) pcm.insert(pcm.end(), p->audio.begin(), p->audio.end());
        if (shot) {
            char fn[256]; snprintf(fn, sizeof(fn), "%s_%05d.ppm", out.c_str(), f);
            FILE* o = fopen(fn, "wb");
            fprintf(o, "P6\n640 400\n255\n");
            for (int i = 0; i < 640 * 400; i++) { uint32_t c = p->fb[i]; uint8_t rgb[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c}; fwrite(rgb, 1, 3, o); }
            fclose(o);
        }
        if (p->m->quit == 2) { fprintf(stderr, "停止: %s (frame %d)\n", p->m->status.c_str(), f); break; }
    }
    if (getenv("DUMPGDC")) {
        for (int k = 0; k < 2; k++) { Gdc& g = k ? p->m->gdcs : p->m->gdcm; fprintf(stderr, "%s sync:", k ? "gdcs" : "gdcm"); for (int i = 0; i < 8; i++) fprintf(stderr, " %02X", g.sync[i]); fprintf(stderr, " pram:"); for (int i = 0; i < 16; i++) fprintf(stderr, " %02X", g.pram[i]); fprintf(stderr, " zoom=%02X pitch=%02X csr=%02X %02X %02X disp=%d\n", g.zoom, g.pitch, g.csrform[0], g.csrform[1], g.csrform[2], g.display); }
    }
    if (getenv("DUMPATTR")) {
        for (int r = 0; r < 25; r++) { fprintf(stderr, "attr %2d:", r); for (int c = 0; c < 80; c++) fprintf(stderr, "%02X", p->m->tvram[0x2000 + (r * 80 + c) * 2]); fprintf(stderr, "\n"); }
    }
    if (getenv("GAMEBLOCKS")) { std::vector<DosMemBlock> b; dos_game_blocks(p->m, b); uint32_t t = 0;
        for (auto& x : b) { fprintf(stderr, "block %05X-%05X %6u %s\n", x.start, x.start + x.len - 1, x.len, x.name.c_str()); t += x.len; }
        fprintf(stderr, "game total %u bytes\n", t); }
    if (getenv("DUMPTEXT")) {
        for (int r = 0; r < 25; r++) {
            bool any = false;
            for (int c = 0; c < 80; c++) { uint32_t i = (r * 80 + c) * 2; if ((p->m->tvram[i] != 0x20 && p->m->tvram[i] != 0) || p->m->tvram[i+1]) any = true; }
            if (!any) continue;
            fprintf(stderr, "row %2d:", r);
            for (int c = 0; c < 80; c++) { uint32_t i = (r * 80 + c) * 2; if ((p->m->tvram[i] == 0x20 || p->m->tvram[i] == 0) && !p->m->tvram[i+1]) continue; fprintf(stderr, " %d:%02X%02X/%02X", c, p->m->tvram[i+1], p->m->tvram[i], p->m->tvram[0x2000+i]); }
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "modeff:"); for (int i = 0; i < 8; i++) fprintf(stderr, " %d", p->m->modeff[i]); fprintf(stderr, "\n");
    }
    if (getenv("DUMPLINE")) {
        int y = atoi(getenv("DUMPLINE"));
        for (int pl = 0; pl < 4; pl++) { fprintf(stderr, "plane%d:", pl); for (int b = 20; b < 60; b++) fprintf(stderr, " %02X", p->m->gvram[p->m->disp_bank][pl][y * 80 + b]); fprintf(stderr, "\n"); }
        fprintf(stderr, "analog=%d pal:", p->m->analog); for (int i = 0; i < 16; i++) fprintf(stderr, " %X%X%X", p->m->pal[i][1], p->m->pal[i][0], p->m->pal[i][2]); fprintf(stderr, "\n");
    }
    fprintf(stderr, "irr=%02X/%02X imr=%02X/%02X isr=%02X/%02X opna: ta=%d tb=%d ena=%d enb=%d reg27=%02X  status=%02X a460=%02X\n", p->m->pic[0].irr, p->m->pic[1].irr, p->m->pic[0].imr, p->m->pic[1].imr, p->m->pic[0].isr, p->m->pic[1].isr,
        p->m->opna.timer_a_run, p->m->opna.timer_b_run, p->m->opna.irq_enable_a, p->m->opna.irq_enable_b, p->m->opna.reg[0][0x27], p->m->opna.status, p->m->opna.reg[1][0xFE]);
    for (int v = 0; v < 256; v++) { uint16_t sg = p->m->ram[v*4+2]|(p->m->ram[v*4+3]<<8); if (sg != 0xF000) fprintf(stderr, "[vec %02X=%04X:%04X] ", v, sg, p->m->ram[v*4]|(p->m->ram[v*4+1]<<8)); }
    fprintf(stderr, "\n");
    for (int v = 0x08; v < 0x18; v++) fprintf(stderr, "%02X=%04X:%04X ", v, p->m->ram[v*4+2]|(p->m->ram[v*4+3]<<8), p->m->ram[v*4]|(p->m->ram[v*4+1]<<8));
    fprintf(stderr, "\n");
    extern unsigned g_opna_writes, g_opna_keyon; fprintf(stderr, "opna writes=%u keyon=%u\n", g_opna_writes, g_opna_keyon);
    extern unsigned g_irq_count[16]; fprintf(stderr, "irq:"); for (int i = 0; i < 16; i++) fprintf(stderr, " %u", g_irq_count[i]); fprintf(stderr, "\n");
    { extern unsigned g_opna_hist[2][256]; fprintf(stderr, "hist:"); for (int pt = 0; pt < 2; pt++) for (int a = 0; a < 256; a++) if (g_opna_hist[pt][a]) fprintf(stderr, " %d:%02X=%u", pt, a, g_opna_hist[pt][a]); fprintf(stderr, "\n"); }
    if (getenv("DUMPMEM")) { unsigned sg, off, n; sscanf(getenv("DUMPMEM"), "%x:%x:%x", &sg, &off, &n); for (unsigned i = 0; i < n; i++) { if (i % 32 == 0) fprintf(stderr, "\n%04X:%04X ", sg, off + i); fprintf(stderr, "%02X ", p->m->ram[((sg << 4) + off + i) & 0xFFFFF]); } fprintf(stderr, "\n"); }
    { extern unsigned g_spin_hits; fprintf(stderr, "spin hits=%u\n", g_spin_hits); }
    if (getenv("DUMPTEXT")) {
        int r0 = atoi(getenv("DUMPTEXT"));
        uint32_t tsad = (uint32_t)(p->m->gdcm.pram[0] | (p->m->gdcm.pram[1] << 8));
        for (int r = r0; r < r0 + 3 && r < 25; r++) {
            fprintf(stderr, "row %d:", r);
            for (int c = 0; c < 80; c++) { uint32_t cell = (tsad + r * 80 + c) & 0xFFF; fprintf(stderr, " %02X%02X/%02X", p->m->tvram[cell * 2], p->m->tvram[cell * 2 + 1], p->m->tvram[0x2000 + cell * 2]); }
            fprintf(stderr, "\n");
        }
    }
    if (getenv("DUMPRAM")) { FILE* df = fopen(getenv("DUMPRAM"), "wb"); if (df) { fwrite(p->m->ram, 1, 0x100000, df); fclose(df); } }
    { extern unsigned* g_prof; if (g_prof) { for (int i = 0; i < 65536; i++) if (g_prof[i]) fprintf(stderr, "%04X:%u ", i, g_prof[i]); fprintf(stderr, "\n"); } }
    if (getenv("FDLIST") && floppy::image()) {   // フロッピーの各トラックの ID と状態
        FloppyImage* im = floppy::image();
        int lim = atoi(getenv("FDLIST")); if (lim <= 0) lim = im->cylinders * 2;
        fprintf(stderr, "[fdlist] %s media=%d cyl=%d lsec=%dx%dx%d\n", im->format.c_str(), im->media, im->cylinders, im->lsec_size, im->lsec_spt, im->lsec_heads);
        for (int t = 0; t < lim && t < FloppyImage::MAX_TRACKS; t++) {
            FdTrack* tr = im->track(t / 2, t & 1);
            if (!tr) continue;
            fprintf(stderr, "T%03d:", t);
            for (auto& x : tr->secs) {
                unsigned sum = 0; for (auto b : x.data) sum += b;
                fprintf(stderr, " %02X%02X%02X%02X", x.c, x.h, x.r, x.n);
                if (x.status) fprintf(stderr, "/%02X", x.status);
                if (x.deleted) fprintf(stderr, "d");
                if (x.copies > 1) fprintf(stderr, "x%d", x.copies);
                fprintf(stderr, "(%zu,%04X)", x.data.size(), sum & 0xFFFF);
            }
            fprintf(stderr, "\n");
        }
    }
    if (getenv("FDSAVE") && floppy::image()) { std::string e; bool ok = floppy::image()->save_d88(getenv("FDSAVE"), &e); fprintf(stderr, "[fdsave] %s %s\n", ok ? "ok" : "NG", e.c_str()); }
    if (getenv("DUMPBIN")) { unsigned sg; char fn[256]; sscanf(getenv("DUMPBIN"), "%x:%255s", &sg, fn); FILE* o = fopen(fn, "wb"); fwrite(&p->m->ram[sg << 4], 1, 65536, o); fclose(o); }
    double secs = (double)(clock() - c0) / CLOCKS_PER_SEC;
    fprintf(stderr, "cpu at %04X:%04X halted=%d AX=%04X SS:SP=%04X:%04X stack:", p->m->cpu.sr[CS_], p->m->cpu.ip, p->m->cpu.halted, (unsigned)(p->m->cpu.r[0] & 0xFFFF), p->m->cpu.sr[SS_], (unsigned)(p->m->cpu.r[4] & 0xFFFF));
    for (int i = 0; i < 12; i++) fprintf(stderr, " %04X", p->m->ram[((p->m->cpu.sr[SS_] << 4) + ((p->m->cpu.r[4] + i * 2) & 0xFFFF)) & 0xFFFFF] | (p->m->ram[((p->m->cpu.sr[SS_] << 4) + ((p->m->cpu.r[4] + i * 2 + 1) & 0xFFFF)) & 0xFFFFF] << 8));
    fprintf(stderr, "\n");
    fprintf(stderr, "frames=%d cpu=%.2fs (%.1f fps) quit=%d cycles=%lld\n", frames, secs, frames / secs, p->m->quit, (long long)p->m->cpu.cycles);
    if (!wav.empty()) {
        FILE* o = fopen(wav.c_str(), "wb");
        uint32_t n = (uint32_t)pcm.size() * 2, rate = (uint32_t)ps.sample_rate;
        uint8_t h[44] = {'R','I','F','F'};
        auto w32 = [&](int o2, uint32_t v) { h[o2] = v; h[o2+1] = v >> 8; h[o2+2] = v >> 16; h[o2+3] = v >> 24; };
        w32(4, 36 + n); memcpy(h + 8, "WAVEfmt ", 8); w32(16, 16); h[20] = 1; h[22] = 2; w32(24, rate); w32(28, rate * 4); h[32] = 4; h[34] = 16;
        memcpy(h + 36, "data", 4); w32(40, n);
        fwrite(h, 1, 44, o); fwrite(pcm.data(), 2, pcm.size(), o); fclose(o);
    }
    return 0;
}
