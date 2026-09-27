#include "notelet/Notelet.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

namespace {
constexpr size_t max_archive = 4 * 1024 * 1024;
constexpr size_t max_entry = 1024 * 1024;

bool id_ok(const std::string &id) {
  return !id.empty() && id.size() <= 64 && id != "." && id != ".." &&
         std::all_of(id.begin(), id.end(), [](unsigned char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
         });
}

bool octal(const char *p, size_t n, size_t &out) {
  out = 0;
  size_t i = 0;
  while (i < n && p[i] == ' ') ++i;
  bool digits = false;
  for (; i < n && p[i] >= '0' && p[i] <= '7'; ++i) {
    digits = true;
    if (out > (max_archive >> 3)) return false;
    out = out * 8 + static_cast<size_t>(p[i] - '0');
  }
  for (; i < n; ++i) if (p[i] != 0 && p[i] != ' ') return false;
  return digits;
}

TS_Status string_result(TS_Value *ret, const std::string &value, TS_Error *err) {
  if (ts_value_make_string(ret, value.c_str()) == TS_OK) return TS_OK;
  ts_error_set(err, TS_ERR_NOMEM, 0, 0, "notelet: out of memory");
  return TS_ERR_NOMEM;
}

TS_Status argument(const TS_Value *args, size_t argc, size_t count,
                   TS_Error *err) {
  if (argc == count) {
    bool valid = true;
    for (size_t i = 0; i < count; ++i) valid &= args[i].type == TS_STRING;
    if (valid) return TS_OK;
  }
  ts_error_set(err, TS_ERR_INVAL, 0, 0, "notelet: expected %zu string arguments", count);
  return TS_ERR_INVAL;
}
}

Notelet::Notelet(std::string id, std::string source, std::string library,
                 std::map<std::string, std::string> assets)
    : id_(std::move(id)), source_(std::move(source)), library_(std::move(library)),
      assets_(std::move(assets)) {}
Notelet::~Notelet() = default;

TS_Status Notelet::key(TS_VM *, void *ctx, const TS_Value *a, size_t n,
                       TS_Value *ret, TS_Error *err) {
  if (auto st = argument(a, n, 0, err); st != TS_OK) return st;
  return string_result(ret, static_cast<Notelet *>(ctx)->key_, err);
}
TS_Status Notelet::name(TS_VM *, void *ctx, const TS_Value *a, size_t n,
                        TS_Value *ret, TS_Error *err) {
  if (auto st = argument(a, n, 0, err); st != TS_OK) return st;
  return string_result(ret, static_cast<Notelet *>(ctx)->id_, err);
}
TS_Status Notelet::resource(TS_VM *, void *ctx, const TS_Value *a, size_t n,
                            TS_Value *ret, TS_Error *err) {
  if (auto st = argument(a, n, 1, err); st != TS_OK) return st;
  const auto &items = static_cast<Notelet *>(ctx)->assets_;
  auto it = items.find(a[0].as.string);
  return it == items.end() ? TS_OK : string_result(ret, it->second, err);
}
TS_Status Notelet::get(TS_VM *, void *ctx, const TS_Value *a, size_t n,
                       TS_Value *ret, TS_Error *err) {
  if (auto st = argument(a, n, 1, err); st != TS_OK) return st;
  const auto &items = static_cast<Notelet *>(ctx)->state_;
  auto it = items.find(a[0].as.string);
  return it == items.end() ? TS_OK : string_result(ret, it->second, err);
}
TS_Status Notelet::set(TS_VM *, void *ctx, const TS_Value *a, size_t n,
                       TS_Value *ret, TS_Error *err) {
  if (auto st = argument(a, n, 2, err); st != TS_OK) return st;
  auto &items = static_cast<Notelet *>(ctx)->state_;
  if (items.size() >= 128 && !items.count(a[0].as.string)) {
    ts_error_set(err, TS_ERR_LIMIT, 0, 0, "notelet: state full");
    return TS_ERR_LIMIT;
  }
  if (std::string(a[0].as.string).size() > 128 ||
      std::string(a[1].as.string).size() > 4096) {
    ts_error_set(err, TS_ERR_LIMIT, 0, 0, "notelet: state value too long");
    return TS_ERR_LIMIT;
  }
  items[a[0].as.string] = a[1].as.string;
  return string_result(ret, a[1].as.string, err);
}

bool Notelet::render(std::string key_name, std::string &error) {
  error.clear();
  key_ = std::move(key_name);
  DT_Error dt_error{};
  DT_TermVM *vm = dt_termscript_create(&dt_error);
  if (!vm) { error = dt_error.message; return false; }
  TS_FuncDef funcs[] = {{"key", &Notelet::key, this},
                        {"name", &Notelet::name, this},
                        {"resource", &Notelet::resource, this},
                        {"get", &Notelet::get, this},
                        {"set", &Notelet::set, this}, {nullptr, nullptr, nullptr}};
  TS_Module module{"diftray.notelet", funcs};
  TS_Error ts_error{};
  if (ts_vm_register_module(dt_termscript_inner_vm(vm), &module, &ts_error) != TS_OK) {
    error = ts_error.message;
    dt_termscript_free(vm);
    return false;
  }
  char *output = nullptr;
  const std::string source = library_ + "\n" + source_;
  const DT_Status status = dt_termscript_run_string(vm, source.c_str(), &output, &dt_error);
  if (status == DT_OK && output && std::char_traits<char>::length(output) <= 65536) {
    frame_ = output;
  } else {
    error = status == DT_OK ? "notelet output exceeds 64 KiB" : dt_error.message;
  }
  std::free(output);
  dt_termscript_free(vm);
  return status == DT_OK && error.empty();
}

