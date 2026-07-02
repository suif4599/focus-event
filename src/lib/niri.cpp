#include "niri.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace niri {

using json = nlohmann::json;

namespace {

Window window_from_json(const json& j) {
    Window w;
    if (j.contains("id")) w.id = j["id"].get<uint64_t>();
    if (j.contains("title")) w.title = j["title"].get<std::string>();
    if (j.contains("app_id")) w.app_id = j["app_id"].get<std::string>();
    if (j.contains("pid")) w.pid = j["pid"].get<int64_t>();
    if (j.contains("workspace_id") && !j["workspace_id"].is_null())
        w.workspace_id = j["workspace_id"].get<int64_t>();
    if (j.contains("is_focused")) w.is_focused = j["is_focused"].get<bool>();
    if (j.contains("is_floating")) w.is_floating = j["is_floating"].get<bool>();
    if (j.contains("is_urgent")) w.is_urgent = j["is_urgent"].get<bool>();
    return w;
}

} // namespace

std::vector<Window> parse_windows(std::string_view text) {
    std::vector<Window> out;
    if (text.empty()) return out;
    json j = json::parse(text, nullptr, true, true);
    if (!j.is_array()) throw std::runtime_error("`niri msg -j windows` returned non-array JSON");
    out.reserve(j.size());
    for (const auto& w : j) out.push_back(window_from_json(w));
    return out;
}

Event parse_event(std::string_view line) {
    Event ev;
    if (line.empty()) {
        ev.kind = Event::Unknown;
        return ev;
    }
    json j;
    try {
        j = json::parse(line, nullptr, true, true);
    } catch (...) {
        // Skip malformed lines silently rather than tearing down the daemon.
        ev.kind = Event::Unknown;
        return ev;
    }
    if (!j.is_object() || j.empty()) {
        ev.kind = Event::Unknown;
        return ev;
    }

    // Each event line is a single-key object whose key names the event type.
    auto it = j.begin();
    ev.raw_tag = it.key();
    const json& payload = it.value();

    if (ev.raw_tag == "WindowFocusChanged") {
        ev.kind = Event::WindowFocusChanged;
        if (payload.contains("id") && !payload["id"].is_null())
            ev.focused_id = payload["id"].get<uint64_t>();
    } else if (ev.raw_tag == "WindowOpenedOrChanged") {
        ev.kind = Event::WindowOpenedOrChanged;
        if (payload.contains("window") && payload["window"].is_object())
            ev.window = window_from_json(payload["window"]);
    } else if (ev.raw_tag == "WindowClosed") {
        ev.kind = Event::WindowClosed;
        if (payload.contains("id"))
            ev.closed_id = payload["id"].get<uint64_t>();
    } else if (ev.raw_tag == "WindowsChanged") {
        ev.kind = Event::WindowsChanged;
        if (payload.contains("windows") && payload["windows"].is_array()) {
            for (const auto& w : payload["windows"])
                ev.windows.push_back(window_from_json(w));
        }
    } else {
        ev.kind = Event::Unknown;
    }
    return ev;
}

} // namespace niri
