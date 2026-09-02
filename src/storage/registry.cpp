#include "ptl/storage/registry.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace ptl::storage {
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
                out += c;
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Checksum
// ---------------------------------------------------------------------------

Checksum Checksum::of(std::string_view content) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_string(h, content);
    // Zero is the sentinel for "empty", so a genuine hash of zero is nudged.
    return Checksum{h == 0 ? 1 : h};
}

Result<Checksum> Checksum::parse(std::string_view hex) {
    if (hex.empty() || hex.size() > 16) {
        return fail(bad("checksum must be 1 to 16 hex digits", std::string{hex}));
    }
    std::uint64_t value = 0;
    const auto* first = hex.data();
    const auto* last = hex.data() + hex.size();
    const auto result = std::from_chars(first, last, value, 16);
    if (result.ec != std::errc{} || result.ptr != last) {
        return fail(bad("checksum is not valid hex", std::string{hex}));
    }
    return Checksum{value};
}

std::string Checksum::to_string() const {
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << value_;
    return ss.str();
}

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

Checksum Schema::checksum() const {
    std::ostringstream ss;
    // Field ORDER is part of the identity: a dataset whose columns were
    // reordered produces different feature matrices, and a checksum that
    // ignored order would call the two identical.
    for (const auto& field : fields) {
        ss << field.name << ':' << field.type << ':' << field.lookback_periods << ';';
    }
    return Checksum::of(ss.str());
}

bool Schema::compatible_with(const Schema& other) const {
    // Compatible means "every field I need exists there with the same type".
    // Extra fields on the other side are fine; a missing one is not.
    for (const auto& field : fields) {
        const auto it = std::find_if(
            other.fields.begin(), other.fields.end(),
            [&field](const SchemaField& candidate) { return candidate.name == field.name; });
        if (it == other.fields.end()) return false;
        if (it->type != field.type) return false;
    }
    return true;
}

std::string Schema::to_json() const {
    std::ostringstream ss;
    ss << '[';
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i != 0) ss << ", ";
        ss << "{\"name\": \"" << json_escape(fields[i].name) << "\", \"type\": \""
           << json_escape(fields[i].type) << "\", \"lookback\": " << fields[i].lookback_periods
           << '}';
    }
    ss << ']';
    return ss.str();
}

// ---------------------------------------------------------------------------
// DatasetVersion
// ---------------------------------------------------------------------------

std::string DatasetVersion::key() const {
    return dataset_id + "@v" + std::to_string(version);
}

Checksum DatasetVersion::fingerprint() const {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_string(h, dataset_id);
    hash_bytes(h, &version, sizeof(version));
    const std::uint64_t content = content_checksum.value();
    hash_bytes(h, &content, sizeof(content));
    const std::uint64_t features = feature_schema.checksum().value();
    hash_bytes(h, &features, sizeof(features));
    const std::uint64_t labels = label_schema.checksum().value();
    hash_bytes(h, &labels, sizeof(labels));
    // Normalisation is part of the identity: a change invalidates every model
    // trained against the previous version even at identical raw data.
    hash_string(h, normalization_version);
    for (const auto boundary : split_boundaries) {
        const std::int64_t ns = boundary.time_since_epoch().count();
        hash_bytes(h, &ns, sizeof(ns));
    }
    return Checksum::of(std::to_string(h));
}

Result<bool> DatasetVersion::validate() const {
    if (dataset_id.empty()) return fail(bad("dataset has no id"));
    if (version == 0) {
        // Versions start at 1 so that zero can mean "unset" everywhere else.
        return fail(bad("dataset version must be at least 1", dataset_id));
    }
    if (content_checksum.empty()) {
        return fail(
            bad("dataset has no content checksum; it cannot be verified later", dataset_id));
    }
    if (feature_schema.fields.empty()) {
        return fail(bad("dataset has an empty feature schema", dataset_id));
    }
    if (is_set(range_begin) && is_set(range_end) && range_end <= range_begin) {
        return fail(bad("dataset range ends before it begins", dataset_id));
    }
    for (std::size_t i = 1; i < split_boundaries.size(); ++i) {
        if (split_boundaries[i] <= split_boundaries[i - 1]) {
            // Unordered folds would make a later evaluation unable to prove it
            // used the same splits.
            return fail(
                bad("walk-forward split boundaries must be strictly ascending", dataset_id));
        }
    }
    return true;
}

