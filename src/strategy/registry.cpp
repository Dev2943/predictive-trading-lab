#include "ptl/strategy/registry.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iomanip>
#include <sstream>

namespace ptl::strategy {
namespace {

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

void hash_bytes(std::uint64_t& h, const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 0x100000001b3ULL;
    }
}

void hash_string(std::uint64_t& h, std::string_view s) {
    hash_bytes(h, s.data(), s.size());
}

[[nodiscard]] std::string json_escape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
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
                    std::ostringstream ss;
                    ss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(static_cast<unsigned char>(c));
                    out += ss.str();
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// StrategyId
// ---------------------------------------------------------------------------

Result<StrategyId> StrategyId::create(std::string value) {
    if (value.empty()) return fail(bad("strategy id cannot be empty"));
    if (value.size() > 128) {
        return fail(bad("strategy id exceeds 128 characters", value.substr(0, 32)));
    }
    // Restricted to what is unambiguous in a filename, a config key and a JSON
    // field at once. Permissive validation here means every downstream layer
    // can embed the id without escaping it.
    for (const char c : value) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return fail(bad("strategy id contains an unsafe character",
                            std::string{"'"} + c + "' in " + value));
        }
    }
    return StrategyId{std::move(value)};
}

std::uint64_t StrategyId::hash() const noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_string(h, value_);
    return h;
}

// ---------------------------------------------------------------------------
// StrategyVersion
// ---------------------------------------------------------------------------

