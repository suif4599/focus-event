// Engine: owns the window cache, applies config rules in response to events,
// lazily refreshes the cache when unknown window ids surface.
//
// Spawn dispatch is pluggable via the Spawner interface so the same engine
// drives both the all-in-one binary (local spawn via fork+exec) and the
// split trigger/executor pair (remote spawn over a Unix domain socket).
//
// Lifecycle:
//   Engine eng(cfg, spawner);
//   eng.bootstrap();                 // query niri for the window list once at startup
//   eng.handle_event(ev);            // for each event from the stream
//   eng.reconcile_focus();           // after event bursts, re-check the focused window

#pragma once

#include "config.hpp"
#include "niri.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace engine {

// Abstract spawn sink. Implementations:
//   - LocalSpawner: fork+setsid+execvp (used by the standalone binary)
//   - RemoteSpawner: write a framed SPAWN message to a UDS fd (used by trigger)
struct Spawner {
    virtual ~Spawner() = default;
    virtual void spawn(const std::vector<std::string>& argv) = 0;
};

// Local fork+setsid+execvp. No remote counterpart; useful for the standalone
// build and for tests.
class LocalSpawner : public Spawner {
public:
    void spawn(const std::vector<std::string>& argv) override;
};

class Engine {
public:
    // Spawner reference must outlive the engine.
    Engine(config::Config cfg, Spawner& spawner);

    // Populate the cache by querying niri over $NIRI_SOCKET. Throws on failure.
    void bootstrap();

    // Process one event from the stream. May synchronously query niri for the
    // full window list if a referenced window id is missing from the cache.
    void handle_event(const niri::Event& ev);

    // Ask niri for the currently focused window and apply it via set_focus().
    // Safety net for focus transitions niri's diff loop swallowed; failures
    // are logged, never fatal.
    void reconcile_focus();

    // For diagnostics: current size of the window cache.
    std::size_t cache_size() const { return cache_.size(); }

private:
    // Apply a focus transition (Blur on the old window, Focus on the new one).
    // Idempotent: returns immediately when the focus is unchanged, so every
    // source (WindowFocusChanged, is_focused derivation, reconcile) can call
    // it without double-firing rules.
    void set_focus(std::optional<uint64_t> new_id);

    // Resolve a window id to its info. Returns nullopt if unknown AND we can't
    // refresh. Triggers a full cache refresh on miss.
    std::optional<niri::Window> resolve(uint64_t id);

    // Re-fetch the full window list and replace the cache.
    void refresh_from_niri();

    // Fire on-focus or on-blur rules against a window.
    void fire_rules(config::Trigger t, const niri::Window& w);

    config::Config cfg_;
    Spawner& spawner_;
    std::unordered_map<uint64_t, niri::Window> cache_;
    std::optional<uint64_t> last_focused_id_;
};

} // namespace engine
