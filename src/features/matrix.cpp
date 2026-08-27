#include "ptl/features/matrix.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace ptl::features {
namespace {

constexpr std::uint32_t kMagic = 0x50544C46;  // "PTLF"
constexpr std::uint32_t kFormatVersion = 1;

[[nodiscard]] Error bad(std::string message, std::string context = {}) {
    return make_error(ErrorCode::ValidationFailed, std::move(message), std::move(context));
}

template <class T>
void write_pod(std::ostream& os, const T& value) {
    os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <class T>
[[nodiscard]] bool read_pod(std::istream& is, T& value) {
    is.read(reinterpret_cast<char*>(&value), sizeof(T));
    return is.gcount() == static_cast<std::streamsize>(sizeof(T));
}

void hash_bytes(std::uint64_t& h, const void* data, std::size_t len) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= 0x100000001b3ULL;
    }
}

}  // namespace

FeatureMatrix::FeatureMatrix(std::vector<std::string> names, std::uint64_t data_version,
                             std::uint64_t feature_set_id)
    : names_(std::move(names)),
      columns_(names_.size()),
      data_version_(data_version),
      feature_set_id_(feature_set_id) {}

Result<bool> FeatureMatrix::append(const FeatureRow& row) {
    if (row.values.size() != names_.size()) {
        // A truncated row would shift every later column by one and produce a
        // matrix whose names no longer describe its values.
        return fail(bad("feature row width " + std::to_string(row.values.size()) +
                        " does not match matrix width " + std::to_string(names_.size())));
    }
    if (row.feature_set_id != feature_set_id_) {
        return fail(bad("feature row was produced by a different feature-set definition"));
    }
    if (row.data_version != data_version_) {
        return fail(bad("feature row derives from a different dataset version"));
    }
    if (!is_set(row.feature_end_time)) {
        return fail(bad("feature row has no feature_end_time"));
    }
    if (!keys_.empty() && row.feature_end_time < keys_.back().feature_end_time) {
        // Out-of-order rows would corrupt every time-ordered fold built from
        // this matrix.
        return fail(bad("feature rows must be appended in non-decreasing time order"));
    }

    keys_.push_back(RowKey{row.feature_end_time, row.instrument, row.ready_mask});
    for (std::size_t j = 0; j < names_.size(); ++j) {
        columns_[j].push_back(row.values[j]);
    }
    return true;
}

std::span<const double> FeatureMatrix::column(std::size_t j) const noexcept {
    if (j >= columns_.size()) return {};
    return columns_[j];
}

double FeatureMatrix::at(std::size_t row, std::size_t col) const noexcept {
    if (col >= columns_.size() || row >= columns_[col].size()) return 0.0;
    return columns_[col][row];
}

std::vector<std::size_t> FeatureMatrix::ready_rows(std::uint64_t required) const {
    std::vector<std::size_t> out;
    out.reserve(keys_.size());
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if ((keys_[i].ready_mask & required) == required) out.push_back(i);
    }
    return out;
}

std::size_t FeatureMatrix::index_of_name(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < names_.size(); ++i) {
        if (names_[i] == name) return i;
    }
    return static_cast<std::size_t>(-1);
}

void FeatureMatrix::reserve(std::size_t rows) {
    keys_.reserve(rows);
    for (auto& c : columns_) c.reserve(rows);
}

std::uint64_t FeatureMatrix::content_hash() const {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_bytes(h, &data_version_, sizeof(data_version_));
    hash_bytes(h, &feature_set_id_, sizeof(feature_set_id_));
    for (const auto& n : names_) hash_bytes(h, n.data(), n.size());
    for (const auto& k : keys_) {
        const std::int64_t ns = k.feature_end_time.time_since_epoch().count();
        hash_bytes(h, &ns, sizeof(ns));
        const std::uint32_t id = index_of(k.instrument);
        hash_bytes(h, &id, sizeof(id));
        hash_bytes(h, &k.ready_mask, sizeof(k.ready_mask));
    }
    for (const auto& col : columns_) {
        // Hash the BIT PATTERN, not a rounded decimal. Two matrices whose
        // values differ in the last ulp must hash differently, or the
        // determinism test would pass over a real divergence.
        for (const double v : col) hash_bytes(h, &v, sizeof(v));
    }
    return h;
}

