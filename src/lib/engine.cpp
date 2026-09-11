#include "engine.hpp"

#include "log.hpp"
#include "niri_socket.hpp"
#include "subprocess.hpp"

#include <iostream>
#include <vector>

namespace engine {

void LocalSpawner::spawn(const std::vector<std::string>& argv) {
    if (subprocess::spawn_detached(argv) < 0) {
        std::cerr << "focus-event: local spawn failed for `" << argv[0] << "`\n";
    }
}

namespace {

std::string id_str(std::optional<uint64_t> id) {
    return id ? std::to_string(*id) : std::string("none");
}

} // namespace

Engine::Engine(config::Config cfg, Spawner& spawner)
    : cfg_(std::move(cfg)), spawner_(spawner) {}

void Engine::bootstrap() {
    refresh_from_niri();
    for (const auto& [_, w] : cache_) {
        if (w.is_focused) {
            last_focused_id_ = w.id;
            break;
        }
    }
}

void Engine::refresh_from_niri() {
    std::vector<niri::Window> ws;
    try {
        ws = niri_socket::query_windows();
    } catch (const std::exception& e) {
        std::cerr << "focus-event: refresh_from_niri failed: " << e.what() << "\n";
        return;
    }
    cache_.clear();
    cache_.reserve(ws.size());
    for (auto& w : ws) cache_.emplace(w.id, std::move(w));
}

std::optional<niri::Window> Engine::resolve(uint64_t id) {
    auto it = cache_.find(id);
    if (it != cache_.end()) return it->second;
    refresh_from_niri();
    it = cache_.find(id);
    if (it != cache_.end()) return it->second;
    return std::nullopt;
}

void Engine::fire_rules(config::Trigger t, const niri::Window& w) {
    for (const auto& rule : cfg_.rules) {
        if (rule.trigger != t) continue;
        if (config::matches(rule.selectors, w)) {
            for (const auto& act : rule.actions) {
                spawner_.spawn(act.argv);
            }
        }
    }
}

void Engine::set_focus(std::optional<uint64_t> new_id) {
    std::optional<uint64_t> old_id = last_focused_id_;
    if (old_id == new_id) return;
    LOG_DEBUG("focus-event: focus " << id_str(old_id) << " -> " << id_str(new_id) << "\n");
    if (old_id) {
        auto prev = resolve(*old_id);
        if (prev) fire_rules(config::Trigger::Blur, *prev);
    }
    if (new_id) {
        auto cur = resolve(*new_id);
        if (cur) fire_rules(config::Trigger::Focus, *cur);
    }
    last_focused_id_ = new_id;
}

void Engine::reconcile_focus() {
    std::optional<niri::Window> w;
    try {
        w = niri_socket::query_focused_window();
    } catch (const std::exception& e) {
        LOG_ERROR("focus-event: focus reconcile query failed: " << e.what() << "\n");
        return;
    }
    LOG_DEBUG("focus-event: reconcile: niri reports focused "
              << id_str(w ? std::optional<uint64_t>(w->id) : std::nullopt) << "\n");
    set_focus(w ? std::optional<uint64_t>(w->id) : std::nullopt);
}

void Engine::handle_event(const niri::Event& ev) {
    switch (ev.kind) {
        case niri::Event::WindowOpenedOrChanged: {
            if (ev.window) {
                cache_[ev.window->id] = *ev.window;
                // niri's diff loop returns early for new/changed windows, which
                // swallows the WindowFocusChanged for a window that gained
                // focus while opening or changing; the payload still carries
                // authoritative focus, so derive the transition here.
                if (ev.window->is_focused) set_focus(ev.window->id);
            }
            break;
        }
        case niri::Event::WindowClosed: {
            if (ev.closed_id) cache_.erase(*ev.closed_id);
            if (ev.closed_id && last_focused_id_ == ev.closed_id) {
                last_focused_id_.reset();
            }
            break;
        }
        case niri::Event::WindowsChanged: {
            cache_.clear();
            for (const auto& w : ev.windows) cache_[w.id] = w;
            // The full snapshot is authoritative (niri's mirror keeps at most
            // one focused window), including "no focused window".
            std::optional<uint64_t> focused;
            for (const auto& w : ev.windows) {
                if (w.is_focused) {
                    focused = w.id;
                    break;
                }
            }
            set_focus(focused);
            break;
        }
        case niri::Event::WindowFocusChanged: {
            set_focus(ev.focused_id);
            break;
        }
        case niri::Event::Unknown:
        default:
            break;
    }
}

} // namespace engine