std::string DatasetVersion::to_json() const {
    std::ostringstream ss;
    ss << "{\"dataset_id\": \"" << json_escape(dataset_id) << "\", \"version\": " << version
       << ", \"checksum\": \"" << content_checksum.to_string() << "\", \"fingerprint\": \""
       << fingerprint().to_string() << "\", \"created_at\": \"" << to_iso8601(created_at)
       << "\", \"range_begin\": \"" << to_iso8601(range_begin) << "\", \"range_end\": \""
       << to_iso8601(range_end) << "\", \"rows\": " << row_count << ", \"normalization\": \""
       << json_escape(normalization_version) << "\", \"source\": \"" << json_escape(source)
       << "\", \"feature_schema\": " << feature_schema.to_json()
       << ", \"label_schema\": " << label_schema.to_json()
       << ", \"splits\": " << split_boundaries.size() << '}';
    return ss.str();
}

// ---------------------------------------------------------------------------
// DatasetRegistry
// ---------------------------------------------------------------------------

Result<bool> DatasetRegistry::register_dataset(DatasetVersion dataset) {
    if (auto ok = dataset.validate(); !ok) return ok;

    const auto key = std::make_pair(dataset.dataset_id, dataset.version);
    if (datasets_.contains(key)) {
        // Datasets are APPEND-ONLY. A rewritten version silently invalidates
        // every model trained on it while leaving the version number unchanged,
        // which is the worst possible combination.
        return fail(
            bad("dataset version already registered; datasets are append-only", dataset.key()));
    }

    if (const auto* previous = latest(dataset.dataset_id); previous != nullptr) {
        if (dataset.version <= previous->version) {
            return fail(
                bad("dataset version must advance; latest is v" + std::to_string(previous->version),
                    dataset.key()));
        }
    }
    datasets_.emplace(key, std::move(dataset));
    return true;
}

const DatasetVersion* DatasetRegistry::find(std::string_view dataset_id,
                                            std::uint32_t version) const noexcept {
    const auto it = datasets_.find(std::make_pair(std::string{dataset_id}, version));
    return it == datasets_.end() ? nullptr : &it->second;
}

const DatasetVersion* DatasetRegistry::latest(std::string_view dataset_id) const noexcept {
    const DatasetVersion* best = nullptr;
    for (const auto& [key, dataset] : datasets_) {
        if (key.first != dataset_id) continue;
        if (best == nullptr || dataset.version > best->version) best = &dataset;
    }
    return best;
}

bool DatasetRegistry::contains(std::string_view dataset_id, std::uint32_t version) const noexcept {
    return find(dataset_id, version) != nullptr;
}

std::vector<std::string> DatasetRegistry::dataset_ids() const {
    std::vector<std::string> out;
    for (const auto& [key, dataset] : datasets_) {
        if (out.empty() || out.back() != key.first) out.push_back(key.first);
    }
    return out;
}

std::string DatasetRegistry::to_json() const {
    std::ostringstream ss;
    ss << "{\"datasets\": [";
    bool first = true;
    for (const auto& [key, dataset] : datasets_) {
        if (!first) ss << ", ";
        first = false;
        ss << dataset.to_json();
    }
    ss << "]}";
    return ss.str();
}

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------

std::string_view to_string(ModelStatus s) noexcept {
    switch (s) {
        case ModelStatus::Training:
            return "training";
        case ModelStatus::Trained:
            return "trained";
        case ModelStatus::Challenger:
            return "challenger";
        case ModelStatus::Champion:
            return "champion";
        case ModelStatus::Archived:
            return "archived";
        case ModelStatus::Failed:
            return "failed";
    }
    return "unknown";
}

Duration TrainingRecord::duration() const noexcept {
    if (!is_set(started_at) || !is_set(finished_at)) return Duration::zero();
    return finished_at - started_at;
}

std::string ModelMetadata::key() const {
    return model_id + "@v" + std::to_string(version);
}

Checksum ModelMetadata::fingerprint() const {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    hash_string(h, model_id);
    hash_bytes(h, &version, sizeof(version));
    hash_string(h, kind);
    hash_string(h, dataset_id);
    hash_bytes(h, &dataset_version, sizeof(dataset_version));
    // Hyperparameters in key order (std::map), so the fingerprint does not
    // depend on the order the caller happened to insert them.
    for (const auto& [name, value] : hyperparameters) {
        hash_string(h, name);
        hash_string(h, value);
    }
    hash_bytes(h, &training.seed, sizeof(training.seed));
    return Checksum::of(std::to_string(h));
}

Result<bool> ModelMetadata::validate() const {
    if (model_id.empty()) return fail(bad("model has no id"));
    if (version == 0) return fail(bad("model version must be at least 1", model_id));
    if (kind.empty()) return fail(bad("model has no kind", model_id));
    if (dataset_id.empty() || dataset_version == 0) {
        // THE CENTRAL INVARIANT. A model whose training data cannot be
        // identified is not reproducible, and a backtest built on it cannot be
        // defended.
        return fail(
            bad("model does not name a dataset version; it would not be "
                "reproducible",
                model_id));
    }
    return true;
}

