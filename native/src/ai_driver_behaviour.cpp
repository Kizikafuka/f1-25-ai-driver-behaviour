// SPDX-License-Identifier: MIT
// AI Driver Behaviour, in-game part: the AI's reaction time. The other options of this addon are database edits
// (db/racecraft.json), applied by Swofty while the game boots.
//
// From 0.2.0 everything about how the AI DRIVES is in this addon and everything about what happens to a CAR (contact,
// walls, aero, damage) is in Contact Physics. The two reaction options moved here from Contact Physics 0.7.3:
//  - AI reaction to slides: the game's AI catches a slide in the same frame it starts, so even with exit mistakes it
//    practically never spins. A real driver needs about 0.15 - 0.4 s to react to oversteer. When an AI car's slip angle
//    passes 7 degrees above 15 m/s, its steering and throttle stay where they were for that long (+-30 % random), so the
//    slide can grow before the AI corrects it: most are still caught, some end in a spin. Set by the "AI spins" preset
//    (or "Custom spins: reaction to slides").
//  - AI reaction after a hit: after a car-to-car hit the AI's steering is held at its value from the moment of the hit, so
//    the hit can turn the car. Contact Physics finds the hits and calls kizi_ai_hit() below; without it this does nothing.
//
// Every car has a ControllerSet (+0x28 its AI controller). Each frame the AI controller's update (vtable slot 1) copies
// the AI's decision for car [ctrl+0x18] into the car's input record [ctrl+8]: +0x0c throttle, +0x14 steering, +0x18 brake.
// All AI controllers get a copy of their vtable whose slot 1 calls the game's update and then, while that car's driver
// "has not reacted yet", puts the held steering (and throttle) back.
//
// The two "Pace" grip-layer options were removed in 0.1.7 (the AI could not judge corner entries on tight tracks); the
// grip-layer code lives on in common/kizi_grip.hpp for a later, track-aware version.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string_view>
#include <thread>

#include <swofty/mixin.hpp>
#include "kizi_mem.hpp"

SWOFTY_SETTING(std::string_view, ai_spins, "more");          // db option: the preset (read once, like its database edits)
SWOFTY_SETTING(std::string_view, slide_reaction, "normal");  // used with ai_spins = custom
SWOFTY_SETTING(std::string_view, hit_reaction, "scaled");

