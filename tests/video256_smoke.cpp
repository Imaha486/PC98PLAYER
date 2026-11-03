// SPDX-License-Identifier: MIT
#include "player.h"
#include "state.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <set>
#include <cstring>
static void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
    Machine* m=nullptr;
    try {
        Ini ini;ini.kv["PC98PLAYER.START"]="TEST.COM";PlayerSettings settings;
        check(player_settings_from_ini(ini,".",&settings) && settings.cfg.video_colors==16,"missing VideoColors preserves the 16-color default");
        ini.kv["PC98PLAYER.VIDEOCOLORS"]="256";
        check(player_settings_from_ini(ini,".",&settings) && settings.cfg.video_colors==256,"INI enables extended graphics explicitly");
        ini.kv["PC98PLAYER.VIDEOCOLORS"]="123";
        check(player_settings_from_ini(ini,".",&settings) && settings.cfg.video_colors==16,"invalid color setting falls back to legacy behavior");
        Config cfg;cfg.video_colors=256;cfg.midi=0;m=machine_create(cfg);
        check(m->ram[0x45c]&0x40,"HLE reports extended graphics without a ROM");
        check(mem_rb(m,0xf8e80)==0x98 && mem_rb(m,0xf8e81)==0x21 && (m->ram[0x597]&4),"PC-9821 ROM and CRT BIOS identity");
        check(!m->pegc.on,"16-color mode remains the reset mode");
        // Later AGS_9821 drivers use INT18/4D rather than writing port 6A.
        m->cpu.r[EAX]=0x4d00;m->cpu.r[ECX]=0x0100;bios_hle(m,HLE_INT18);
        check(m->pegc.on && (m->ram[0x54d]&0x80),"INT18/4D CH=1 enables the same extended framebuffer as port 6A");
        io_out8(m,0xa8,200);io_out8(m,0xaa,0x12);io_out8(m,0xac,0x34);io_out8(m,0xae,0x56);
        mem_wb(m,0xa8000,200);m->gdcs.display=1;m->gdcm.display=0;m->gdcs.pitch=80;m->gdc_clk5=1;
        std::vector<uint32_t> biosPixels(640*400);video_render(m,biosPixels.data());
        check(biosPixels[0]==0xff341256,"BIOS switch routes guest pixel writes and palettes to 256-color renderer");
        m->cpu.r[EAX]=0x4d00;m->cpu.r[ECX]=0x0200;bios_hle(m,HLE_INT18);
        check(m->pegc.on,"unsupported INT18/4D switch leaves the current mode alone");
        m->cpu.r[EAX]=0x4d00;m->cpu.r[ECX]=0;bios_hle(m,HLE_INT18);
        check(!m->pegc.on && m->pegc_vram[0]==200,"BIOS legacy switch preserves extended VRAM");
        mem_wb(m,0xe0123,0x5a);check(m->gvram[0][3][0x123]==0x5a && m->pegc.mmio[0x123]==0,"legacy E-plane remains accessible before extended mode");
        io_out8(m,0x6a,7);io_out8(m,0x6a,0x21);io_out8(m,0x6a,6);
        check(m->pegc.on,"guest enables extended graphics");
        io_out8(m,0xa8,254);io_out8(m,0xaa,0x12);io_out8(m,0xac,0x34);io_out8(m,0xae,0x56);
        check(io_in8(m,0xa8)==254 && io_in8(m,0xaa)==0x12 && io_in8(m,0xac)==0x34 && io_in8(m,0xae)==0x56,"8-bit RGB palette and full index readback");
        mem_ww(m,0xe0004,15);mem_ww(m,0xe0006,3);
        mem_ww(m,0xa8000,0xabcd);mem_ww(m,0xb0000,0x1234);
        check(m->pegc_vram[15*0x8000]==0xcd && m->pegc_vram[3*0x8000]==0x34,"independent 32K VRAM windows");
        check(mem_rw(m,0xa8000)==0xabcd && mem_rw(m,0xb0000)==0x1234,"banked VRAM reads");
        io_out8(m,0xf2,0);
        check(mem_rb(m,0xf78000)==0xff,"linear VRAM stays hidden until E0102h enables it");
        mem_wb(m,0xe0102,1);
        check(mem_rw(m,0xf78000)==0xabcd && mem_rb(m,0xf18000)==0x34,"linear VRAM at F00000h");
        mem_ww(m,0xf00010,0x4321);check(m->pegc_vram[0x10]==0x21 && m->pegc_vram[0x11]==0x43,"linear VRAM writes");
        m->pegc_vram[0x10]=m->pegc_vram[0x11]=0;mem_wb(m,0xe0102,0);
        mem_ww(m,0xe0006,0);mem_ww(m,0xaffff,0x9876);
        check(m->pegc_vram[0x7ffff]==0x76 && m->pegc_vram[0]==0x98,"word access crosses window boundary correctly");
        for(int i=0;i<256;++i) {io_out8(m,0xa8,(uint8_t)i);io_out8(m,0xaa,(uint8_t)i);io_out8(m,0xac,(uint8_t)i);io_out8(m,0xae,(uint8_t)i);}
        for(unsigned i=0;i<256000;++i) m->pegc_vram[i]=(uint8_t)i;
        m->gdcs.display=1;m->gdcm.display=0;m->gdcs.pitch=80;m->gdc_clk5=1;
        std::vector<uint32_t> pixels(640*400);video_render(m,pixels.data());
        check(std::set<uint32_t>(pixels.begin(),pixels.end()).size()==256,"renderer produces all 256 palette entries");
        check(pixels[255]==0xffffffff && pixels[640]==0xff808080,"packed pixel order and pitch");
        m->disp_bank=1;m->pegc_vram[0x40000]=7;video_render(m,pixels.data());check(pixels[0]==0xff070707,"display page selection");m->disp_bank=0;
        // Preserve upstream's GDC scroll/pitch and 31kHz support after the merge.
        memset(m->gdcs.pram,0,sizeof m->gdcs.pram);
        m->gdcs.pram[0]=5;m->gdcs.pram[2]=0x10;m->gdcs.pram[4]=10;
        video_render(m,pixels.data());
        check(pixels[0]==0xff505050 && pixels[640]==0xffa0a0a0,"GDC split scroll regions retain their start addresses");
        memset(m->gdcs.pram,0,sizeof m->gdcs.pram);
        m->gdc_clk5=0;m->gdcs.pitch=40;video_render(m,pixels.data());
        check(pixels[640]==0xff808080,"2.5MHz word pitch preserves the same scanline stride");
        m->gdc_clk5=1;m->gdcs.pitch=80;io_out8(m,0x6a,0x69);io_out8(m,0x9a8,1);
        std::vector<uint32_t> tall(640*480);m->pegc_vram[479*640]=7;
        check(video_render(m,tall.data())==480 && tall[479*640]==0xff070707 && io_in8(m,0x9a8)==1,"31kHz mode renders all 480 lines");
        io_out8(m,0x9a8,0);io_out8(m,0x6a,0x68);
        memset(m->pegc_vram.data(),0,m->pegc_vram.size());mem_ww(m,0xe0100,1);mem_ww(m,0xe0104,0);mem_ww(m,0xe010c,0xffff);mem_ww(m,0xe0108,0x11f0);
        mem_ww(m,0xa8000,0x00aa);
        check(m->pegc_vram[0]==255 && m->pegc_vram[1]==0 && m->pegc_vram[8]==0,"CPU source ROP and planar bit ordering");
        mem_ww(m,0xe0114,255);mem_ww(m,0xe010a,1);mem_ww(m,0xe0108,0x30f0);
        check(mem_rw(m,0xa8000)==0x00aa,"planar color comparison and read latch");
        mem_ww(m,0xe0108,0x10f0);mem_ww(m,0xa8002,0);
        check(m->pegc_vram[16]==255 && m->pegc_vram[17]==0,"latched VRAM source copy");
        mem_ww(m,0xe0104,0x00fe);mem_ww(m,0xe0108,0x11f0);mem_ww(m,0xe010c,0x0080);mem_ww(m,0xa8004,0xffff);
        check(m->pegc_vram[32]==1 && m->pegc_vram[33]==0,"plane write mask and bit mask");
        StateW snapshot;machine_state_save(m,snapshot);m->pegc.pal[255][0]=1;m->pegc_vram[32]=99;m->pegc.on=0;
        StateR restore(snapshot.b.data(),snapshot.b.size());machine_state_load(m,restore);
        check(restore.ok && m->pegc.on && m->pegc.pal[255][0]==255 && m->pegc_vram[32]==1,"PEGC state roundtrip");
        // Upstream snapshots have AVSF + PEGC but no additional planar chunk.
        StateR upstreamState(snapshot.b.data(),snapshot.b.size()-sizeof(PegcPlanar)-4);
        m->pegc_planar.selector=0x10;m->pegc_planar.source[0]=123;
        machine_state_load(m,upstreamState);
        check(upstreamState.ok && m->pegc.on && m->pegc_vram[32]==1 && m->pegc_planar.selector==0 && m->pegc_planar.source[0]==0,"upstream PEGC snapshots retain their layout and reset new latches");
        // Reconstruct the PR's earlier unversioned PEGC layout to test migration.
        StateW oldPr;
        size_t pegcSize=4+sizeof(Pegc)+m->pegc_vram.size()+4+sizeof(PegcPlanar);
        oldPr.bytes(snapshot.b.data(),snapshot.b.size()-pegcSize-sizeof(AvsFm)-4);
        oldPr.tag("PEGC");oldPr.bytes(m->pegc_vram.data(),m->pegc_vram.size());
        oldPr.pod(m->pegc.pal);oldPr.pod(m->pegc.mmio);
        oldPr.pod(m->pegc_planar.source);oldPr.pod(m->pegc_planar.pattern);
        oldPr.pod(m->pegc.on);oldPr.pod(m->pegc.single);oldPr.pod(m->pegc_planar.selector);
        m->pegc_vram[32]=123;StateR oldPrState(oldPr.b.data(),oldPr.b.size());machine_state_load(m,oldPrState);
        check(oldPrState.ok && m->pegc.on && m->pegc_vram[32]==1,"earlier PR snapshots migrate to upstream's framebuffer");
        StateR legacy(snapshot.b.data(),snapshot.b.size()-sizeof(Pegc)-0x80000-4-sizeof(PegcPlanar)-4);machine_state_load(m,legacy);check(legacy.ok && !m->pegc.on,"legacy snapshots remain readable");
        machine_destroy(m);m=nullptr;
        cfg.video_colors=16;m=machine_create(cfg);io_out8(m,0x6a,0x21);check(!m->pegc.on && !(m->ram[0x45c]&0x40),"16-color hardware capability preserved");
        m->cpu.r[EAX]=0x4d00;m->cpu.r[ECX]=0x0100;bios_hle(m,HLE_INT18);
        check(!m->pegc.on,"BIOS switch cannot expose 256 colors on a 16-color configuration");
        machine_destroy(m);puts("PASS: BIOS-free PEGC, palette, bank windows, rendering, planar ROP/masks, state, 16-color compatibility");return 0;
    } catch(const std::exception& error) { fprintf(stderr,"FAIL: %s\n",error.what());if(m) machine_destroy(m);return 1; }
}