Result<StrategyVersion> StrategyVersion::parse(std::string_view text) {
    StrategyVersion out;
    std::uint32_t* fields[] = {&out.major, &out.minor, &out.patch};

    std::size_t field = 0;
    std::size_t start = 0;
    while (field < 3) {
        const std::size_t dot = text.find('.', start);
        const std::string_view part = text.substr(
            start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (part.empty()) return fail(bad("version has an empty component", std::string{text}));

        const auto* first = part.data();
        const auto* last = part.data() + part.size();
        const auto result = std::from_chars(first, last, *fields[field]);
        if (result.ec != std::errc{} || result.ptr != last) {
            return fail(bad("version component is not a number", std::string{text}));
        }
        ++field;
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    if (field != 3) {
        return fail(bad("version must be major.minor.patch", std::string{text}));
    }
    return out;
}

std::string StrategyVersion::to_string() const {
    std::ostringstream ss;
    ss << major << '.' << minor << '.' << patch;
    return ss.str();
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

std::string_view to_string(StrategyState s) noexcept {
    switch (s) {
        case StrategyState::Draft:
            return "draft";
        case StrategyState::Research:
            return "research";
        case StrategyState::Candidate:
            return "candidate";
        case StrategyState::Production:
            return "production";
        case StrategyState::Deprecated:
            return "deprecated";
        case StrategyState::Retired:
            return "retired";
    }
    return "unknown";
}

bool accepts_new_experiments(StrategyState s) noexcept {
    // A deprecated strategy may still be REPRODUCED -- old results must remain
    // regenerable -- but no new research should start against it.
    return s == StrategyState::Research || s == StrategyState::Candidate ||
           s == StrategyState::Production;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

Result<bool> ParameterSpec::validate_value(std::string_view value) const {
    if (value.empty()) {
        if (required) return fail(bad("required parameter '" + name + "' is empty"));
        return true;
    }

    const auto numeric = [&](bool integral) -> Result<bool> {
        std::size_t consumed = 0;
        try {
            if (integral) {
                (void)std::stoll(std::string{value}, &consumed);
            } else {
                (void)std::stod(std::string{value}, &consumed);
            }
        } catch (...) {
            return fail(
                bad("parameter '" + name + "' is not a " + (integral ? "integer" : "number"),
                    std::string{value}));
        }
        if (consumed != value.size()) {
            // Trailing characters mean the value was only partly parsed, which
            // is how "1.5x" silently becomes 1.5.
            return fail(
                bad("parameter '" + name + "' has trailing characters", std::string{value}));
        }
        return true;
    };

    if (type == "double") return numeric(false);
    if (type == "int") return numeric(true);
    if (type == "bool") {
        if (value == "true" || value == "false") return true;
        return fail(bad("parameter '" + name + "' must be true or false", std::string{value}));
    }
    if (type == "string") return true;
    return fail(bad("parameter '" + name + "' has an unknown type", type));
}

Result<bool> validate_parameters(const StrategyDescriptor& descriptor,
                                 const ParameterMap& parameters) {
    // Every supplied parameter must be declared. An undeclared parameter is
    // almost always a typo, and silently ignoring it means the experiment runs
    // with a default nobody intended.
    for (const auto& [key, value] : parameters) {
        const auto it =
            std::find_if(descriptor.parameters.begin(), descriptor.parameters.end(),
                         [&key](const ParameterSpec& spec) { return spec.name == key; });
        if (it == descriptor.parameters.end()) {
            return fail(bad("parameter '" + key + "' is not declared by " + descriptor.id.value()));
        }
        if (auto ok = it->validate_value(value); !ok) return ok;
    }

    // Every required parameter must be supplied or have a default.
    for (const auto& spec : descriptor.parameters) {
        if (!spec.required) continue;
        if (parameters.contains(spec.name)) continue;
        if (!spec.default_value.empty()) continue;
        return fail(bad("required parameter '" + spec.name + "' was not supplied"));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Metadata and descriptor
// ---------------------------------------------------------------------------

std::string StrategyMetadata::describe() const {
    std::ostringstream ss;
    ss << description;
    if (!author.empty()) ss << " (by " << author << ')';
    if (!tags.empty()) {
        ss << " [";
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (i != 0) ss << ", ";
            ss << tags[i];
        }
        ss << ']';
    }
    return ss.str();
}

std::uint64_t StrategyDescriptor::fingerprint() const {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_string(h, id.value());
    hash_bytes(h, &version.major, sizeof(version.major));
    hash_bytes(h, &version.minor, sizeof(version.minor));
    hash_bytes(h, &version.patch, sizeof(version.patch));

    // The PARAMETER SCHEMA is part of the fingerprint. A change to any name or
    // type makes results incomparable with the previous shape, and folding the
    // schema in is what stops an experiment being ranked against a predecessor
    // that took different inputs.
    for (const auto& spec : parameters) {
        hash_string(h, spec.name);
        hash_string(h, spec.type);
        const auto required = static_cast<std::uint8_t>(spec.required ? 1 : 0);
        hash_bytes(h, &required, sizeof(required));
        hash_string(h, spec.default_value);
    }
    for (const auto& dependency : depends_on) hash_string(h, dependency.value());
    // Deliberately EXCLUDED: state, author, description, tags, created_at.
    // Promoting a strategy from research to production does not change what it
    // computes, and a fingerprint that moved on promotion would orphan every
    // result recorded before it.
    return h;
}

Result<bool> StrategyDescriptor::validate() const {
    if (id.empty()) return fail(bad("descriptor has no strategy id"));

    std::vector<std::string> seen;
    for (const auto& spec : parameters) {
        if (spec.name.empty()) return fail(bad("parameter has no name"));
        if (std::find(seen.begin(), seen.end(), spec.name) != seen.end()) {
            return fail(bad("duplicate parameter '" + spec.name + "'"));
        }
        seen.push_back(spec.name);
        if (spec.type != "double" && spec.type != "int" && spec.type != "bool" &&
            spec.type != "string") {
            return fail(bad("parameter '" + spec.name + "' has an unsupported type", spec.type));
        }
        if (!spec.default_value.empty()) {
            if (auto ok = spec.validate_value(spec.default_value); !ok) {
                return fail(bad("default for '" + spec.name + "' fails its own schema",
                                spec.default_value));
            }
        }
    }

    for (const auto& dependency : depends_on) {
        if (dependency == id) {
            return fail(bad("strategy depends on itself", id.value()));
        }
    }
    return true;
}

std::string StrategyDescriptor::describe() const {
    std::ostringstream ss;
    ss << id.value() << '@' << version.to_string() << " [" << to_string(state) << "] "
       << metadata.describe();
    return ss.str();
}

std::string StrategyDescriptor::to_json() const {
    std::ostringstream ss;
    ss << "{\"id\": \"" << json_escape(id.value()) << "\", \"version\": \"" << version.to_string()
       << "\", \"state\": \"" << to_string(state) << "\", \"fingerprint\": \"" << std::hex
       << std::setw(16) << std::setfill('0') << fingerprint() << std::dec << "\", \"author\": \""
       << json_escape(metadata.author) << "\", \"description\": \""
       << json_escape(metadata.description) << "\", \"tags\": [";
    for (std::size_t i = 0; i < metadata.tags.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << '"' << json_escape(metadata.tags[i]) << '"';
    }
    ss << "], \"parameters\": [";
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << "{\"name\": \"" << json_escape(parameters[i].name) << "\", \"type\": \""
           << json_escape(parameters[i].type)
           << "\", \"required\": " << (parameters[i].required ? "true" : "false") << '}';
    }
    ss << "], \"depends_on\": [";
    for (std::size_t i = 0; i < depends_on.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << '"' << json_escape(depends_on[i].value()) << '"';
    }
    ss << "]}";
    return ss.str();
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

Result<bool> StrategyRegistry::register_strategy(StrategyDescriptor descriptor) {
    if (auto ok = descriptor.validate(); !ok) return ok;

    const Key key{descriptor.id.value(), descriptor.version};
    if (descriptors_.contains(key)) {
        // Two definitions of one version means a lookup would have to choose,
        // and choosing silently is how an experiment runs code nobody intended.
        return fail(bad("strategy already registered at this version",
                        descriptor.id.value() + "@" + descriptor.version.to_string()));
    }
    descriptors_.emplace(key, std::move(descriptor));
    return true;
}

const StrategyDescriptor* StrategyRegistry::find(const StrategyId& id,
                                                 const StrategyVersion& version) const noexcept {
    const auto it = descriptors_.find(Key{id.value(), version});
    return it == descriptors_.end() ? nullptr : &it->second;
}

const StrategyDescriptor* StrategyRegistry::latest(const StrategyId& id) const noexcept {
    const StrategyDescriptor* best = nullptr;
    for (const auto& [key, descriptor] : descriptors_) {
        if (key.first != id.value()) continue;
        // Ordered comparison, not a string sort: under a string sort 1.10.0
        // precedes 1.9.0 and a rollback picks the wrong artifact.
        if (best == nullptr || best->version < descriptor.version) best = &descriptor;
    }
    return best;
}

const StrategyDescriptor* StrategyRegistry::latest_compatible(
    const StrategyId& id, const StrategyVersion& version) const noexcept {
    const StrategyDescriptor* best = nullptr;
    for (const auto& [key, descriptor] : descriptors_) {
        if (key.first != id.value()) continue;
        if (!descriptor.version.compatible_with(version)) continue;
        if (best == nullptr || best->version < descriptor.version) best = &descriptor;
    }
    return best;
}

std::vector<StrategyId> StrategyRegistry::ids() const {
    std::vector<StrategyId> out;
    for (const auto& [key, descriptor] : descriptors_) {
        if (out.empty() || out.back() != descriptor.id) out.push_back(descriptor.id);
    }
    return out;
}

std::vector<StrategyVersion> StrategyRegistry::versions_of(const StrategyId& id) const {
    std::vector<StrategyVersion> out;
    for (const auto& [key, descriptor] : descriptors_) {
        if (key.first == id.value()) out.push_back(descriptor.version);
    }
    return out;
}

std::vector<const StrategyDescriptor*> StrategyRegistry::with_state(StrategyState state) const {
    std::vector<const StrategyDescriptor*> out;
    for (const auto& [key, descriptor] : descriptors_) {
        if (descriptor.state == state) out.push_back(&descriptor);
    }
    return out;
}

std::vector<const StrategyDescriptor*> StrategyRegistry::with_tag(std::string_view tag) const {
    std::vector<const StrategyDescriptor*> out;
    for (const auto& [key, descriptor] : descriptors_) {
        const auto& tags = descriptor.metadata.tags;
        if (std::find(tags.begin(), tags.end(), tag) != tags.end()) {
            out.push_back(&descriptor);
        }
    }
    return out;
}

Result<bool> StrategyRegistry::transition(const StrategyId& id, const StrategyVersion& version,
                                          StrategyState target) {
    const auto it = descriptors_.find(Key{id.value(), version});
    if (it == descriptors_.end()) {
        return fail(bad("no such strategy", id.value() + "@" + version.to_string()));
    }

    const StrategyState current = it->second.state;
    if (current == target) return true;

    // A retired strategy cannot be revived by a state change. Re-registration
    // under a new version is required, which leaves a trace; a silent revival
    // would let retired code reach production with no record of the decision.
    if (current == StrategyState::Retired) {
        return fail(
            bad("a retired strategy cannot be transitioned; register a new "
                "version instead",
                id.value() + "@" + version.to_string()));
    }
    // Draft cannot jump straight to production without passing research.
    if (current == StrategyState::Draft && target == StrategyState::Production) {
        return fail(
            bad("a draft strategy cannot go straight to production; it must "
                "pass through research and candidate first",
                id.value()));
    }
    it->second.state = target;
    return true;
}

Result<std::vector<StrategyId>> StrategyRegistry::resolve_dependencies(
    const StrategyId& id, const StrategyVersion& version) const {
    const auto* root = find(id, version);
    if (root == nullptr) {
        return fail(bad("no such strategy", id.value() + "@" + version.to_string()));
    }

    // Iterative depth-first with an explicit stack and a visiting set. A cycle
    // would make a recursive walk run until the stack ran out, which is a much
    // worse diagnostic than naming the loop.
    std::vector<StrategyId> ordered;
    std::map<std::string, int> mark;  // 0 unseen, 1 visiting, 2 done

    struct Frame {
        const StrategyDescriptor* descriptor;
        std::size_t next = 0;
    };
    std::vector<Frame> stack{Frame{root}};
    mark[root->id.value()] = 1;

    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.next < frame.descriptor->depends_on.size()) {
            const StrategyId& dependency = frame.descriptor->depends_on[frame.next++];
            const int state = mark[dependency.value()];
            if (state == 1) {
                return fail(bad("dependency cycle detected", dependency.value()));
            }
            if (state == 2) continue;

            const auto* next = latest(dependency);
            if (next == nullptr) {
                return fail(bad("unresolved dependency", dependency.value()));
            }
            mark[dependency.value()] = 1;
            stack.push_back(Frame{next});
            continue;
        }
        mark[frame.descriptor->id.value()] = 2;
        // Post-order: dependencies precede the strategy that needs them.
        ordered.push_back(frame.descriptor->id);
        stack.pop_back();
    }
    return ordered;
}

std::string StrategyRegistry::to_json() const {
    std::ostringstream ss;
    ss << "{\"strategies\": [";
    bool first = true;
    // std::map iteration: name then version, so the manifest is byte-identical
    // between runs and can be diffed.
    for (const auto& [key, descriptor] : descriptors_) {
        if (!first) ss << ", ";
        first = false;
        ss << descriptor.to_json();
    }
    ss << "]}";
    return ss.str();
}

void StrategyRegistry::clear() noexcept {
    descriptors_.clear();
}

}  // namespace ptl::strategy