namespace {

uintptr_t g_vehicles;                              // vehicle table (index = vehicle id)
uintptr_t g_sets_global;                           // ControllerSet manager pointer
uint8_t g_sets_vec;                                // manager + this = vector of ControllerSet*
using CtrlUpdate = uint64_t (*)(void *);
CtrlUpdate g_ctrl_update;
uint64_t *g_ctrl_vt;                               // our vtable copy (slot 0 at index 1; index 0 = RTTI locator)
uintptr_t g_ctrl_vt_orig;
std::string_view g_preset = "more";                // ai_spins at load

struct Hold {
    std::atomic<DWORD> until{0};
    std::atomic<float> steer{0};
    std::atomic<float> throttle{-1};               // >= 0: throttle held too (a slide the driver has not reacted to yet)
};
Hold g_hold[32];
std::atomic<int> g_hits{0}, g_slides{0};
std::atomic<uint32_t> g_hit_ms_sum{0}, g_hit_ms_max{0}, g_slide_ms_sum{0};
std::atomic<float> g_impact_max{0};

bool holding(int idx) { return g_hold[idx].until.load() && int(g_hold[idx].until.load() - GetTickCount()) > 0; }

uint64_t ctrl_update_hook(void *self)
{
    uint64_t r = g_ctrl_update(self);
    auto c = static_cast<uint8_t *>(self);
    uint8_t idx = c[0x18];
    if (idx < 32 && holding(idx))
        if (auto out = *reinterpret_cast<uint8_t **>(c + 8)) {
            *reinterpret_cast<float *>(out + 0x14) = g_hold[idx].steer;
            float th = g_hold[idx].throttle;
            if (th >= 0)
                *reinterpret_cast<float *>(out + 0x0c) = th;
        }
    return r;
}

// The AI controller of car idx, or 0.
uintptr_t ai_ctrl(int idx)
{
    uint64_t mgr = 0, b = 0, e = 0, set = 0, ai = 0;
    if (!g_sets_global || idx < 0 || !kizi::read(g_sets_global, mgr) || !mgr || !kizi::read(mgr + g_sets_vec, b) ||
        !kizi::read(mgr + g_sets_vec + 8, e) || b + 8 * idx >= e || !kizi::read(b + 8 * idx, set) || !set ||
        !kizi::read(set + 0x28, ai))
        return 0;
    return ai;
}

// Put our vtable on every AI controller that still has the game's (new sessions make new controllers).
void wrap_controllers()
{
    for (int i = 0; i < 32; i++) {
        uintptr_t ai = ai_ctrl(i);
        uint64_t vt = 0;
        if (!ai || !kizi::read(ai, vt) || !vt)
            continue;
        if (!g_ctrl_vt) {                          // first controller: make the copy from its vtable
            uint64_t slot1 = 0;
            if (!kizi::in_image(vt) || !kizi::read(vt + 8, slot1) || !kizi::in_image(slot1))
                return;
            auto copy = new uint64_t[17];
            if (!kizi::read(vt - 8, copy, 17 * 8))
                return;
            g_ctrl_update = reinterpret_cast<CtrlUpdate>(slot1);
            copy[2] = reinterpret_cast<uint64_t>(&ctrl_update_hook);
            g_ctrl_vt_orig = vt, g_ctrl_vt = copy;
        }
        if (vt == g_ctrl_vt_orig) {
            uint64_t nv = reinterpret_cast<uint64_t>(g_ctrl_vt + 1);
            kizi::write(ai, &nv, 8);
        }
    }
}

// Slide reaction from the preset: stock = off, some = quick, more = normal, lots = slow, custom = its own option.
DWORD slide_ms()
{
    std::string_view v = g_preset == "custom" ? slide_reaction.get()
                         : g_preset == "some" ? "quick"
                         : g_preset == "more" ? "normal"
                         : g_preset == "lots" ? "slow"
                                              : "off";
    return v == "quick" ? 150 : v == "normal" ? 250 : v == "slow" ? 400 : 0;
}

// How long the AI keeps its steering after a hit. Fixed (0.2 / 0.4 / 0.6 s), or "scaled": a light nudge is caught almost at
// once and a big hit takes longer, from 0.1 s at a closing speed of 1 m/s or less to 1.0 s at 10 m/s or more (smooth curve).
DWORD hit_ms(float impact)
{
    std::string_view v = hit_reaction.get();
    if (v == "scaled") {
        float t = std::clamp((impact - 1.0f) / 9.0f, 0.0f, 1.0f);
        return DWORD(100.0f + 900.0f * t * t * (3.0f - 2.0f * t));
    }
    return v == "0.2" ? 200 : v == "0.4" ? 400 : v == "0.6" ? 600 : 0;
}

bool read_inputs(int idx, float &steer, float &throttle)
{
    uintptr_t ai = ai_ctrl(idx);
    uint64_t out = 0;
    return ai && kizi::read(ai + 8, out) && out && kizi::read(out + 0x14, steer) && kizi::read(out + 0x0c, throttle);
}

uint32_t g_rng = 0x2545F491u;

void slide_hold(int idx)
{
    DWORD base = slide_ms();
    float steer = 0, throttle = 0;
    if (!base || holding(idx) || !read_inputs(idx, steer, throttle))
        return;                                    // off, already reacting late (a hit), or no AI controller
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 17, g_rng ^= g_rng << 5;
    DWORD ms = DWORD(float(base) * (0.7f + 0.6f * float(g_rng & 0xFFFF) / 65535.0f));
    g_hold[idx].steer = steer, g_hold[idx].throttle = std::clamp(throttle, 0.0f, 1.0f);
    g_hold[idx].until = GetTickCount() + ms;
    g_slides++, g_slide_ms_sum += ms;
}

bool human(uint64_t veh)                           // collision mass factor (physics +0x3e4): human 50, AI 45
{
    uint64_t phys = 0;
    float m = 0;
    return veh && kizi::read(veh + 0x558, phys) && phys && kizi::read(phys + 0x3e4, m) && m > 47.5f;
}

uint64_t body_of(uint64_t veh)                     // [[vehicle+0x558]+0x340]+0x18
{
    uint64_t phys = 0, rb = 0, body = 0;
    return veh && kizi::read(veh + 0x558, phys) && phys && kizi::read(phys + 0x340, rb) && rb && kizi::read(rb + 0x18, body)
               ? body
               : 0;
}

// Slip angle of every AI car at 20 Hz (between where it points and where it goes, on the car's ground plane); a slide
// starts when it passes 7 degrees above 15 m/s.
void watch_slides()
{
    static uint64_t veh_of[32], body[32];
    static float prev[32];
    if (!g_vehicles || !slide_ms())
        return;
    for (int i = 0; i < 32; i++) {
        uint64_t veh = 0;
        float beta = 0;
        if (kizi::read(g_vehicles + i * 8, veh) && veh) {
            if (!body[i] || veh_of[i] != veh)
                veh_of[i] = veh, body[i] = body_of(veh);
            float b[0x9c / 4] = {};                // body +0x190 .. +0x22c: left, up, fwd .. velocity
            if (body[i] && kizi::read(body[i] + 0x190, b, sizeof b)) {
                float up[3] = {b[4], b[5], b[6]}, fwd[3] = {b[8], b[9], b[10]};
                const float *vel = b + (0x220 - 0x190) / 4;
                float ul = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
                if (ul > 0.5f) {
                    for (float &u : up)
                        u /= ul;
                    float vu = vel[0] * up[0] + vel[1] * up[1] + vel[2] * up[2];
                    float fu = fwd[0] * up[0] + fwd[1] * up[1] + fwd[2] * up[2];
                    float vh[3], fh[3], sp = 0, fl = 0, d = 0;
                    for (int k = 0; k < 3; k++)
                        vh[k] = vel[k] - up[k] * vu, fh[k] = fwd[k] - up[k] * fu, sp += vh[k] * vh[k], fl += fh[k] * fh[k],
                        d += vh[k] * fh[k];
                    sp = std::sqrt(sp), fl = std::sqrt(fl);
                    if (sp > 15.0f && fl > 0.3f)
                        beta = std::acos(std::clamp(d / (sp * fl), -1.0f, 1.0f)) * 57.2958f;
                }
            } else
                body[i] = 0;
        }
        if (beta >= 7.0f && beta < 120.0f && prev[i] < 7.0f && !human(veh))
            slide_hold(i);                         // an AI car starts to slide: the driver reacts late
        prev[i] = beta;
    }
}

void worker()
{
    for (int tick = 1;; tick++) {
        Sleep(50);
        watch_slides();
        if (tick % 20 == 0)
            wrap_controllers();
        if (tick % 600 == 0 && g_slides) {
            int n = g_slides.exchange(0);
            swofty::log("[ai-driver-behaviour] slide reaction, last 30 s: %d AI slide(s) caught late, average %u ms", n,
                        g_slide_ms_sum.exchange(0) / unsigned(n));
        }
        if (tick % 600 == 0 && g_hits) {
            int n = g_hits.exchange(0);
            swofty::log("[ai-driver-behaviour] reaction after a hit, last 30 s: %d AI car(s), average %u ms, longest %u ms, "
                        "hardest hit %.1f m/s", n, g_hit_ms_sum.exchange(0) / unsigned(n), g_hit_ms_max.exchange(0),
                        double(g_impact_max.exchange(0)));
        }
    }
}

}  // namespace

