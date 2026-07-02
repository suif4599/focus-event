// Niri IPC types and helpers.
//
// We talk to niri via two JSON-producing subcommands of `niri msg`:
//   - `niri msg -j event-stream`: long-running, prints one JSON event per line.
//   - `niri msg -j windows`: one-shot, returns the full window list as a JSON array.
//
// The Window struct mirrors the fields we actually use; unknown fields are ignored.
// Event types are kept minimal: only the ones we react to are modeled precisely.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace niri {

struct Window {
    uint64_t id = 0;
    std::string title;
    std::string app_id;
    int64_t pid = 0;
    std::optional<int64_t> workspace_id;
    bool is_focused = false;
    bool is_floating = false;
    bool is_urgent = false;
};

// Events we model. Unknown events come back as Unknown (with the raw tag name).
struct Event {
    enum Kind {
        Unknown,
        WindowFocusChanged,
        WindowOpenedOrChanged,
        WindowClosed,
        WindowsChanged,
    };
    Kind kind = Unknown;
    std::string raw_tag;

    // WindowFocusChanged
    std::optional<uint64_t> focused_id;

    // WindowOpenedOrChanged
    std::optional<Window> window;

    // WindowClosed
    std::optional<uint64_t> closed_id;

    // WindowsChanged (full list snapshot)
    std::vector<Window> windows;
};

// Parse a single line of `niri msg -j event-stream` output.
// Returns Event::Unknown (with the raw tag) for event types we don't model.
Event parse_event(std::string_view line);

// Parse the output of `niri msg -j windows` (a JSON array of window objects).
std::vector<Window> parse_windows(std::string_view json);

} // namespace niri
