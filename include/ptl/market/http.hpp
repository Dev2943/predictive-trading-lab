#pragma once

/// \file http.hpp
/// The network seam.
///
/// The Alpaca adapter is a parser and a normaliser, not a network client. It
/// talks to this interface, so its entire behaviour -- URL construction,
/// pagination, timestamp convention, error mapping -- is testable against
/// recorded fixtures without credentials, without a socket, and
/// deterministically.
///
/// The production implementation is deliberately not here. It requires an HTTP
/// dependency, which ADR-0001 defers until the entitlement gate has actually
/// been run against a real account. Until then the adapter is complete and
/// tested; only the transport is pending, and that is a twenty-line class.

#include <map>
#include <string>
#include <vector>

#include "ptl/core/result.hpp"

namespace ptl::market {

struct HttpRequest {
    std::string url;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> query;

    /// URL with the query string appended, keys in sorted order.
    ///
    /// Sorted so the same logical request always produces the same string --
    /// which is what lets a fixture be keyed by URL and lets a cache be keyed
    /// deterministically.
    [[nodiscard]] std::string full_url() const;
};

struct HttpResponse {
    int status = 0;
    std::string body;

    [[nodiscard]] bool ok() const noexcept { return status >= 200 && status < 300; }
};

class IHttpTransport {
public:
    IHttpTransport() = default;
    virtual ~IHttpTransport() = default;
    IHttpTransport(const IHttpTransport&) = delete;
    IHttpTransport& operator=(const IHttpTransport&) = delete;

protected:
    IHttpTransport(IHttpTransport&&) = default;
    IHttpTransport& operator=(IHttpTransport&&) = default;

public:
    [[nodiscard]] virtual Result<HttpResponse> get(const HttpRequest&) = 0;
    [[nodiscard]] virtual std::string_view description() const noexcept = 0;
};

/// Replays recorded responses keyed by full URL. Every adapter test uses this.
class RecordedTransport final : public IHttpTransport {
public:
    void record(std::string url, HttpResponse response);

    [[nodiscard]] Result<HttpResponse> get(const HttpRequest&) override;
    [[nodiscard]] std::string_view description() const noexcept override { return "recorded"; }

    /// Every URL requested, in order. Lets a test assert HOW the adapter
    /// paginated, not merely what it returned.
    [[nodiscard]] const std::vector<std::string>& requested() const noexcept { return requested_; }

private:
    std::map<std::string, HttpResponse> responses_;
    std::vector<std::string> requested_;
};

}  // namespace ptl::market
