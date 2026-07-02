// Engine: owns the window cache, applies config rules in response to events,
// lazily refreshes the cache when unknown window ids surface.
//
// Spawn dispatch is pluggable via the Spawner interface so the same engine
// drives both the all-in-one binary (local spawn via fork+exec) and the
// split trigger/executor pair (remote spawn over a Unix domain socket).
//
// Lifecycle:
//   Engine eng(cfg, spawner);
//   eng.bootstrap();                 // call `niri msg -j windows` once at startup
//   eng.handle_event(ev);            // for each event from the stream

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

    config::Config cfg_;
    Spawner& spawner_;
    std::unordered_map<uint64_t, niri::Window> cache_;
    std::optional<uint64_t> last_focused_id_;
};

} // namespace engine
