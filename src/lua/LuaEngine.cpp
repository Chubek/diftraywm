#include "lua/LuaEngine.hpp"
#include "command/CommandBar.hpp"
#include "lua/KaguyaCompat.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>

struct LuaEngine::Impl {
  struct Script {
    Impl &engine;
    std::string name, error;
    size_t allocated = 0;
    unsigned instructions = 0;
    std::chrono::steady_clock::time_point deadline;
    lua_State *lua = nullptr;
    std::unique_ptr<kaguya::State> state;
    std::map<std::string, std::vector<kaguya::LuaFunction>> hooks;
    static void *allocate(void *data, void *ptr, size_t old_size, size_t size) {
      auto &self = *static_cast<Script *>(data);
      if (!ptr) old_size = 0;
      if (!size) { self.allocated -= old_size; std::free(ptr); return nullptr; }
      if (size > 16 * 1024 * 1024 || self.allocated - old_size > 16 * 1024 * 1024 - size) return nullptr;
      void *result = std::realloc(ptr, size);
      if (result) self.allocated = self.allocated - old_size + size;
      return result;
    }
    static void hook(lua_State *lua, lua_Debug *) {
      auto *self = *static_cast<Script **>(lua_getextraspace(lua));
      self->instructions += 1000;
      if (self->instructions >= 100000 || std::chrono::steady_clock::now() >= self->deadline)
        luaL_error(lua, "extension execution budget exceeded");
    }
    void begin() {
      error.clear(); instructions = 0;
      deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
      lua_sethook(lua, hook, LUA_MASKCOUNT, 1000);
    }
    void end() { lua_sethook(lua, nullptr, 0, 0); }
    Script(Impl &parent, std::string script_name) : engine(parent), name(std::move(script_name)) {
#if LUA_VERSION_NUM >= 505
      lua = diftray_lua_newstate(allocate, this);
#else
      lua = lua_newstate(allocate, this);
#endif
      if (!lua) throw std::runtime_error("cannot allocate Lua state");
      *static_cast<Script **>(lua_getextraspace(lua)) = this;
      state = std::make_unique<kaguya::State>(lua);
      state->setErrorHandler([this](int, const char *message) { error = message ? message : "Lua error"; });
      state->openlibs(kaguya::LoadLibs{
        kaguya::LoadLib("_G", luaopen_base), kaguya::LoadLib("table", luaopen_table),
        kaguya::LoadLib("string", luaopen_string), kaguya::LoadLib("math", luaopen_math),
        kaguya::LoadLib("utf8", luaopen_utf8)});
      // No filesystem, network, bytecode loader, coroutine or protected-call
      // escape hatch around the instruction hook. Extensions are event handlers.
      for (const char *key : {"dofile", "loadfile", "load", "print", "warn", "collectgarbage", "pcall", "xpcall", "setmetatable", "getmetatable"})
        (*state)[key] = kaguya::NilValue();
      (*state)["string"]["dump"] = kaguya::NilValue();
      auto api = state->newTable();
      api["status"] = kaguya::function([this](const std::string &text) {
        if (engine.bar) engine.bar->set_status_line(text.substr(0, 4096));
      });
      api["command"] = kaguya::function([this](const std::string &text) {
        if (text.size() > 4096 || engine.commands.size() >= 64) throw std::runtime_error("extension command queue full");
        engine.commands.emplace_back(this, text);
      });
      api["on"] = kaguya::function([this](const std::string &event, kaguya::LuaFunction callback) {
        if (event != "view" && event != "input" && event != "frame") throw std::runtime_error("unknown event");
        if (hooks[event].size() >= 64) throw std::runtime_error("too many event hooks");
        hooks[event].push_back(callback);
      });
      api["register_command"] = kaguya::function([this](const std::string &name, kaguya::LuaFunction callback) {
        if (!engine.bar || !engine.bar->register_command(name, this,
          [this, callback](const std::vector<std::string> &tokens, CommandScope scope) mutable {
            begin();
            try { callback.call<void>(tokens, scope == CommandScope::CELL ? "cell" : "global"); }
            catch (const std::exception &exception) { error = exception.what(); }
            end();
            return error.empty() ? std::string{} : this->name + ": " + error;
          })) throw std::runtime_error("command registration failed: " + name);
      });
      (*state)["diftray"] = api;
    }
    ~Script() {
      if (engine.bar) engine.bar->unregister_owner(this);
      hooks.clear(); state.reset();
      if (lua) lua_close(lua);
    }
  };
  CommandBar *bar;
  bool initialized = false, notifying = false, draining = false;
  std::string error;
  std::vector<std::string> extensions;
  std::deque<std::pair<Script *, std::string>> commands;
  std::vector<std::unique_ptr<Script>> scripts;
  explicit Impl(CommandBar *b) : bar(b) {}
};
LuaEngine::LuaEngine(CommandBar *bar) : impl_(std::make_unique<Impl>(bar)) {}
LuaEngine::~LuaEngine() = default;
bool LuaEngine::init() { impl_->initialized = true; return true; }
bool LuaEngine::load_source(const std::string &name, const std::string &source) {
  if (!impl_->initialized || source.size() > 1024 * 1024 || source.find('\0') != std::string::npos) {
    impl_->error = "invalid extension source"; return false;
  }
  if (std::find(impl_->extensions.begin(), impl_->extensions.end(), name) != impl_->extensions.end()) return true;
  std::unique_ptr<Impl::Script> script;
  try {
    script = std::make_unique<Impl::Script>(*impl_, name);
    script->begin();
    bool success = script->state->dostring(source.c_str());
    script->end();
    if (!success || !script->error.empty()) throw std::runtime_error(script->error);
    impl_->scripts.push_back(std::move(script));
    impl_->extensions.push_back(name);
    return true;
  } catch (const std::exception &exception) {
    impl_->error = name + ": " + exception.what();
    std::erase_if(impl_->commands, [&](const auto &entry) { return entry.first == script.get(); });
    return false;
  }
}
void LuaEngine::scan_extensions() {
  const char *configured = std::getenv("DIFTRAYWM_EXTENSION_PATH");
  const char *xdg = std::getenv("XDG_CONFIG_HOME");
  const auto root = xdg && *xdg ? std::filesystem::path(xdg) :
      std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".config";
  const auto directory = configured && *configured ? std::filesystem::path(configured) : root / "diftraywm/extensions";
  std::error_code ec;
  if (!std::filesystem::exists(directory, ec)) return;
  std::vector<std::filesystem::path> paths;
  for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec))
    if (it->is_regular_file(ec) && it->path().extension() == ".lua") paths.push_back(it->path());
  if (ec) { impl_->error = ec.message(); return; }
  std::sort(paths.begin(), paths.end());
  for (const auto &path : paths) {
    if (std::filesystem::file_size(path, ec) > 1024 * 1024 || ec) { impl_->error = "extension exceeds 1 MiB: " + path.string(); continue; }
    std::ifstream file(path);
    if (!file) { impl_->error = "cannot read extension: " + path.string(); continue; }
    std::string source;
    char data[4096];
    while ((file.read(data, sizeof(data)) || file.gcount()) && source.size() <= 1024 * 1024) source.append(data, file.gcount());
    load_source(path.string(), source);
  }
}
void LuaEngine::notify(const std::string &event) {
  if (impl_->notifying) return;
  impl_->notifying = true;
  for (const auto &script : impl_->scripts) {
    auto hooks = script->hooks[event];
    for (auto &hook : hooks) {
      script->begin();
      try { hook.call<void>(event); } catch (const std::exception &exception) { script->error = exception.what(); }
      script->end();
      if (!script->error.empty()) {
        impl_->error = script->name + ": " + script->error;
        script->hooks[event].clear();
        break;
      }
    }
  }
  impl_->notifying = false;
}
void LuaEngine::drain_commands() {
  if (!impl_->bar || impl_->draining) return;
  impl_->draining = true;
  const auto count = std::min<size_t>(impl_->commands.size(), 16);
  const auto saved_scope = impl_->bar->scope;
  impl_->bar->scope = CommandScope::NCURSOR_GLOBAL;
  for (size_t i = 0; i < count; ++i) {
    auto command = std::move(impl_->commands.front().second);
    impl_->commands.pop_front();
    impl_->bar->dispatch(command);
  }
  impl_->bar->scope = saved_scope;
  impl_->draining = false;
}
bool LuaEngine::initialized() const { return impl_->initialized; }
const std::vector<std::string> &LuaEngine::extensions() const { return impl_->extensions; }
const std::string &LuaEngine::error() const { return impl_->error; }