std::string ModelMetadata::to_json() const {
    std::ostringstream ss;
    ss.precision(10);
    ss << std::fixed;
    ss << "{\"model_id\": \"" << json_escape(model_id) << "\", \"version\": " << version
       << ", \"status\": \"" << to_string(status) << "\", \"kind\": \"" << json_escape(kind)
       << "\", \"dataset\": \"" << json_escape(dataset_id) << "@v" << dataset_version
       << "\", \"strategy_id\": \"" << json_escape(strategy_id) << "\", \"experiment_id\": \""
       << json_escape(experiment_id) << "\", \"fingerprint\": \"" << fingerprint().to_string()
       << "\", \"seed\": " << training.seed << ", \"training_rows\": " << training.training_rows
       << ", \"hyperparameters\": {";
    bool first = true;
    for (const auto& [name, value] : hyperparameters) {
        if (!first) ss << ", ";
        first = false;
        ss << '"' << json_escape(name) << "\": \"" << json_escape(value) << '"';
    }
    ss << "}, \"metrics\": {";
    first = true;
    for (const auto& [name, value] : training.metrics) {
        if (!first) ss << ", ";
        first = false;
        ss << '"' << json_escape(name)
           << "\": " << (is_finite(value) ? std::to_string(value) : "null");
    }
    ss << "}}";
    return ss.str();
}

Result<bool> ModelRegistry::register_model(ModelMetadata model) {
    if (auto ok = model.validate(); !ok) return ok;

    // THE REFUSAL THIS MODULE EXISTS FOR.
    if (!datasets_->contains(model.dataset_id, model.dataset_version)) {
        return fail(
            bad("refusing to register a model trained on an unregistered dataset "
                "version; register the dataset first so the model stays "
                "reproducible",
                model.dataset_id + "@v" + std::to_string(model.dataset_version)));
    }

    const auto key = std::make_pair(model.model_id, model.version);
    if (models_.contains(key)) {
        return fail(bad("model version already registered", model.key()));
    }
    models_.emplace(key, std::move(model));
    return true;
}

const ModelMetadata* ModelRegistry::find(std::string_view model_id,
                                         std::uint32_t version) const noexcept {
    const auto it = models_.find(std::make_pair(std::string{model_id}, version));
    return it == models_.end() ? nullptr : &it->second;
}

const ModelMetadata* ModelRegistry::champion(std::string_view model_id) const noexcept {
    // O(log n) through the index rather than a scan over every model. The
    // status on the record stays authoritative; this only accelerates finding
    // it. A benchmark over a thousand models showed the scan to be 130x slower
    // than the other registries -- unacceptable on a path a live system
    // consults per prediction.
    const auto it = champions_.find(model_id);
    if (it == champions_.end()) return nullptr;
    return find(model_id, it->second);
}

std::vector<const ModelMetadata*> ModelRegistry::challengers(std::string_view model_id) const {
    std::vector<const ModelMetadata*> out;
    for (const auto& [key, model] : models_) {
        if (key.first == model_id && model.status == ModelStatus::Challenger) {
            out.push_back(&model);
        }
    }
    return out;
}

std::vector<std::uint32_t> ModelRegistry::versions_of(std::string_view model_id) const {
    std::vector<std::uint32_t> out;
    for (const auto& [key, model] : models_) {
        if (key.first == model_id) out.push_back(model.version);
    }
    return out;
}

Result<bool> ModelRegistry::promote(std::string_view model_id, std::uint32_t version, Timestamp at,
                                    std::string reason) {
    const auto key = std::make_pair(std::string{model_id}, version);
    const auto it = models_.find(key);
    if (it == models_.end()) {
        return fail(bad("no such model", std::string{model_id} + "@v" + std::to_string(version)));
    }
    if (it->second.status == ModelStatus::Champion) return true;
    if (it->second.status == ModelStatus::Failed) {
        return fail(bad("a failed model cannot be promoted", it->second.key()));
    }

    ModelTransition transition;
    transition.model_id = std::string{model_id};
    transition.to_version = version;
    transition.to_status = ModelStatus::Champion;
    transition.at = at;
    transition.reason = std::move(reason);

    // The incumbent is ARCHIVED, not deleted. Rollback needs it, and a
    // promotion that destroyed its predecessor would be irreversible.
    for (auto& [existing_key, model] : models_) {
        if (existing_key.first != model_id) continue;
        if (model.status != ModelStatus::Champion) continue;
        transition.from_version = model.version;
        transition.from_status = ModelStatus::Champion;
        model.status = ModelStatus::Archived;
    }

    it->second.status = ModelStatus::Champion;
    // Index updated in the same step that changes the status, so the two can
    // never disagree.
    champions_.insert_or_assign(std::string{model_id}, version);
    history_.push_back(std::move(transition));
    return true;
}

