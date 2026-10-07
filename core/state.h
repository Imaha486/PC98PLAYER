// SPDX-License-Identifier: MIT
//  state.h -- ステートセーブ／ロードの読み書き道具
//
//  Machine は std::vector や std::string を含むので memcpy 一発にはできない。
//  各層（CPU/デバイス・BIOS・DOS・音）が自分の分を順番に書き、同じ順番で読む。
#pragma once
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

struct StateW {
    std::vector<uint8_t> b;
    void bytes(const void* p, size_t n) { const uint8_t* s = (const uint8_t*)p; b.insert(b.end(), s, s + n); }
    template<class T> void pod(const T& v) { bytes(&v, sizeof(T)); }
    void u32(uint32_t v) { pod(v); }
    void str(const std::string& s) { u32((uint32_t)s.size()); bytes(s.data(), s.size()); }
    void tag(const char* t) { bytes(t, 4); }
};

struct StateR {
    const uint8_t* p = nullptr;
    size_t n = 0, pos = 0;
    bool ok = true;
    StateR(const uint8_t* d, size_t len) : p(d), n(len) {}
    void bytes(void* dst, size_t len) {
        if (!ok || pos + len > n) { ok = false; memset(dst, 0, len); return; }
        memcpy(dst, p + pos, len); pos += len;
    }
    template<class T> void pod(T& v) { bytes(&v, sizeof(T)); }
    uint32_t u32() { uint32_t v = 0; pod(v); return v; }
    std::string str() {
        uint32_t len = u32();
        if (!ok || len > 0x1000000 || pos + len > n) { ok = false; return std::string(); }
        std::string s((const char*)p + pos, len); pos += len; return s;
    }
    bool tag(const char* t) { char x[4]; bytes(x, 4); if (memcmp(x, t, 4)) ok = false; return ok; }
    bool peek_tag(const char* t) const { return ok && pos + 4 <= n && !memcmp(p + pos, t, 4); }
};

struct Machine;
void bios_state_save(StateW& w);
void xmsems_state_save(Machine* m, StateW& w);
void pcm86_state_save(Machine* m, StateW& w);
void pcm86_state_load(Machine* m, StateR& r);
void xmsems_state_load(Machine* m, StateR& r);
void bios_state_load(StateR& r);
void dos_state_save(Machine* m, StateW& w);
void dos_state_load(Machine* m, StateR& r);
void machine_state_save(Machine* m, StateW& w);
void machine_state_load(Machine* m, StateR& r);
void fontrom_state_save(StateW& w);
void fontrom_state_load(StateR& r);
void egc_state_save(StateW& w);
void egc_state_load(StateR& r);
