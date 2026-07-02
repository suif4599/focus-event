#include "engine.hpp"

#include "subprocess.hpp"

#include <iostream>
#include <vector>

namespace engine {

namespace {

std::vector<std::string> niri_windows_argv() {
    return {"niri", "msg", "-j", "windows"};
}

} // namespace

Engine::Engine(config::Config cfg) : cfg_(std::move(cfg)) {}

void Engine::bootstrap() {
    refresh_from_niri();
    // Seed last_focused_id_ from whatever window the cache says is focused.
    for (const auto& [_, w] : cache_) {
        if (w.is_focused) {
            last_focused_id_ = w.id;
            break;
        }
    }
}

void Engine::refresh_from_niri() {
    std::string out;
    try {
        out = subprocess::run_capture_stdout(niri_windows_argv());
    } catch (const std::exception& e) {
        std::cerr << "focus-event: refresh_from_niri failed: " << e.what() << "\n";
        return;
    }
    std::vector<niri::Window> ws;
    try {
        ws = niri::parse_windows(out);
    } catch (const std::exception& e) {
        std::cerr << "focus-event: parse_windows failed: " << e.what() << "\n";
        return;
    }
    cache_.clear();
    cache_.reserve(ws.size());
    for (auto& w : ws) cache_.emplace(w.id, std::move(w));
}

std::optional<niri::Window> Engine::resolve(uint64_t id) {
    auto it = cache_.find(id);
    if (it != cache_.end()) return it->second;
    // Miss — refresh once and try again.
    refresh_from_niri();
    it = cache_.find(id);
    if (it != cache_.end()) return it->second;
    return std::nullopt;
}

void Engine::run_actions(const std::vector<config::SpawnAction>& actions) {
    for (const auto& act : actions) {
        if (subprocess::spawn_detached(act.argv) < 0) {
            std::cerr << "focus-event: spawn failed for `" << act.argv[0] << "`\n";
        }
    }
}

void Engine::fire_rules(config::Trigger t, const niri::Window& w) {
    for (const auto& rule : cfg_.rules) {
        if (rule.trigger != t) continue;
        if (config::matches(rule.selectors, w)) {
            run_actions(rule.actions);
        }
    }
}

void Engine::handle_event(const niri::Event& ev) {
    switch (ev.kind) {
        case niri::Event::WindowOpenedOrChanged: {
            if (ev.window) {
                cache_[ev.window->id] = *ev.window;
            }
            break;
        }
        case niri::Event::WindowClosed: {
            if (ev.closed_id) cache_.erase(*ev.closed_id);
            // If the closed window was the last focused one, treat as blur.
            if (ev.closed_id && last_focused_id_ == ev.closed_id) {
                // We don't have window info anymore; skip firing blur for it.
                last_focused_id_.reset();
            }
            break;
        }
        case niri::Event::WindowsChanged: {
            cache_.clear();
            for (const auto& w : ev.windows) cache_[w.id] = w;
            break;
        }
        case niri::Event::WindowFocusChanged: {
            std::optional<uint64_t> new_id = ev.focused_id;
            std::optional<uint64_t> old_id = last_focused_id_;

            // Skip no-op re-focuses (same id reported twice in a row).
            if (old_id == new_id) break;

            // Fire blur on the previously focused window (if any).
            if (old_id) {
                auto prev = resolve(*old_id);
                if (prev) fire_rules(config::Trigger::Blur, *prev);
            }
            // Fire focus on the new window (if any).
            if (new_id) {
                auto cur = resolve(*new_id);
                if (cur) fire_rules(config::Trigger::Focus, *cur);
            }
            last_focused_id_ = new_id;
            break;
        }
        case niri::Event::Unknown:
        default:
            break;
    }
}

} // namespace engine
