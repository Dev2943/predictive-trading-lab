#pragma once

/// \file credentials.hpp
/// Credential loading, with secrets kept out of the repository and out of logs.
///
/// Three rules:
///
/// 1. NEVER FROM THE CONFIG FILE. config/*.toml is committed. A key id in a
///    committed file is a leaked key id, and the loader therefore has no path
///    that reads one -- the temptation is removed rather than documented.
///
/// 2. NEVER LOGGED OR PRINTED. Secret is a distinct type whose value can only be
///    reached through reveal(), which reads as an assertion at the call site.
///    Its formatter is redacted, so a secret that reaches a log line by accident
///    prints as "<redacted:32>" instead of the key.
///
/// 3. INJECTED, NOT LOOKED UP. The source is an interface. Tests use an
///    in-memory implementation; nothing needs a real key to be tested.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ptl/core/result.hpp"

namespace ptl::auth {

/// A value that must not be printed.
class Secret {
public:
    Secret() = default;
    explicit Secret(std::string value) : value_(std::move(value)) {}

    /// The only way to the plaintext. Named so that every use is greppable and
    /// reads as a deliberate act at the call site.
    [[nodiscard]] const std::string& reveal() const noexcept { return value_; }

    [[nodiscard]] bool empty() const noexcept { return value_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return value_.size(); }

    /// Length only. Enough to tell "unset" from "wrong", which is what a
    /// diagnostic actually needs.
    [[nodiscard]] std::string redacted() const;

    /// Best-effort scrub on destruction. Not a security guarantee -- the string
    /// may have been reallocated -- but it narrows the window and costs nothing.
    ~Secret();
    Secret(const Secret&) = default;
    Secret& operator=(const Secret&) = default;
    Secret(Secret&&) = default;
    Secret& operator=(Secret&&) = default;

private:
    std::string value_;
};

/// Where credentials come from. An interface so tests never touch a real key.
class ICredentialSource {
public:
    ICredentialSource() = default;
    virtual ~ICredentialSource() = default;
    // Copy is deleted to prevent slicing. Move is PROTECTED and defaulted, not
    // deleted: deleting it would make every concrete source non-movable too,
    // and FileCredentials::load must be able to return one by value.
    ICredentialSource(const ICredentialSource&) = delete;
    ICredentialSource& operator=(const ICredentialSource&) = delete;

protected:
    ICredentialSource(ICredentialSource&&) = default;
    ICredentialSource& operator=(ICredentialSource&&) = default;

public:
    [[nodiscard]] virtual std::optional<Secret> lookup(std::string_view key) const = 0;
    [[nodiscard]] virtual std::string_view description() const noexcept = 0;
};

/// Process environment. The production source.
class EnvironmentCredentials final : public ICredentialSource {
public:
    [[nodiscard]] std::optional<Secret> lookup(std::string_view key) const override;
    [[nodiscard]] std::string_view description() const noexcept override {
        return "process environment";
    }
};

/// An untracked dotenv-style file. Convenient for local development; the loader
/// refuses a world-readable file, because a credential file anyone on the
/// machine can read is not a credential file.
class FileCredentials final : public ICredentialSource {
public:
    FileCredentials(FileCredentials&&) = default;
    FileCredentials& operator=(FileCredentials&&) = default;
    ~FileCredentials() override = default;

    [[nodiscard]] static Result<FileCredentials> load(const std::string& path);

    [[nodiscard]] std::optional<Secret> lookup(std::string_view key) const override;
    [[nodiscard]] std::string_view description() const noexcept override { return description_; }

private:
    FileCredentials() = default;
    std::map<std::string, Secret, std::less<>> values_;
    std::string description_;
};

/// Deterministic, in-memory. For tests.
class StaticCredentials final : public ICredentialSource {
public:
    explicit StaticCredentials(std::map<std::string, std::string, std::less<>> values);

    [[nodiscard]] std::optional<Secret> lookup(std::string_view key) const override;
    [[nodiscard]] std::string_view description() const noexcept override { return "static"; }

private:
    std::map<std::string, Secret, std::less<>> values_;
};

/// Tries each source in order and reports which one answered. Order is
/// explicit, so "which credential did this run use?" has an answer.
class ChainedCredentials final : public ICredentialSource {
public:
    void add(std::shared_ptr<ICredentialSource> source);

    [[nodiscard]] std::optional<Secret> lookup(std::string_view key) const override;
    [[nodiscard]] std::string_view description() const noexcept override { return "chained"; }

    /// Which source supplied `key`, for the run manifest. Never the value.
    [[nodiscard]] std::string_view source_of(std::string_view key) const;

private:
    std::vector<std::shared_ptr<ICredentialSource>> sources_;
};

/// A resolved provider credential.
struct ApiCredential {
    std::string key_id;  // not secret; recorded in the manifest for provenance
    Secret secret;
    std::string source;  // which ICredentialSource answered
};

/// Resolve one credential, failing with a message that names the environment
/// variables to set and never echoes a value.
[[nodiscard]] Result<ApiCredential> resolve(const ICredentialSource& source,
                                            std::string_view key_id_name,
                                            std::string_view secret_name);

}  // namespace ptl::auth