Result<bool> FeatureMatrix::save(const std::filesystem::path& path) const {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream os{path, std::ios::binary | std::ios::trunc};
    if (!os)
        return fail(make_error(ErrorCode::IoError, "cannot open matrix for write", path.string()));

    write_pod(os, kMagic);
    write_pod(os, kFormatVersion);
    write_pod(os, data_version_);
    write_pod(os, feature_set_id_);
    const auto hash = content_hash();
    write_pod(os, hash);

    // Declared as fixed-width rather than cast: the file format must be
    // identical on every platform, but on a 64-bit host size_t already IS
    // uint64_t and an explicit cast is flagged as useless. Declaring the type
    // gets portability without the redundant cast.
    const std::uint64_t ncols = names_.size();
    const std::uint64_t nrows = keys_.size();
    write_pod(os, ncols);
    write_pod(os, nrows);

    for (const auto& n : names_) {
        const auto len = static_cast<std::uint32_t>(n.size());
        write_pod(os, len);
        os.write(n.data(), static_cast<std::streamsize>(len));
    }
    for (const auto& k : keys_) {
        const std::int64_t ns = k.feature_end_time.time_since_epoch().count();
        write_pod(os, ns);
        write_pod(os, index_of(k.instrument));
        write_pod(os, k.ready_mask);
    }
    for (const auto& col : columns_) {
        os.write(reinterpret_cast<const char*>(col.data()),
                 static_cast<std::streamsize>(col.size() * sizeof(double)));
    }
    if (!os) return fail(make_error(ErrorCode::IoError, "write failed", path.string()));
    return true;
}

Result<FeatureMatrix> FeatureMatrix::load(const std::filesystem::path& path) {
    std::ifstream is{path, std::ios::binary};
    if (!is) return fail(make_error(ErrorCode::NotFound, "cannot open matrix", path.string()));

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    if (!read_pod(is, magic) || magic != kMagic) {
        return fail(bad("not a feature matrix file", path.string()));
    }
    if (!read_pod(is, version) || version != kFormatVersion) {
        // Refuse rather than guess. A format change means the layout below is
        // wrong, and reading it anyway produces plausible nonsense.
        return fail(bad("unsupported feature matrix format version " + std::to_string(version),
                        path.string()));
    }

    FeatureMatrix m;
    std::uint64_t stored_hash = 0;
    std::uint64_t ncols = 0;
    std::uint64_t nrows = 0;
    if (!read_pod(is, m.data_version_) || !read_pod(is, m.feature_set_id_) ||
        !read_pod(is, stored_hash) || !read_pod(is, ncols) || !read_pod(is, nrows)) {
        return fail(bad("truncated feature matrix header", path.string()));
    }

    m.names_.resize(ncols);
    for (auto& n : m.names_) {
        std::uint32_t len = 0;
        if (!read_pod(is, len)) return fail(bad("truncated name table", path.string()));
        n.resize(len);
        is.read(n.data(), static_cast<std::streamsize>(len));
        if (is.gcount() != static_cast<std::streamsize>(len)) {
            return fail(bad("truncated name table", path.string()));
        }
    }

    m.keys_.resize(nrows);
    for (auto& k : m.keys_) {
        std::int64_t ns = 0;
        std::uint32_t inst = 0;
        std::uint64_t mask = 0;
        if (!read_pod(is, ns) || !read_pod(is, inst) || !read_pod(is, mask)) {
            return fail(bad("truncated row keys", path.string()));
        }
        k.feature_end_time = Timestamp{Duration{ns}};
        k.instrument = static_cast<InstrumentId>(inst);
        k.ready_mask = mask;
    }

    m.columns_.resize(ncols);
    for (auto& col : m.columns_) {
        col.resize(nrows);
        is.read(reinterpret_cast<char*>(col.data()),
                static_cast<std::streamsize>(nrows * sizeof(double)));
        if (is.gcount() != static_cast<std::streamsize>(nrows * sizeof(double))) {
            return fail(bad("truncated value block", path.string()));
        }
    }

    if (m.content_hash() != stored_hash) {
        // A silent corruption would train a model on values nobody produced.
        return fail(
            bad("feature matrix content hash mismatch: the file is corrupt or was "
                "written by a different build",
                path.string()));
    }
    return m;
}

std::filesystem::path FeatureMatrix::cache_path(const std::filesystem::path& dir,
                                                std::uint64_t data_version,
                                                std::uint64_t feature_set_id) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "features_%016llx_%016llx.fmx",
                  static_cast<unsigned long long>(data_version),
                  static_cast<unsigned long long>(feature_set_id));
    return dir / buf;
}

}  // namespace ptl::features
