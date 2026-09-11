// Niri IPC types and helpers.
//
// We talk to niri directly over its IPC socket ($NIRI_SOCKET): one request
// encoded as a single-line JSON string ("EventStream", "Windows",
// "FocusedWindow"), one reply per line wrapped as {"Ok":...}/{"Err":"..."}.
// After an "EventStream" request niri keeps streaming one JSON event per line.
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

// Parse a single line of the niri event stream.
// Returns Event::Unknown (with the raw tag) for event types we don't model.
Event parse_event(std::string_view line);

// Parse a "Windows" reply line: {"Ok":{"Windows":[...]}}. Throws
// std::runtime_error on {"Err":...}, malformed JSON, or a wrong shape.
std::vector<Window> parse_windows_reply(std::string_view line);

// Parse a "FocusedWindow" reply line: {"Ok":{"FocusedWindow":null|{...}}}.
// nullopt means niri reports no focused window. Throws on {"Err":...} or a
// wrong shape.
std::optional<Window> parse_focused_window_reply(std::string_view line);

// Validate an "EventStream" handshake reply: {"Ok":"Handled"}. Throws on
// {"Err":...} or anything else.
void expect_handled_reply(std::string_view line);

} // namespace niri
