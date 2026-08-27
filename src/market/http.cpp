#include "ptl/market/http.hpp"

namespace ptl::market {

std::string HttpRequest::full_url() const {
    if (query.empty()) return url;
    std::string out = url;
    out += '?';
    bool first = true;
    // std::map iterates in key order, so the same logical request always
    // produces the same string. That determinism is what lets a fixture be
    // keyed by URL and a response cache be keyed reproducibly.
    for (const auto& [k, v] : query) {
        if (!first) out += '&';
        first = false;
        out += k;
        out += '=';
        out += v;
    }
    return out;
}

void RecordedTransport::record(std::string url, HttpResponse response) {
    responses_.insert_or_assign(std::move(url), std::move(response));
}

Result<HttpResponse> RecordedTransport::get(const HttpRequest& request) {
    const std::string url = request.full_url();
    requested_.push_back(url);
    const auto it = responses_.find(url);
    if (it == responses_.end()) {
        // Loud, and it names the URL. A test that silently received an empty
        // response would be asserting on the adapter's error handling while
        // believing it was asserting on its parsing.
        return fail(make_error(ErrorCode::NotFound, "no recorded response for URL", url));
    }
    return it->second;
}

}  // namespace ptl::market
