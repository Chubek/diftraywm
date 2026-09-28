#pragma once

#include <domterm.h>
#include <termscript/termscript.h>

#include <filesystem>
#include <chrono>
#include <deque>
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
  bool render(std::string key, std::string &error, std::string event = "key");
  void set_context(std::map<std::string, std::string> context) { context_ = std::move(context); }
  bool request_render(std::string key, std::string &error, std::string event = "key");
  bool poll(std::string &error);
  bool busy() const { return worker_ > 0 || !events_.empty(); }



private:
  bool start_worker(std::string &error);
  void stop_worker();
  static TS_Status context(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  static TS_Status event(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  static TS_Status edit(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  static TS_Status erase(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  static TS_Status key(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  static TS_Status name(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  static TS_Status resource(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  static TS_Status get(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  static TS_Status set(TS_VM *, void *, const TS_Value *, size_t, TS_Value *, TS_Error *);
  std::string id_, source_, library_, frame_, key_;
  std::map<std::string, std::string> assets_;
  std::map<std::string, std::string> state_, context_;
  std::string event_;
  std::deque<std::pair<std::string, std::string>> events_;
  int worker_ = -1, read_fd_ = -1;
  std::string worker_bytes_;
  std::chrono::steady_clock::time_point deadline_;

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