// Called by Contact Physics (0.8.0 or newer) on its physics thread at the first record of a touch, once per AI car in it:
// idx = the car's vehicle id, impact = closing speed along the contact normal (m/s). Only writes this car's hold.
extern "C" __declspec(dllexport) void kizi_ai_hit(int idx, float impact)
{
    DWORD ms = hit_ms(impact);
    float steer = 0, throttle = 0;
    if (!ms || idx < 0 || idx >= 32 || !read_inputs(idx, steer, throttle))
        return;
    if (!holding(idx))
        g_hold[idx].steer = steer, g_hold[idx].throttle = -1.0f;   // a new hold freezes the steering of this moment
    g_hold[idx].until = GetTickCount() + ms;
    g_hits++;
    g_hit_ms_sum += ms;
    if (ms > g_hit_ms_max)
        g_hit_ms_max = ms;
    if (impact > g_impact_max)
        g_impact_max = impact;
}

SWOFTY_ON_LOAD
{
    std::string_view p = ai_spins.get();
    g_preset = p == "stock" ? "stock" : p == "some" ? "some" : p == "lots" ? "lots" : p == "custom" ? "custom" : "more";
    if (uintptr_t hit = kizi::scan_code("48 8D 05 ?? ?? ?? ?? 48 8B 0C F8 48 85 C9 0F 84 ?? ?? ?? ?? 4C 8B B1",
                                        "ai-driver-behaviour: vehicle table")) {
        int32_t d = 0;
        if (kizi::read(hit + 3, d))
            g_vehicles = hit + 7 + d;
    }
    // ControllerSet table (vehicle input code): mov rcx,[rip+G] ... mov rcx,[rcx+V]; mov rcx,[rcx+rdx*8]; mov edx,[rcx+M]
    if (uintptr_t hit = kizi::scan_code("48 8B 49 ?? 48 8B 0C D1 8B 51 ?? 33 51 ??", "ai-driver-behaviour: controller sets")) {
        for (uintptr_t a = hit - 8; a >= hit - 32; a--) {
            uint8_t op[3] = {};
            int32_t d = 0;
            if (kizi::read(a, op, 3) && op[0] == 0x48 && op[1] == 0x8B && op[2] == 0x0D && kizi::read(a + 3, d)) {
                g_sets_global = a + 7 + d;
                kizi::read(hit + 3, g_sets_vec);
                break;
            }
        }
    }
    swofty::log("[ai-driver-behaviour] AI reaction %s: AI spins %.*s (slide reaction %u ms), after a hit %.*s (hits come from "
                "Contact Physics)", g_sets_global && g_vehicles ? "ready" : "NOT available", int(g_preset.size()), g_preset.data(),
                unsigned(slide_ms()), int(hit_reaction.get().size()), hit_reaction.get().data());
    if (g_sets_global)
        std::thread(worker).detach();
}

SWOFTY_ADDON()
