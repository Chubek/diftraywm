#pragma once

// Layer 3 -- kernel contracts.
//
// A kernel is a named, in-process command implementation with an explicit
// lifecycle. Kernels are the extension seam for embedding shell behaviour
// without forking: a kernel observes an Invocation and writes through
// Writer/Sinklet, so its I/O participates in redirection and capture exactly
// like an external process's.

#include "IR.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lsh {
namespace kernel {

struct Version {
    int major {0};
    int minor {1};
    int patch {0};
    std::string label {"draft"};
};

struct Metadata {
    std::string name;
    std::vector<std::string> aliases;
    Version version;
    std::string summary;
    std::string documentation;
};

// Everything a kernel is given for one invocation. Pointers are borrowed for
// the duration of execute(); an empty writer means "no stream is attached".
struct Invocation {
    std::vector<std::string> argv;
    Environment* environment {nullptr};
    Reader* stdin_reader {nullptr};
    Writer* stdout_writer {nullptr};
    Writer* stderr_writer {nullptr};
    // Positional parameters ($1, $2, ...) established by the caller.
    std::vector<std::string> positional;
    // Absolute path of the working directory for this invocation.
    std::string cwd;
};

class Kernel {
public:
    virtual ~Kernel() = default;

    [[nodiscard]] virtual const Metadata& metadata() const noexcept = 0;

    virtual Result<void> load() { return {}; }
    virtual Result<void> initialize(Environment&) { return {}; }
    virtual Result<ExitStatus> execute(const Invocation& invocation) = 0;
    virtual Result<void> shutdown() { return {}; }
};

class FunctionKernel final : public Kernel {
public:
    using ExecuteFn = std::function<Result<ExitStatus>(const Invocation&)>;

    FunctionKernel(Metadata metadata, ExecuteFn execute)
        : metadata_(std::move(metadata)), execute_(std::move(execute)) {}

    [[nodiscard]] const Metadata& metadata() const noexcept override { return metadata_; }

    Result<ExitStatus> execute(const Invocation& invocation) override {
        if (!execute_) {
            return failure(ErrorCode::execution_failed, "function kernel has no execute handler", metadata_.name);
        }
        return execute_(invocation);
    }

private:
    Metadata metadata_;
    ExecuteFn execute_;
};

// Name -> kernel map with alias support. Resolution order is exact name, then
// alias, so a kernel can be shadowed by a registered command of the same name.
class Registry {
public:
    Result<void> add(std::shared_ptr<Kernel> kernel) {
        if (!kernel || kernel->metadata().name.empty()) {
            return failure(ErrorCode::invalid_graph, "kernel registration requires a named kernel");
        }

        const auto& metadata = kernel->metadata();
        if (const auto alias = aliases_.find(metadata.name); alias != aliases_.end()
            && alias->second != metadata.name) {
            return failure(ErrorCode::invalid_graph, "kernel name conflicts with an existing alias", metadata.name);
        }
        for (const std::string& alias : metadata.aliases) {
            if (alias.empty() || alias == metadata.name) {
                continue;
            }
            if (kernels_.contains(alias)) {
                return failure(ErrorCode::invalid_graph, "kernel alias conflicts with an existing kernel name", alias);
            }
            if (const auto found = aliases_.find(alias); found != aliases_.end()
                && found->second != metadata.name) {
                return failure(ErrorCode::invalid_graph, "kernel alias is already registered", alias);
            }
        }

        // Re-registering a kernel replaces its metadata atomically: first drop
        // aliases formerly owned by this name, then install the validated set.
        for (auto it = aliases_.begin(); it != aliases_.end();) {
            if (it->second == metadata.name) {
                it = aliases_.erase(it);
            } else {
                ++it;
            }
        }
        kernels_[metadata.name] = std::move(kernel);
        for (const std::string& alias : metadata.aliases) {
            if (!alias.empty() && alias != metadata.name) {
                aliases_[alias] = metadata.name;
            }
        }
        return {};
    }

    [[nodiscard]] std::shared_ptr<Kernel> find(std::string_view name) const {
        auto direct = kernels_.find(name);
        if (direct != kernels_.end()) {
            return direct->second;
        }

        auto alias = aliases_.find(name);
        if (alias == aliases_.end()) {
            return {};
        }

        auto resolved = kernels_.find(alias->second);
        return resolved == kernels_.end() ? std::shared_ptr<Kernel> {} : resolved->second;
    }

    [[nodiscard]] bool contains(std::string_view name) const { return find(name) != nullptr; }

    [[nodiscard]] std::vector<Metadata> list() const {
        std::vector<Metadata> metadata;
        metadata.reserve(kernels_.size());
        for (const auto& [_, kernel] : kernels_) {
            metadata.push_back(kernel->metadata());
        }
        return metadata;
    }

    [[nodiscard]] std::size_t size() const noexcept { return kernels_.size(); }

private:
    std::map<std::string, std::shared_ptr<Kernel>, std::less<>> kernels_;
    std::map<std::string, std::string, std::less<>> aliases_;
};

struct PackageManifest {
    Metadata metadata;
    std::vector<std::string> entrypoints;
    std::vector<std::string> permissions;
};

class PackageLoader {
public:
    virtual ~PackageLoader() = default;
    virtual Result<PackageManifest> inspect(std::string_view path) = 0;
    virtual Result<std::shared_ptr<Kernel>> load(std::string_view path) = 0;
};

} // namespace kernel
} // namespace lsh