Result<bool> ModelRegistry::rollback(std::string_view model_id, std::uint32_t version, Timestamp at,
                                     std::string reason) {
    const auto it = models_.find(std::make_pair(std::string{model_id}, version));
    if (it == models_.end()) {
        return fail(bad("no such model to roll back to",
                        std::string{model_id} + "@v" + std::to_string(version)));
    }
    if (it->second.status != ModelStatus::Archived && it->second.status != ModelStatus::Champion) {
        // Only a previously-live model can be rolled back to. Rolling back to
        // something that was never champion is a promotion, and calling it a
        // rollback would misrepresent the history.
        return fail(
            bad("only an archived former champion can be rolled back to; this "
                "model is " +
                    std::string{to_string(it->second.status)},
                it->second.key()));
    }
    return promote(model_id, version, at,
                   reason.empty() ? std::string{"rollback"} : "rollback: " + reason);
}

std::string ModelRegistry::to_json() const {
    std::ostringstream ss;
    ss << "{\"models\": [";
    bool first = true;
    for (const auto& [key, model] : models_) {
        if (!first) ss << ", ";
        first = false;
        ss << model.to_json();
    }
    ss << "], \"transitions\": " << history_.size() << '}';
    return ss.str();
}

// ---------------------------------------------------------------------------
// ArtifactStore
// ---------------------------------------------------------------------------

Result<std::string> ArtifactStore::path_for(std::string_view key) const {
    if (key.empty()) return fail(bad("artifact key cannot be empty"));
    // Path traversal is refused rather than sanitised: a key containing ".."
    // is a bug or an attack, and quietly rewriting it hides which.
    if (key.find("..") != std::string_view::npos) {
        return fail(bad("artifact key must not contain '..'", std::string{key}));
    }
    if (key.front() == '/') {
        return fail(bad("artifact key must be relative", std::string{key}));
    }
    return (std::filesystem::path{root_} / key).string() + ".json";
}

Result<std::string> ArtifactStore::put(std::string_view key, std::string_view json) {
    auto path = path_for(key);
    if (!path) return path;

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path{*path}.parent_path(), ec);

    // Write to a temporary and rename into place, so a crash mid-write leaves
    // no half-written artifact for a later read to trust.
    const std::string temp = *path + ".tmp";
    {
        std::ofstream out{temp, std::ios::binary | std::ios::trunc};
        if (!out) return fail(make_error(ErrorCode::IoError, "cannot open artifact", temp));
        out.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!out) return fail(make_error(ErrorCode::IoError, "artifact write failed", temp));
    }
    std::filesystem::rename(temp, *path, ec);
    if (ec) {
        return fail(
            make_error(ErrorCode::IoError, "cannot rename artifact into place", ec.message()));
    }
    return *path;
}

Result<std::string> ArtifactStore::get(std::string_view key) const {
    auto path = path_for(key);
    if (!path) return path;
    if (!std::filesystem::exists(*path)) {
        return fail(make_error(ErrorCode::NotFound, "no such artifact", std::string{key}));
    }

    std::ifstream in{*path, std::ios::binary | std::ios::ate};
    if (!in) return fail(make_error(ErrorCode::IoError, "cannot open artifact", *path));
    const auto size = in.tellg();
    if (size < 0) return fail(make_error(ErrorCode::IoError, "cannot size artifact", *path));

    std::string contents(static_cast<std::size_t>(size), '\0');
    in.seekg(0);
    in.read(contents.data(), size);
    if (in.gcount() != size) {
        return fail(make_error(ErrorCode::IoError, "artifact read was short", *path));
    }
    return contents;
}

bool ArtifactStore::contains(std::string_view key) const {
    auto path = path_for(key);
    return path.has_value() && std::filesystem::exists(*path);
}

Result<Checksum> ArtifactStore::checksum_of(std::string_view key) const {
    auto contents = get(key);
    if (!contents) return fail(contents.error());
    return Checksum::of(*contents);
}

Result<std::vector<std::string>> ArtifactStore::list(std::string_view prefix) const {
    std::vector<std::string> out;
    const std::filesystem::path base = prefix.empty()
                                           ? std::filesystem::path{root_}
                                           : std::filesystem::path{root_} / std::string{prefix};
    if (!std::filesystem::exists(base)) return out;

    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{base, ec}) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        auto relative = std::filesystem::relative(entry.path(), root_, ec).string();
        if (ec) continue;
        if (relative.size() > 5) relative.resize(relative.size() - 5);  // drop ".json"
        out.push_back(std::move(relative));
    }
    // Sorted, so a listing is reproducible: directory iteration order is
    // filesystem-defined and varies between machines.
    std::sort(out.begin(), out.end());
    return out;
}

Result<bool> ArtifactStore::remove(std::string_view key) {
    auto path = path_for(key);
    if (!path) return fail(path.error());
    std::error_code ec;
    std::filesystem::remove(*path, ec);
    return true;
}

}  // namespace ptl::storage
