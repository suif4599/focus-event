// Config data structures and loader.
//
// File format (KDL-like):
//   on-focus <selector>... {
//       spawn "prog" "arg1" "arg2"
//       spawn ...
//   }
//   on-blur <selector>... { ... }
//
// Selectors (regex unless noted), combined with logical AND:
//   app-id="regex"           match the window's app_id
//   title="regex"            match the window's title
//   id=42                    match the numeric window id
//   workspace-id=3           match workspace_id
//   is-floating=true|false   match is_floating flag
//   is-urgent=true|false     match is_urgent flag
//
// An absent selector matches anything. A rule with no selectors matches every window.
//
// Example:
//   on-focus app-id="codium" title=".*focus-event.*" {
//       spawn "pactl" "set-sink-mute" "@DEFAULT_SINK@" "1"
//   }
//   on-blur app-id="codium" {
//       spawn "pactl" "set-sink-mute" "@DEFAULT_SINK@" "0"
//   }

#pragma once

#include "niri.hpp"

#include <optional>
#include <regex>
#include <string>
#include <variant>
#include <vector>

namespace config {

enum class Trigger { Focus, Blur };

struct StringMatcher {
    std::string pattern;
    std::regex re;
};

struct BoolMatcher {
    bool expected;
};

struct IntMatcher {
    long long expected;
};

// One selector entry — a (field, matcher) pair.
struct Selector {
    enum Field { AppId, Title, Id, WorkspaceId, IsFloating, IsUrgent };
    Field field;
    std::variant<StringMatcher, BoolMatcher, IntMatcher> matcher;
};

struct SpawnAction {
    std::vector<std::string> argv;  // argv[0] = program
};

struct Rule {
    Trigger trigger;
    std::vector<Selector> selectors;   // AND
    std::vector<SpawnAction> actions;
};

struct Config {
    std::vector<Rule> rules;
};

// Load and parse a config file. Throws std::runtime_error on failure.
Config load_file(const std::string& path);

// Load and parse config from a string (used for testing).
Config load_string(std::string_view src, const std::string& origin_name);

// Returns true if every selector matches the window.
// A window without id (nullopt) only matches selectors that don't reference the id.
bool matches(const std::vector<Selector>& selectors, const niri::Window& w);

} // namespace config
