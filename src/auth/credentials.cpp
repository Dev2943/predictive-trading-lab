#include "ptl/auth/credentials.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>

namespace ptl::auth {
namespace {

[[nodiscard]] std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return std::string{s.substr(b, e - b + 1)};
}

/// Strip a single layer of matching quotes, as dotenv files commonly use.
[[nodiscard]] std::string unquote(std::string s) {
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

}  // namespace

Secret::~Secret() {
    // Overwrite before release. Not a guarantee -- the buffer may have been
    // reallocated during earlier assignment -- but it narrows the window during
    // which a core dump would contain the plaintext, and it costs nothing.
    std::fill(value_.begin(), value_.end(), '\0');
}

std::string Secret::redacted() const {
    if (value_.empty()) return "<unset>";
    return "<redacted:" + std::to_string(value_.size()) + ">";
}

std::optional<Secret> EnvironmentCredentials::lookup(std::string_view key) const {
    // getenv needs a NUL-terminated string; string_view is not one.
    const std::string name{key};
    const char* v = std::getenv(name.c_str());  // NOLINT(concurrency-mt-unsafe)
    if (v == nullptr) return std::nullopt;
    const std::string value{v};
    if (value.empty()) return std::nullopt;
    return Secret{value};
}

Result<FileCredentials> FileCredentials::load(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return fail(make_error(ErrorCode::NotFound, "credential file not found", path));
    }
    // A credential file that group or world can read is not a credential file.
    // Refusing is more useful than warning: a warning in a build log is a
    // warning nobody reads.
    if ((st.st_mode & (S_IRGRP | S_IROTH | S_IWGRP | S_IWOTH)) != 0) {
        return fail(make_error(ErrorCode::ValidationFailed,
                               "credential file is group- or world-accessible; "
                               "run: chmod 600 " +
                                   path,
                               path));
    }

    std::ifstream in{path};
    if (!in) return fail(make_error(ErrorCode::IoError, "cannot open credential file", path));

    FileCredentials fc;
    fc.description_ = "file:" + path;
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        const std::string t = trim(line);
        if (t.empty() || t.front() == '#') continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) {
            return fail(make_error(ErrorCode::ParseError,
                                   "expected KEY=VALUE on line " + std::to_string(line_no), path));
        }
        std::string key = trim(std::string_view{t}.substr(0, eq));
        // Support an "export KEY=VALUE" prefix so the same file can be sourced.
        if (key.rfind("export ", 0) == 0) key = trim(std::string_view{key}.substr(7));
        if (key.empty()) {
            return fail(make_error(ErrorCode::ParseError,
                                   "empty key on line " + std::to_string(line_no), path));
        }
        fc.values_.emplace(std::move(key),
                           Secret{unquote(trim(std::string_view{t}.substr(eq + 1)))});
    }
    return fc;
}

std::optional<Secret> FileCredentials::lookup(std::string_view key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    if (it->second.empty()) return std::nullopt;
    return it->second;
}

StaticCredentials::StaticCredentials(std::map<std::string, std::string, std::less<>> values) {
    for (auto& [k, v] : values) values_.emplace(k, Secret{std::move(v)});
}

std::optional<Secret> StaticCredentials::lookup(std::string_view key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

void ChainedCredentials::add(std::shared_ptr<ICredentialSource> source) {
    if (source != nullptr) sources_.push_back(std::move(source));
}

std::optional<Secret> ChainedCredentials::lookup(std::string_view key) const {
    for (const auto& s : sources_) {
        if (auto v = s->lookup(key)) return v;
    }
    return std::nullopt;
}

std::string_view ChainedCredentials::source_of(std::string_view key) const {
    for (const auto& s : sources_) {
        if (s->lookup(key).has_value()) return s->description();
    }
    return "(none)";
}

Result<ApiCredential> resolve(const ICredentialSource& source, std::string_view key_id_name,
                              std::string_view secret_name) {
    const auto key_id = source.lookup(key_id_name);
    const auto secret = source.lookup(secret_name);

    if (!key_id.has_value() || !secret.has_value()) {
        // Name the variables that are missing, never echo a value. A message
        // that says "authentication failed" costs the reader twenty minutes.
        std::string msg =
            "missing credentials. Set these in the environment or an "
            "untracked credential file: ";
        if (!key_id.has_value()) {
            msg += std::string{key_id_name};
            if (!secret.has_value()) msg += " and ";
        }
        if (!secret.has_value()) msg += std::string{secret_name};
        msg += ". Never place these in config/*.toml -- that directory is committed.";
        return fail(
            make_error(ErrorCode::NotFound, std::move(msg), std::string{source.description()}));
    }

    ApiCredential c;
    c.key_id = key_id->reveal();  // an id, not a secret
    c.secret = *secret;
    c.source = source.description();
    return c;
}

}  // namespace ptl::auth
