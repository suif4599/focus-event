#include "config.hpp"

#include "kdl.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace config {

namespace {

Trigger parse_trigger(const std::string& name) {
    if (name == "on-focus") return Trigger::Focus;
    if (name == "on-blur")  return Trigger::Blur;
    throw std::runtime_error("unexpected top-level node '" + name +
                             "' (expected 'on-focus' or 'on-blur')");
}

Selector::Field parse_field(const std::string& key) {
    if (key == "app-id")       return Selector::AppId;
    if (key == "title")        return Selector::Title;
    if (key == "id")           return Selector::Id;
    if (key == "workspace-id") return Selector::WorkspaceId;
    if (key == "is-floating")  return Selector::IsFloating;
    if (key == "is-urgent")    return Selector::IsUrgent;
    throw std::runtime_error("unknown selector '" + key + "'");
}

bool parse_bool(const std::string& v) {
    if (v == "true")  return true;
    if (v == "false") return false;
    throw std::runtime_error("expected 'true' or 'false', got '" + v + "'");
}

long long parse_int(const std::string& v) {
    // Allow leading +/-, base 10. Reject on overflow / garbage.
    errno = 0;
    char* end = nullptr;
    long long n = std::strtoll(v.c_str(), &end, 10);
    if (errno != 0 || end == v.c_str() || *end != '\0') {
        throw std::runtime_error("expected integer, got '" + v + "'");
    }
    return n;
}

Selector make_selector(const std::string& key, const std::string& value) {
    Selector s;
    s.field = parse_field(key);
    switch (s.field) {
        case Selector::AppId:
        case Selector::Title: {
            StringMatcher m;
            m.pattern = value;
            m.re = std::regex(value, std::regex::ECMAScript | std::regex::optimize);
            s.matcher = std::move(m);
            break;
        }
        case Selector::Id:
        case Selector::WorkspaceId: {
            IntMatcher m;
            m.expected = parse_int(value);
            s.matcher = m;
            break;
        }
        case Selector::IsFloating:
        case Selector::IsUrgent: {
            BoolMatcher m;
            m.expected = parse_bool(value);
            s.matcher = m;
            break;
        }
    }
    return s;
}

SpawnAction parse_spawn(const kdl::Node& node) {
    SpawnAction act;
    act.argv = node.values;
    if (act.argv.empty()) throw std::runtime_error("'spawn' requires at least a program name");
    return act;
}

Rule parse_rule(const kdl::Node& node) {
    Rule r;
    r.trigger = parse_trigger(node.name);
    for (const auto& [k, v] : node.props) {
        r.selectors.push_back(make_selector(k, v));
    }
    for (const auto& child : node.children) {
        if (child.name == "spawn") {
            r.actions.push_back(parse_spawn(child));
        } else {
            throw std::runtime_error("unexpected child node '" + child.name +
                                     "' inside '" + node.name + "' (only 'spawn' is allowed)");
        }
    }
    return r;
}

} // namespace

Config load_string(std::string_view src, const std::string& origin_name) {
    std::vector<kdl::Node> nodes;
    try {
        nodes = kdl::parse(src);
    } catch (const kdl::ParseError& e) {
        throw std::runtime_error(origin_name + ": " + e.what());
    }

    Config cfg;
    for (const auto& node : nodes) {
        try {
            cfg.rules.push_back(parse_rule(node));
        } catch (const std::runtime_error& e) {
            throw std::runtime_error(origin_name + ": " + e.what());
        }
    }
    return cfg;
}

Config load_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) throw std::runtime_error("cannot open config file: " + path);
    std::stringstream ss;
    ss << ifs.rdbuf();
    return load_string(ss.str(), path);
}

bool matches(const std::vector<Selector>& selectors, const niri::Window& w) {
    for (const auto& s : selectors) {
        if (s.field == Selector::AppId) {
            const auto& m = std::get<StringMatcher>(s.matcher);
            if (!std::regex_search(w.app_id, m.re)) return false;
        } else if (s.field == Selector::Title) {
            const auto& m = std::get<StringMatcher>(s.matcher);
            if (!std::regex_search(w.title, m.re)) return false;
        } else if (s.field == Selector::Id) {
            const auto& m = std::get<IntMatcher>(s.matcher);
            if ((long long)w.id != m.expected) return false;
        } else if (s.field == Selector::WorkspaceId) {
            const auto& m = std::get<IntMatcher>(s.matcher);
            if (!w.workspace_id.has_value() || *w.workspace_id != m.expected) return false;
        } else if (s.field == Selector::IsFloating) {
            const auto& m = std::get<BoolMatcher>(s.matcher);
            if (w.is_floating != m.expected) return false;
        } else if (s.field == Selector::IsUrgent) {
            const auto& m = std::get<BoolMatcher>(s.matcher);
            if (w.is_urgent != m.expected) return false;
        }
    }
    return true;
}

} // namespace config