bool NoteletCatalog::load(const std::filesystem::path &path, Bundle &out,
                          std::string &error) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > max_archive || size < 1024 || size % 512) {
    error = "invalid notelet archive: " + path.string(); return false;
  }
  std::ifstream input(path, std::ios::binary);
  std::string bytes(size, '\0');
  if (!input.read(bytes.data(), static_cast<std::streamsize>(size))) {
    error = "cannot read notelet: " + path.string(); return false;
  }
  bool end = false;
  for (size_t offset = 0; offset + 512 <= bytes.size();) {
    const char *h = bytes.data() + offset;
    if (std::all_of(h, h + 512, [](char c) { return c == 0; })) {
      end = true; break;
    }
    if (std::string_view(h + 257, 5) != "ustar") {
      error = "notelet requires a ustar archive"; return false;
    }
    size_t checksum = 0, expected = 0, length = 0;
    if (!octal(h + 148, 8, expected) || !octal(h + 124, 12, length) ||
        length > max_entry) { error = "invalid notelet entry size"; return false; }
    for (size_t i = 0; i < 512; ++i)
      checksum += static_cast<unsigned char>(i >= 148 && i < 156 ? ' ' : h[i]);
    if (checksum != expected || (h[156] != '0' && h[156] != 0)) {
      error = "invalid notelet entry"; return false;
    }
    const auto name_end = std::find(h, h + 100, '\0');
    std::string name(h, name_end);
    if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos ||
        std::find(h + 345, h + 500, '\0') != h + 345) {
      error = "unsafe notelet entry"; return false;
    }
    const size_t start = offset + 512;
    const size_t padded = ((length + 511) / 512) * 512;
    if (padded > bytes.size() - start) {
      error = "truncated notelet entry"; return false;
    }
    std::string content = bytes.substr(start, length);
    if (content.find('\0') != std::string::npos) {
      error = "notelet entries must be text: " + name; return false;
    }
    if (name == "main.tsc" && out.main.empty()) out.main = std::move(content);
    else if (name == "notelet.tsc" && out.library.empty()) out.library = std::move(content);
    else if (name.rfind("assets/", 0) == 0 && name.size() > 7) {
      if (out.assets.count(name.substr(7))) {
        error = "duplicate notelet asset: " + name; return false;
      }
      out.assets.emplace(name.substr(7), std::move(content));
    }
    else { error = "unexpected notelet entry: " + name; return false; }
    offset = start + padded;
  }
  if (!end || out.main.empty() || out.library.empty()) {
    error = "notelet needs main.tsc and notelet.tsc"; return false;
  }
  TS_Error parse_error{};
  if (ts_check_syntax(out.main.c_str(), &parse_error) != TS_OK ||
      ts_check_syntax(out.library.c_str(), &parse_error) != TS_OK) {
    error = "invalid notelet script: " + std::string(parse_error.message); return false;
  }
  return true;
}

bool NoteletCatalog::discover(const std::string &paths, std::string &error) {
  bundles_.clear(); names_.clear(); error.clear();
  std::istringstream dirs(paths);
  for (std::string dir; std::getline(dirs, dir, ':');) {
    if (dir.empty()) continue;
    std::filesystem::path root(dir);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
      if (ec) { error = "cannot access notelets: " + root.string(); return false; }
      continue;
    }
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_regular_file(root, ec)) files.push_back(root);
    else if (std::filesystem::is_directory(root, ec)) {
      for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end;
           it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".notelet")
          files.push_back(it->path());
      }
    }
    if (ec) { error = "cannot scan notelets: " + root.string(); return false; }
    std::sort(files.begin(), files.end());
    for (const auto &path : files) {
      if (path.extension() != ".notelet") continue;
      const auto id = path.stem().string();
      if (!id_ok(id)) { error = "invalid notelet name: " + id; return false; }
      if (bundles_.count(id)) continue; // First path wins.
      Bundle bundle;
      if (!load(path, bundle, error)) return false;
      names_.push_back(id);
      bundles_.emplace(id, std::move(bundle));
    }
  }
  return true;
}

std::unique_ptr<Notelet> NoteletCatalog::open(const std::string &id,
                                               std::string &error) const {
  auto it = bundles_.find(id);
  if (it == bundles_.end()) { error = "notelet not found: " + id; return {}; }
  return std::make_unique<Notelet>(id, it->second.main, it->second.library,
                                   it->second.assets);
}
