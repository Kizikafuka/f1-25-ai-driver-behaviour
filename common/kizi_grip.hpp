// SPDX-License-Identifier: MIT
// One live-car layer per AI car on the field "ai.grip_scale" (Swofty 1.7, experimental): the grip the AI plans its corner
// speeds from. Layers use the `scale` operation, so layers from different addons multiply instead of replacing each other.
// A factor above 1 plans faster than the car can drive (slides, spins); below 1 it drives safe.
// The core keeps a layer through car rebuilds. The layers are refreshed about once a second on the flow thread, so cars that
// appear later, and cars that change between AI and you (AI Drive), are handled. A new session rolls a new random seed.
// Make one Layers with `new` and never delete it: the layers must not call the core while the game unloads.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <random>
#include <span>
#include <string>
#include <swofty/events.hpp>
#include <swofty/systems/cars.hpp>

namespace kizi::grip {

namespace live = swofty::cars::live;

// A car's own number from the seed, -1 .. 1, the same all session.
inline float unit(uint32_t seed, uint32_t idx)
{
    uint32_t x = seed ^ (idx * 0x9E3779B9u);
    x ^= x >> 16, x *= 0x7feb352du, x ^= x >> 15, x *= 0x846ca68bu, x ^= x >> 16;
    return float(x) / 2147483647.5f - 1.0f;
}

class Layers {
public:
    // factor(car index, session seed) -> grip factor for that AI car (1 = leave it alone). Called on the flow thread.
    Layers(const char *tag, std::function<float(uint32_t, uint32_t)> factor) : tag_(tag), factor_(std::move(factor)) {}

    void start()
    {
        auto r1 = swofty::events::on("session.start").after(
            [this](std::span<const std::byte>, const swofty::events::Context &) { new_session_ = true; });
        auto r2 = swofty::events::on_frame(&Layers::frame_entry, this);
        swofty::log("[%s] grip layers: session.start %s, frame callback %s", tag_, swofty::events::name(r1),
                    swofty::events::name(r2));
    }

private:
    struct Car {
        float factor;
        live::Layer layer;                         // removed (the car goes back to the game's grip) when this is destroyed
    };

    static void frame_entry(double dt, void *self) { static_cast<Layers *>(self)->frame(dt); }

    void frame(double dt)
    {
        acc_ += dt;
        if (acc_ < 1.0)
            return;
        acc_ = 0;
        update();
    }

    void update()
    {
        if (new_session_.exchange(false)) {
            cars_.clear();
            seed_ = std::random_device{}();
        }
        if (refused_)
            return;
        if (!live::available()) {
            if (!missing_logged_) {
                missing_logged_ = true;
                swofty::log("[%s] grip: the live car operations are not available (needs Swofty Launcher 1.7 and this game build)",
                            tag_);
            }
            return;
        }
        auto list = live::cars();
        std::map<uint32_t, const live::Car *> ai;
        for (const auto &c : list)
            if (c.is_ai())
                ai[c.index()] = &c;
        for (auto it = cars_.begin(); it != cars_.end();)   // your car after AI Drive handed it back, cars that left
            it = ai.count(it->first) ? std::next(it) : cars_.erase(it);
        int made = 0;
        float lo = 9, hi = 0;
        for (auto &[idx, car] : ai) {
            float f = std::clamp(factor_(idx, seed_), 0.8f, 1.2f);
            auto it = cars_.find(idx);
            bool none = std::fabs(f - 1.0f) < 1e-6f;
            if (it != cars_.end() && !none && std::fabs(it->second.factor - f) < 1e-6f)
                continue;
            if (it != cars_.end())
                cars_.erase(it);
            if (none)
                continue;
            live::Layer l = live::Layer::scale(live::Cars::car(*car), "ai.grip_scale", f);
            if (!l.ok()) {
                swofty::Result r = l.result();
                if (r == swofty::Result::unavailable || r == swofty::Result::invalid) {
                    refused_ = true;
                    swofty::log("[%s] grip: the core refused the field ai.grip_scale (%s): not used", tag_, swofty::result_name(r));
                    return;
                }
                continue;                          // late (the game is rebuilding): tried again next second
            }
            cars_.emplace(idx, Car{f, std::move(l)});
            made++, lo = std::min(lo, f), hi = std::max(hi, f);
        }
        if (made)
            swofty::log("[%s] grip: %d AI car(s) set, factor %.3f to %.3f", tag_, made, lo, hi);
        was_set_ = was_set_ || made;
        if (cars_.empty() && was_set_) {
            was_set_ = false;
            swofty::log("[%s] grip: back to the game's", tag_);
        }
    }

    const char *tag_;
    std::function<float(uint32_t, uint32_t)> factor_;
    std::map<uint32_t, Car> cars_;
    uint32_t seed_ = std::random_device{}();
    std::atomic<bool> new_session_{false};
    bool refused_ = false, missing_logged_ = false, was_set_ = false;
    double acc_ = 0;
};

}
