// Engine: owns the window cache, applies config rules in response to events,
// lazily refreshes the cache when unknown window ids surface.
//
// Lifecycle:
//   Engine eng(cfg);
//   eng.bootstrap();                 // call `niri msg -j windows` once at startup
//   eng.handle_event(ev);            // for each event from the stream
//
// The engine is single-threaded by design; all calls happen on the epoll loop
// thread.

#pragma once

#include "config.hpp"
#include "niri.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace engine {

class Engine {
public:
    explicit Engine(config::Config cfg);

    // Populate the cache from `niri msg -j windows`. Throws on failure.
    void bootstrap();

    // Process one event from the stream. May synchronously call `niri msg -j
    // windows` if a referenced window id is missing from the cache.
    void handle_event(const niri::Event& ev);

    // For diagnostics: current size of the window cache.
    std::size_t cache_size() const { return cache_.size(); }

private:
    // Resolve a window id to its info. Returns nullopt if unknown AND we can't
    // refresh. Triggers a full cache refresh on miss.
    std::optional<niri::Window> resolve(uint64_t id);

    // Re-fetch the full window list and replace the cache.
    void refresh_from_niri();

    // Fire on-focus or on-blur rules against a window.
    void fire_rules(config::Trigger t, const niri::Window& w);

    // Run all spawn actions for matched rules.
    static void run_actions(const std::vector<config::SpawnAction>& actions);

    config::Config cfg_;
    std::unordered_map<uint64_t, niri::Window> cache_;
    std::optional<uint64_t> last_focused_id_;
};

} // namespace engine
