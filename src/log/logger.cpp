#include "ptl/log/logger.hpp"

#include <atomic>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>

#include "ptl/core/clock.hpp"
#include "ptl/core/types.hpp"

namespace ptl::log {
namespace {

std::mutex g_mutex;
Config g_config;
std::ofstream g_file;
std::atomic<std::uint64_t> g_dropped{0};
std::deque<Logger>* g_loggers = nullptr;

/// Minimal JSON string escaping. Structured logs are only useful if `jq` can
/// parse them, and an unescaped quote in a symbol or error message silently
/// corrupts the whole line.
void escape_json(std::string& out, std::string_view s) {
    for (const char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
}

void append_value(std::string& out, const FieldValue& v) {
    std::visit(
        [&out](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            char buf[64];
            if constexpr (std::is_same_v<T, std::int64_t>) {
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(x));
                out += buf;
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(x));
                out += buf;
            } else if constexpr (std::is_same_v<T, double>) {
                // 17 significant digits round-trips an IEEE double exactly.
                // Logs are an audit trail; a truncated price is a lie.
                std::snprintf(buf, sizeof(buf), "%.17g", x);
                out += buf;
            } else if constexpr (std::is_same_v<T, bool>) {
                out += x ? "true" : "false";
            } else {
                out += '"';
                escape_json(out, x);
                out += '"';
            }
        },
        v);
}

}  // namespace

std::string_view to_string(Level l) noexcept {
    switch (l) {
        case Level::Trace:
            return "trace";
        case Level::Debug:
            return "debug";
        case Level::Info:
            return "info";
        case Level::Warn:
            return "warn";
        case Level::Error:
            return "error";
        case Level::Critical:
            return "critical";
        case Level::Off:
            return "off";
    }
    return "unknown";
}

bool parse_level(std::string_view text, Level& out) noexcept {
    if (text == "trace") {
        out = Level::Trace;
        return true;
    }
    if (text == "debug") {
        out = Level::Debug;
        return true;
    }
    if (text == "info") {
        out = Level::Info;
        return true;
    }
    if (text == "warn") {
        out = Level::Warn;
        return true;
    }
    if (text == "error") {
        out = Level::Error;
        return true;
    }
    if (text == "critical") {
        out = Level::Critical;
        return true;
    }
    if (text == "off") {
        out = Level::Off;
        return true;
    }
    return false;
}

void Logger::log(Level level, std::string_view message, std::span<const Field> fields) const {
    std::string line;
    line.reserve(160 + fields.size() * 32);

    // sim_time first, wall_time last: wall_time is the ONLY field that varies
    // between two identical runs, so a parity diff strips one trailing field
    // rather than parsing the record.
    if (g_config.json) {
        line += '{';
        if (g_config.sim_clock != nullptr) {
            line += R"("sim_time":")";
            line += to_iso8601(g_config.sim_clock->now());
            line += R"(",)";
        }
        line += R"("level":")";
        line += to_string(level);
        line += R"(","subsystem":")";
        escape_json(line, subsystem_);
        line += R"(","msg":")";
        escape_json(line, message);
        line += '"';
        for (const auto& f : fields) {
            line += ",\"";
            escape_json(line, f.key);
            line += "\":";
            append_value(line, f.value);
        }
        line += R"(,"wall_time":")";
        line += to_iso8601(WallClock{}.now());
        line += R"(")";
        line += '}';
    } else {
        line += to_iso8601(g_config.sim_clock != nullptr ? g_config.sim_clock->now()
                                                         : WallClock{}.now());
        line += " [";
        line += to_string(level);
        line += "] ";
        line += subsystem_;
        line += ": ";
        line += message;
        for (const auto& f : fields) {
            line += ' ';
            line += f.key;
            line += '=';
            append_value(line, f.value);
        }
    }

    const std::lock_guard lock(g_mutex);
    if (g_config.console) {
        auto& os = (level >= Level::Error) ? std::cerr : std::cout;
        os << line << '\n';
    }
    if (g_file.is_open()) g_file << line << '\n';
}

Logger& get(std::string_view name) {
    const std::lock_guard lock(g_mutex);
    if (g_loggers == nullptr) g_loggers = new std::deque<Logger>();
    for (auto& lg : *g_loggers) {
        if (lg.subsystem() == name) return lg;
    }
    // deque: references handed out must stay valid as more loggers appear.
    g_loggers->push_back(Logger{name});
    g_loggers->back().set_level(g_config.level);
    return g_loggers->back();
}

void init(const Config& cfg) {
    const std::lock_guard lock(g_mutex);
    g_config = cfg;
    if (!cfg.file.empty()) {
        g_file.open(cfg.file, std::ios::app);
    }
    if (g_loggers != nullptr) {
        for (auto& lg : *g_loggers) lg.set_level(cfg.level);
    }
}

void shutdown() {
    const std::lock_guard lock(g_mutex);
    // Drop the borrowed clock before the caller can destroy it.
    g_config.sim_clock = nullptr;
    if (g_file.is_open()) {
        g_file.flush();
        g_file.close();
    }
}

std::uint64_t dropped_records() noexcept {
    return g_dropped.load(std::memory_order_relaxed);
}

}  // namespace ptl::log
