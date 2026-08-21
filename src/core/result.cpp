#include "ptl/core/result.hpp"

namespace ptl {

std::string_view to_string(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::Ok:               return "ok";
        case ErrorCode::NotFound:         return "not_found";
        case ErrorCode::ParseError:       return "parse_error";
        case ErrorCode::InvalidArgument:  return "invalid_argument";
        case ErrorCode::IoError:          return "io_error";
        case ErrorCode::ValidationFailed: return "validation_failed";
        case ErrorCode::ConfigError:      return "config_error";
        case ErrorCode::Unsupported:      return "unsupported";
    }
    return "unknown";
}

std::string Error::describe() const {
    std::string out{to_string(code)};
    out += ": ";
    out += message;
    if (!context.empty()) {
        out += " [";
        out += context;
        out += ']';
    }
    return out;
}

}  // namespace ptl
