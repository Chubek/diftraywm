#pragma once

#include <domterm.h>
#include <termscript/termscript.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>


class Notelet {
public:
  Notelet(std::string id, std::string source, std::string library,
          std::map<std::string, std::string> assets);
  ~Notelet();
  Notelet(const Notelet &) = delete;
  Notelet &operator=(const Notelet &) = delete;
  const std::string &id() const { return id_; }
  const std::string &frame() const { return frame_; }
  bool render(std::string key, std::string &error);

private:
  static TS_Status key(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  static TS_Status name(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  static TS_Status resource(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  static TS_Status get(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  static TS_Status set(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  std::string id_, source_, library_, frame_, key_;
  std::map<std::string, std::string> assets_;
  std::map<std::string, std::string> state_;
};

class NoteletCatalog {
public:
  bool discover(const std::string &paths, std::string &error);
  const std::vector<std::string> &names() const { return names_; }
  std::unique_ptr<Notelet> open(const std::string &id, std::string &error) const;

private:
  struct Bundle {
    std::string main, library;
    std::map<std::string, std::string> assets;
  };
  static bool load(const std::filesystem::path &path, Bundle &out, std::string &error);
  std::map<std::string, Bundle> bundles_;
  std::vector<std::string> names_;
};
