/* termlib.hpp -- Termlib C++ API (header-only, C++17).
 *
 * Termlib provides PTY/TTY management, Terminfo parsing and expansion,
 * terminal session orchestration, session recording/replay, local and
 * remote terminal connections, and the Termscript automation language.
 * This header is the C++ face of that library: it wraps every call in
 * termlib.h, owns the resources the C API asks the caller to release,
 * and reports outcomes in C++ terms.
 *
 * ============================================================
 *  QUICK START
 * ============================================================
 *
 * Spawn a child on a PTY, talk to it, and reap it:
 *
 *   termlib::pty_options options;
 *   options.argv = {"/bin/sh", "-i"};
 *   options.terminal_name = "xterm-256color";
 *   options.rows = 24;
 *   options.columns = 80;
 *
 *   termlib::pty_session shell = termlib::pty_session::spawn(options);
 *   shell.set_nonblocking(true);
 *   if (auto chunk = shell.read_some())          // normal end of stream is
 *     std::cout << chunk.value();               // reported, not thrown
 *   const termlib::child_exit status = shell.wait();
 *
 * Look up and expand a Terminfo capability:
 *
 *   termlib::database db = termlib::database::open_default();
 *   termlib::profile vt = db.load("xterm-256color");
 *   const std::string move = vt.expand("cup", {3, 5});   // row 4, col 6
 *
 * Frame messages over a local stream:
 *
 *   termlib::local_connection left{termlib::local_options{sock[0], true}};
 *   left.send(termlib::envelope::make_data(1, "hello")).raise();
 *
 * Failures throw termlib::error; expected conditions (end of stream,
 * timeout, would-block, cancellation, an already-exited child) are
 * returned through termlib::result instead of being turned into
 * exceptions.
 *
 * ============================================================
 *  ERROR MODEL
 * ============================================================
 *
 * - Every C operation that can only fail is wrapped in a method that
 *   throws termlib::error (derived from std::runtime_error) carrying
 *   the DT_Status, the preserved errno, the parser offset and the
 *   diagnostic message.
 * - Operations whose C contract can report a *normal* condition return
 *   termlib::result<T> instead: DT_ERR_EOF, DT_ERR_TIMEOUT,
 *   DT_ERR_WOULD_BLOCK, DT_ERR_CANCELLED and DT_ERR_CHILD_EXITED are
 *   values to inspect, never exceptions.  result<T>::value() throws
 *   when the state is not ok, so a caller who does not care can write
 *   `if (auto r = pty.read(buf, n)) use(r.value());` and a caller who
 *   wants the details can write `switch (r.state()) { ... }`.
 * - Best-effort loops (write_all) report progress and state separately
 *   through termlib::written.
 * - Lookups that are documented to fail softly return std::optional
 *   (absent/unknown capabilities) instead of throwing.
 *
 * ============================================================
 *  OWNERSHIP, LIFETIME AND THREAD-SAFETY
 * ============================================================
 *
 * - RAII: database, profile, entry_parser, tty_session, pty_session,
 *   puppeteer, recorder, replayer, message, envelope, local_connection,
 *   remote_connection and script_vm own their C handle and release it
 *   in the destructor.  All of them are move-only; copy operations are
 *   deleted so a handle can never be released twice.
 * - A destructor is safe on a default-constructed object (an empty
 *   wrapper releases nothing) and safe after a failed factory call.
 * - Non-owning observers (profile_view, message_view, capability,
 *   bytes_view, winsize, record_event payload, term_value) are plain
 *   values.  Views returned by *view() methods borrow from the object
 *   they came from: they stay valid while that object lives and must
 *   never outlive it.  string_view values from a profile borrow the
 *   profile's storage.
 * - File descriptors keep the C semantics: a tty_session closes its fd
 *   only when its options asked it to, pty_session always owns its
 *   master, and no wrapper ever closes a descriptor it merely borrows.
 * - Buffer ownership: input buffers are borrowed and copied when the
 *   implementation has to retain them.  Replayed events and received
 *   payloads are copied into std::string / std::vector, so they survive
 *   the next call on the replayer or connection.
 * - Thread safety: no object may be used concurrently from two threads
 *   unless documented.  The thread-safe members are
 *   local_connection::cancel(), remote_connection::cancel() and
 *   puppeteer::request_resize() (async-signal-safe; callable from a
 *   SIGWINCH handler).  Separate objects may be used on separate
 *   threads.  There is no hidden global mutable state.
 * - Blocking calls retry EINTR internally and honour the timeouts
 *   configured on the object (see local_options/remote_options).
 * - SECURITY: remote_connection is raw TCP.  It is NOT encrypted, it
 *   has no peer-credential verification and no replay protection; the
 *   auth token is a bearer secret sent in the clear.  Only use it on a
 *   trusted local network or behind an external secure tunnel.
 */

#ifndef TERMLIB_HPP
#define TERMLIB_HPP

#include "termlib.h"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace termlib
{

/* ------------------------------------------------------------------ */
/* Status, diagnostics and results.                                    */
/* ------------------------------------------------------------------ */

/* Mirror of DT_Status.  DT_OK is 0; every other value is a failure or a
   normal condition (end of stream, timeout, would-block, cancellation,
   an already-exited child, a protocol or limit violation). */
enum class status : int
{
  ok = DT_OK,
  invalid_argument = DT_ERR_INVALID_ARGUMENT,
  no_memory = DT_ERR_NO_MEMORY,
  io = DT_ERR_IO,
  not_found = DT_ERR_NOT_FOUND,
  parse = DT_ERR_PARSE,
  unsupported = DT_ERR_UNSUPPORTED,
  system = DT_ERR_SYSTEM,
  eof = DT_ERR_EOF,
  timeout = DT_ERR_TIMEOUT,
  would_block = DT_ERR_WOULD_BLOCK,
  child_exited = DT_ERR_CHILD_EXITED,
  cancelled = DT_ERR_CANCELLED,
  protocol = DT_ERR_PROTOCOL,
  limit = DT_ERR_LIMIT,
  auth = DT_ERR_AUTH
};

inline constexpr bool is_ok (status state) noexcept
{
  return state == status::ok;
}

/* Orderly end of stream: the peer closed, the child is gone. */
inline constexpr bool is_end_of_stream (status state) noexcept
{
  return state == status::eof;
}

/* "Nothing happened yet" conditions on a healthy non-blocking object:
   retry after waiting for readiness. */
inline constexpr bool is_retryable (status state) noexcept
{
  return state == status::would_block || state == status::timeout ||
         state == status::cancelled;
}

/* Stable, never-NULL, static string; unknown values yield "UNKNOWN". */
inline const char *to_string (status state) noexcept
{
  return dt_status_string (static_cast<DT_Status> (state));
}

/* Owned copy of a DT_Error.  `message` is never empty for a non-ok
   state; `offset` is the byte offset of a parser failure and is only
   meaningful when the operation documents one. */
struct diagnostic
{
  status state{status::ok};
  int system_errno{0};
  std::size_t offset{0};
  std::string message;

  bool ok () const noexcept { return state == status::ok; }
  explicit operator bool () const noexcept { return ok (); }
};

/* Thrown by every Termlib method whose C operation can only fail. */
class error : public std::runtime_error
{
public:
  explicit error (diagnostic info)
      : std::runtime_error (info.message.empty ()
                                ? std::string (to_string (info.state))
                                : info.message),
        info_ (std::move (info))
  {
  }

  status state () const noexcept { return info_.state; }
  const diagnostic &info () const noexcept { return info_; }
  int system_errno () const noexcept { return info_.system_errno; }
  std::size_t offset () const noexcept { return info_.offset; }

private:
  diagnostic info_;
};

namespace detail
{

/* Copy a DT_Error into an owning diagnostic. */
inline diagnostic
diagnose (DT_Status state, const DT_Error &raw)
{
  diagnostic info;
  info.state = static_cast<status> (state);
  info.system_errno = raw.system_errno;
  info.offset = raw.offset;
  info.message = raw.message[0] != '\0' ? raw.message : to_string (info.state);
  return info;
}

[[noreturn]] inline void
fail (DT_Status state, const DT_Error &raw)
{
  throw error (diagnose (state, raw));
}

inline void
check (DT_Status state, const DT_Error &raw)
{
  if (state != DT_OK)
    fail (state, raw);
}

/* C strings cannot carry embedded NUL bytes; reject them loudly instead
   of silently truncating a caller-supplied name. */
inline std::string
c_string (std::string_view text, const char *what)
{
  if (text.find ('\0') != std::string_view::npos)
    {
      diagnostic info;
      info.state = status::invalid_argument;
      info.message = std::string (what) + " must not contain a NUL byte";
      throw error (std::move (info));
    }
  return std::string (text);
}

/* exec-style argument vectors need mutable char pointers; exec never
   writes through them. */
inline std::vector<char *>
mutable_arguments (const std::vector<std::string_view> &arguments)
{
  std::vector<char *> raw;
  raw.reserve (arguments.size () + 1);
  for (const std::string_view argument : arguments)
    raw.push_back (const_cast<char *> (argument.data ()));
  raw.push_back (nullptr);
  return raw;
}

/* Shared RAII skeleton for the wrapper types: one owned C handle, one
   C cleanup function, move-only semantics and a null-safe release. */
template <class Derived, class Handle, void (*Cleanup) (Handle)>
class unique_handle
{
public:
  unique_handle () noexcept = default;
  explicit unique_handle (Handle raw) noexcept : raw_ (raw) {}
  ~unique_handle () { reset (); }

  unique_handle (unique_handle &&other) noexcept
      : raw_ (std::exchange (other.raw_, nullptr))
  {
  }

  unique_handle &
  operator= (unique_handle &&other) noexcept
  {
    if (this != &other)
      {
        reset ();
        raw_ = std::exchange (other.raw_, nullptr);
      }
    return *this;
  }

  unique_handle (const unique_handle &) = delete;
  unique_handle &operator= (const unique_handle &) = delete;

  /* Borrowed native handle; null when the wrapper is empty. */
  Handle native_handle () const noexcept { return raw_; }
  bool valid () const noexcept { return raw_ != nullptr; }

  /* Deterministic release; safe on an empty wrapper and idempotent. */
  void reset (Handle raw = nullptr) noexcept
  {
    if (raw_ != nullptr)
      Cleanup (raw_);
    raw_ = raw;
  }

  /* Give up ownership without running the cleanup function. */
  Handle release () noexcept { return std::exchange (raw_, nullptr); }

protected:
  Handle raw_{};
};

} /* namespace detail */

/* Result of an operation that can report a normal condition.  The
   payload is valid only when the state is ok; value() throws otherwise.
   Always check ok() (or use the explicit bool conversion) before
   reading the payload. */
template <class T>
class result
{
public:
  using value_type = T;

  result () = default;

  static result
  success (T value)
  {
    result out;
    out.value_ = std::move (value);
    return out;
  }

  static result
  failure (status state, diagnostic info)
  {
    info.state = state;
    result out;
    out.info_ = std::move (info);
    return out;
  }

  bool ok () const noexcept { return info_.state == status::ok; }
  explicit operator bool () const noexcept { return ok (); }
  status state () const noexcept { return info_.state; }
  const diagnostic &info () const noexcept { return info_; }

  T &value () &
  {
    raise ();
    return value_;
  }

  const T &value () const &
  {
    raise ();
    return value_;
  }

  T
  value () &&
  {
    raise ();
    return std::move (value_);
  }

  T value_or (T fallback) const
  {
    return ok () ? value_ : std::move (fallback);
  }

  /* Throw when the operation did not succeed. */
  void
  raise () const
  {
    if (!ok ())
      throw error (info_);
  }

private:
  T value_{};
  diagnostic info_{};
};

/* Specialisation for operations without a payload. */
template <>
class result<void>
{
public:
  using value_type = void;

  result () noexcept = default;

  static result success () noexcept { return result (); }

  static result
  failure (status state, diagnostic info)
  {
    info.state = state;
    result out;
    out.info_ = std::move (info);
    return out;
  }

  bool ok () const noexcept { return info_.state == status::ok; }
  explicit operator bool () const noexcept { return ok (); }
  status state () const noexcept { return info_.state; }
  const diagnostic &info () const noexcept { return info_; }

  void
  raise () const
  {
    if (!ok ())
      throw error (info_);
  }

private:
  diagnostic info_{};
};

/* One I/O transfer: a state plus the number of bytes moved.  The count
   is meaningful only when the state is ok; partial transfers are normal
   (the caller loops). */
using transfer = result<std::size_t>;

/* Outcome of a best-effort "write everything" loop: progress and state
   are reported independently so a partial write is never lost. */
struct written
{
  std::size_t bytes{0};
  status state{status::ok};
  diagnostic info;

  bool ok () const noexcept { return state == status::ok; }
  explicit operator bool () const noexcept { return ok (); }

  void
  raise () const
  {
    if (!ok ())
      throw error (info);
  }

  /* Build an outcome: `bytes` bytes were accepted before `state`. */
  static written from (status state, diagnostic info, std::size_t bytes)
  {
    written out;
    out.state = state;
    out.info = std::move (info);
    if (!out.info.ok ())
      out.info.state = state;
    out.bytes = bytes;
    return out;
  }
};

/* ------------------------------------------------------------------ */
/* Library information.                                                 */
/* ------------------------------------------------------------------ */

inline constexpr int version_major = DT_VERSION_MAJOR;
inline constexpr int version_minor = DT_VERSION_MINOR;
inline constexpr int version_patch = DT_VERSION_PATCH;

/* Packed as (major << 16) | (minor << 8) | patch. */
inline unsigned int
version_number () noexcept
{
  return dt_version_number ();
}

/* "major.minor.patch". */
inline std::string
version_string ()
{
  return std::string (dt_version_string ());
}

/* Feature probe: "pty", "tty", "terminfo", "record", "replay",
   "local-conn", "remote-conn", "termscript", "puppeteer".
   "tls" is always false: raw remote TCP is never encrypted. */
inline bool
has_feature (std::string_view feature)
{
  return dt_feature_query (detail::c_string (feature, "feature").c_str ());
}

/* Portable limits enforced by the library. */
inline constexpr std::size_t max_expand_bytes = DT_TI_MAX_EXPAND;
inline constexpr std::size_t max_expand_steps = DT_TI_MAX_STEPS;
inline constexpr std::size_t max_stack_depth = DT_TI_MAX_STACK;
inline constexpr std::size_t max_entry_bytes = DT_TI_MAX_ENTRY;
inline constexpr std::size_t max_record_payload = DT_RECORD_MAX_PAYLOAD;
inline constexpr std::size_t max_message_bytes = DT_CONN_MAX_MESSAGE;

/* ------------------------------------------------------------------ */
/* Borrowed byte ranges.                                                */
/* ------------------------------------------------------------------ */

/* Non-owning view of binary data.  The referenced bytes must outlive
   the view.  text views are byte views too: no encoding is assumed. */
class bytes_view
{
public:
  constexpr bytes_view () noexcept = default;
  constexpr bytes_view (const std::uint8_t *data, std::size_t size) noexcept
      : data_ (data), size_ (size)
  {
  }
  bytes_view (std::string_view text) noexcept
      : data_ (reinterpret_cast<const std::uint8_t *> (text.data ())),
        size_ (text.size ())
  {
  }
  bytes_view (const std::vector<std::uint8_t> &bytes) noexcept
      : data_ (bytes.data ()), size_ (bytes.size ())
  {
  }

  /* View any contiguous container (std::string, std::array, ...). */
  template <class Container>
  static bytes_view
  of (const Container &container) noexcept
  {
    return bytes_view (
        reinterpret_cast<const std::uint8_t *> (std::data (container)),
        std::size (container));
  }

  const std::uint8_t *data () const noexcept { return data_; }
  std::size_t size () const noexcept { return size_; }
  bool empty () const noexcept { return size_ == 0; }
  const std::uint8_t *begin () const noexcept { return data_; }
  const std::uint8_t *end () const noexcept { return data_ + size_; }
  std::uint8_t operator[] (std::size_t index) const noexcept
  {
    return data_[index];
  }

  /* Copy out as text (NUL bytes are preserved). */
  std::string to_string () const
  {
    return size_ == 0 ? std::string ()
                      : std::string (reinterpret_cast<const char *> (data_),
                                     size_);
  }

private:
  const std::uint8_t *data_{};
  std::size_t size_{};
};

/* ------------------------------------------------------------------ */
/* Terminfo: capabilities, profiles, databases and the entry parser.   */
/* ------------------------------------------------------------------ */

enum class capability_type
{
  boolean = DT_TI_BOOL,
  number = DT_TI_NUMBER,
  text = DT_TI_STRING
};

inline const char *
to_string (capability_type type) noexcept
{
  switch (type)
    {
    case capability_type::boolean:
      return "boolean";
    case capability_type::number:
      return "number";
    case capability_type::text:
      return "text";
    }
  return "unknown";
}

/* A capability value.  The string alternative borrows the profile's
   storage and stays valid while the profile lives. */
using capability_value = std::variant<bool, std::int32_t, std::string_view>;

/* Metadata for any known capability name. */
struct capability
{
  std::string_view name;        /* Borrowed short name, e.g. "cup". */
  capability_type type{capability_type::boolean};
  unsigned int index{0};        /* Index within its type section. */
  bool present{false};          /* False when absent in this profile. */
};

/* Tagged expansion parameter: a number (the default) or text. */
struct parameter
{
  parameter () = default;
  parameter (std::int32_t number) noexcept : number_ (number) {}
  parameter (std::string_view text) noexcept : text_ (text), text_flag_ (true)
  {
  }
  parameter (const char *text) noexcept
      : parameter (std::string_view (text != nullptr ? text : ""))
  {
  }

  bool is_text () const noexcept { return text_flag_; }
  std::int32_t number () const noexcept { return number_; }
  std::string_view text () const noexcept { return text_; }

private:
  std::int32_t number_{0};
  std::string_view text_{};
  bool text_flag_{false};
};

/* Read-only view over a compiled Terminfo profile.  Borrowed: it never
   owns the profile, so it must not outlive the object it came from. */
class profile_view
{
public:
  profile_view () noexcept = default;
  explicit profile_view (DT_TIProf *raw) noexcept : raw_ (raw) {}

  /* Borrowed primary name (first entry of the names field). */
  std::string_view name () const noexcept;
  /* Number of '|' aliases after the primary name. */
  std::size_t alias_count () const noexcept;
  /* Borrowed alias, empty when out of range. */
  std::string_view alias (std::size_t index) const noexcept;
  std::vector<std::string_view> aliases () const;

  /* Typed lookup; std::nullopt when the capability is unknown or
     absent.  Text values borrow the profile. */
  std::optional<capability_value> value_of (std::string_view name) const noexcept;
  std::optional<bool> boolean (std::string_view name) const noexcept;
  std::optional<std::int32_t> number (std::string_view name) const noexcept;
  std::optional<std::string_view> text (std::string_view name) const noexcept;
  /* True when the capability exists and is present in this profile. */
  bool has (std::string_view name) const noexcept;

  /* Introspection for any known capability name. */
  std::optional<capability>
  capability_info (std::string_view name) const;
  /* Number of known capabilities (bool + numeric + string). */
  static std::size_t capability_count () noexcept;
  /* Metadata for the i-th known capability in canonical order. */
  static std::optional<capability>
  capability_by_index (std::size_t index) noexcept;

  /* Expand a parameterized string capability.  Throws
     status::not_found for unknown names, status::invalid_argument for
     non-string capabilities and status::limit when the output or step
     budget (max_expand_bytes / max_expand_steps) is exhausted.  At most
     nine numeric parameters are used. */
  std::string
  expand (std::string_view name, const std::vector<std::int32_t> &parameters = {})
      const;
  /* Same, with tagged numeric/text parameters. */
  std::string expand_tagged (std::string_view name,
                             const std::vector<parameter> &parameters) const;
  /* Expand into caller storage.  Returns the required size excluding
     the terminating NUL and truncates to fit `capacity`; the buffer is
     NUL-terminated when capacity is non-zero. */
  std::size_t expand_into (std::string_view name, char *output,
                           std::size_t capacity,
                           const std::vector<parameter> &parameters = {}) const;

  bool valid () const noexcept { return raw_ != nullptr; }
  DT_TIProf *native_handle () const noexcept { return raw_; }

protected:
  DT_TIProf *raw_{};
};

inline std::string_view
profile_view::name () const noexcept
{
  const char *name = dt_tiprof_name (raw_);
  return name != nullptr ? std::string_view (name) : std::string_view ();
}

inline std::size_t
profile_view::alias_count () const noexcept
{
  return dt_tiprof_alias_count (raw_);
}

inline std::string_view
profile_view::alias (std::size_t index) const noexcept
{
  const char *name = dt_tiprof_alias (raw_, index);
  return name != nullptr ? std::string_view (name) : std::string_view ();
}

inline std::vector<std::string_view>
profile_view::aliases () const
{
  std::vector<std::string_view> out;
  const std::size_t count = alias_count ();
  out.reserve (count);
  for (std::size_t i = 0; i < count; ++i)
    out.push_back (alias (i));
  return out;
}

inline std::optional<capability_value>
profile_view::value_of (std::string_view name) const noexcept
{
  DT_TIValue value{};
  if (!dt_tiprof_get (raw_, detail::c_string (name, "capability").c_str (),
                      &value))
    return std::nullopt;
  switch (value.type)
    {
    case DT_TI_BOOL:
      return capability_value{value.as.boolean};
    case DT_TI_NUMBER:
      return capability_value{value.as.number};
    case DT_TI_STRING:
      return capability_value{std::string_view (
          value.as.string != nullptr ? value.as.string : "")};
    }
  return std::nullopt;
}

inline std::optional<bool>
profile_view::boolean (std::string_view name) const noexcept
{
  const std::optional<capability_value> value = value_of (name);
  if (!value || !std::holds_alternative<bool> (*value))
    return std::nullopt;
  return std::get<bool> (*value);
}

inline std::optional<std::int32_t>
profile_view::number (std::string_view name) const noexcept
{
  const std::optional<capability_value> value = value_of (name);
  if (!value || !std::holds_alternative<std::int32_t> (*value))
    return std::nullopt;
  return std::get<std::int32_t> (*value);
}

inline std::optional<std::string_view>
profile_view::text (std::string_view name) const noexcept
{
  const std::optional<capability_value> value = value_of (name);
  if (!value || !std::holds_alternative<std::string_view> (*value))
    return std::nullopt;
  return std::get<std::string_view> (*value);
}

inline bool
profile_view::has (std::string_view name) const noexcept
{
  return dt_tiprof_has (raw_, detail::c_string (name, "capability").c_str ());
}

inline std::optional<capability>
profile_view::capability_info (std::string_view name) const
{
  DT_TICapInfo info{};
  if (!dt_tiprof_cap_info (raw_, detail::c_string (name, "capability").c_str (),
                           &info))
    return std::nullopt;
  capability out;
  out.name = info.name != nullptr ? std::string_view (info.name)
                                  : std::string_view ();
  out.type = static_cast<capability_type> (info.type);
  out.index = info.index;
  out.present = info.present;
  return out;
}

inline std::size_t
profile_view::capability_count () noexcept
{
  return dt_tiprof_cap_count ();
}

inline std::optional<capability>
profile_view::capability_by_index (std::size_t index) noexcept
{
  DT_TICapInfo info{};
  if (!dt_tiprof_cap_by_index (index, &info))
    return std::nullopt;
  capability out;
  out.name = info.name != nullptr ? std::string_view (info.name)
                                  : std::string_view ();
  out.type = static_cast<capability_type> (info.type);
  out.index = info.index;
  out.present = info.present;
  return out;
}

inline std::string
profile_view::expand (std::string_view name,
                      const std::vector<std::int32_t> &numbers) const
{
  std::vector<char> buffer (max_expand_bytes + 1);
  DT_Error raw{};
  const std::size_t needed
      = dt_tiprof_expand (raw_, detail::c_string (name, "capability").c_str (),
                          numbers.data (), numbers.size (), buffer.data (),
                          buffer.size (), &raw);
  if (needed == SIZE_MAX)
    detail::fail (raw.code != DT_OK ? raw.code : DT_ERR_PARSE, raw);
  return std::string (buffer.data (), needed);
}

inline std::string
profile_view::expand_tagged (std::string_view name,
                             const std::vector<parameter> &parameters) const
{
  /* Text parameters must be NUL-terminated for the C layer. */
  std::vector<std::string> text;
  std::vector<DT_TIParam> raw;
  text.reserve (parameters.size ());
  raw.reserve (parameters.size ());
  for (const parameter &entry : parameters)
    {
      DT_TIParam item{};
      if (entry.is_text ())
        {
          text.push_back (std::string (entry.text ()));
          item.is_string = true;
          item.string = text.back ().c_str ();
        }
      else
        item.number = entry.number ();
      raw.push_back (item);
    }
  std::vector<char> buffer (max_expand_bytes + 1);
  DT_Error error_raw{};
  const std::size_t needed = dt_tiprof_expand_params (
      raw_, detail::c_string (name, "capability").c_str (), raw.data (),
      raw.size (), buffer.data (), buffer.size (), &error_raw);
  if (needed == SIZE_MAX)
    detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_PARSE,
                  error_raw);
  return std::string (buffer.data (), needed);
}

inline std::size_t
profile_view::expand_into (std::string_view name, char *output,
                           std::size_t capacity,
                           const std::vector<parameter> &parameters) const
{
  if (capacity != 0 && output == nullptr)
    {
      diagnostic info;
      info.state = status::invalid_argument;
      info.message = "output buffer must not be null when capacity > 0";
      throw error (std::move (info));
    }
  std::vector<std::string> text;
  std::vector<DT_TIParam> raw;
  text.reserve (parameters.size ());
  raw.reserve (parameters.size ());
  for (const parameter &entry : parameters)
    {
      DT_TIParam item{};
      if (entry.is_text ())
        {
          text.push_back (std::string (entry.text ()));
          item.is_string = true;
          item.string = text.back ().c_str ();
        }
      else
        item.number = entry.number ();
      raw.push_back (item);
    }
  DT_Error error_raw{};
  const std::size_t needed = dt_tiprof_expand_params (
      raw_, detail::c_string (name, "capability").c_str (),
      raw.empty () ? nullptr : raw.data (), raw.size (), output, capacity,
      &error_raw);
  if (needed == SIZE_MAX)
    detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_PARSE,
                  error_raw);
  return needed;
}

/* Owning handle for a compiled Terminfo profile. */
class profile final : public profile_view,
                       private detail::unique_handle<profile, DT_TIProf *,
                                                    dt_tiprof_free>
{
  using handle_base = detail::unique_handle<profile, DT_TIProf *, dt_tiprof_free>;

public:
  profile () noexcept = default;
  /* Adopt an existing profile (transferred reference). */
  explicit profile (DT_TIProf *raw) noexcept : profile_view (raw), handle_base (raw)
  {
  }

  using handle_base::native_handle;
  using handle_base::release;
  using handle_base::reset;
  using handle_base::valid;
};

/* Terminfo database: an ordered list of search directories.  Opening a
   database copies the paths, so temporaries are fine. */
class database final
    : private detail::unique_handle<database, DT_TIDB *, dt_tidb_close>
{
  using handle_base
      = detail::unique_handle<database, DT_TIDB *, dt_tidb_close>;

public:
  database () noexcept = default;
  /* Adopt an existing database (transferred reference). */
  explicit database (DT_TIDB *raw) noexcept : handle_base (raw) {}

  /* Open with an explicit search path.  With no paths the database has
     an empty path list and every load fails with status::not_found. */
  static database
  open (std::initializer_list<std::string_view> search_paths = {})
  {
    return open (std::vector<std::string_view> (search_paths));
  }

  static database open (const std::vector<std::string_view> &search_paths)
  {
    std::vector<std::string> owned;
    std::vector<const char *> raw;
    owned.reserve (search_paths.size ());
    raw.reserve (search_paths.size ());
    for (const std::string_view path : search_paths)
      {
        owned.push_back (detail::c_string (path, "search path"));
        raw.push_back (owned.back ().c_str ());
      }
    DT_Error error_raw{};
    DT_TIDB *raw_db = dt_tidb_open (raw.empty () ? nullptr : raw.data (),
                                    raw.size (), &error_raw);
    if (raw_db == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_SYSTEM,
                    error_raw);
    return database (raw_db);
  }

  /* Open with the environment-derived search path: explicit paths win;
     otherwise $TERMINFO, then $TERMINFO_DIRS, then $HOME/.terminfo,
     then the compiled-in fallback list. */
  static database open_default ()
  {
    DT_Error error_raw{};
    DT_TIDB *raw_db = dt_tidb_open_default (&error_raw);
    if (raw_db == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_SYSTEM,
                    error_raw);
    return database (raw_db);
  }

  /* Load the entry for `terminal_name`, matching the primary name or any
     '|'-separated alias.  Does not mutate the database. */
  profile load (std::string_view terminal_name) const
  {
    DT_Error error_raw{};
    DT_TIProf *raw_profile
        = dt_tiprof_load (const_cast<DT_TIDB *> (native_handle ()),
                          detail::c_string (terminal_name, "terminal name")
                              .c_str (),
                          &error_raw);
    if (raw_profile == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_NOT_FOUND,
                    error_raw);
    return profile (raw_profile);
  }

  /* Load the entry named by $TERM.  Does not mutate the database. */
  profile load_default () const
  {
    DT_Error error_raw{};
    DT_TIProf *raw_profile = dt_tidb_load_default (
        const_cast<DT_TIDB *> (native_handle ()), &error_raw);
    if (raw_profile == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_NOT_FOUND,
                    error_raw);
    return profile (raw_profile);
  }

  using handle_base::native_handle;
  using handle_base::release;
  using handle_base::reset;
  using handle_base::valid;
};

/* Incremental parser for compiled Terminfo entries (16-bit and 32-bit
   magic numbers, little-endian, plus the extended capability section).
   Malformed or truncated input fails with status::parse without reading
   outside the buffer. */
class entry_parser final
    : private detail::unique_handle<entry_parser, DT_TIParser *,
                                    dt_tiparser_free>
{
  using handle_base
      = detail::unique_handle<entry_parser, DT_TIParser *, dt_tiparser_free>;

public:
  entry_parser ()
  {
    DT_Error error_raw{};
    DT_TIParser *raw_parser = dt_tiparser_create (&error_raw);
    if (raw_parser == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_NO_MEMORY,
                    error_raw);
    reset (raw_parser);
  }

  /* Discard buffered input and return to the initial state. */
  void reset_input () noexcept { dt_tiparser_reset (native_handle ()); }

  /* Feed bytes; pass final_chunk when no more input will arrive.  The
     compiled format carries no total length, so bytes are buffered until
     the final chunk.  Returns the number of bytes accepted. */
  std::size_t feed (const void *data, std::size_t size, bool final_chunk)
  {
    DT_Error error_raw{};
    std::size_t consumed = 0;
    const DT_Status state = dt_tiparser_feed (native_handle (),
                                              static_cast<const std::uint8_t *> (
                                                  data),
                                              size, final_chunk, &consumed,
                                              &error_raw);
    detail::check (state, error_raw);
    return consumed;
  }

  std::size_t
  feed (bytes_view data, bool final_chunk = false)
  {
    return feed (data.data (), data.size (), final_chunk);
  }

  /* Transfer the parsed profile to the caller.  std::nullopt when no
     final chunk was seen or the entry is malformed; the C parser does
     not report a diagnostic for this case (use parse() for one). */
  std::optional<profile> take () noexcept
  {
    DT_TIProf *raw_profile = dt_tiparser_take_profile (native_handle ());
    if (raw_profile == nullptr)
      return std::nullopt;
    return profile (raw_profile);
  }

  /* Convenience: parse one complete compiled entry. */
  static profile parse (bytes_view entry)
  {
    entry_parser parser;
    parser.feed (entry, true);
    if (std::optional<profile> parsed = parser.take ())
      return std::move (*parsed);
    diagnostic info;
    info.state = status::parse;
    info.message = "malformed or truncated terminfo entry";
    throw error (std::move (info));
  }

  using handle_base::native_handle;
  using handle_base::release;
  using handle_base::valid;
};

/* ------------------------------------------------------------------ */
/* TTY sessions.                                                       */
/* ------------------------------------------------------------------ */

/* Mirrors DT_TTYOptions.  `fd` must name an open terminal descriptor. */
struct tty_options
{
  int fd{-1};
  bool take_ownership{false}; /* Close `fd` when the session is closed. */
  bool no_restore{false};     /* Skip the termios restore on close. */
  bool nonblocking{false};    /* Set O_NONBLOCK on open. */
};

/* Portable termios subset.  The defaults describe a cooked terminal. */
struct tty_settings
{
  bool canonical{true};
  bool echo{true};
  bool signals{true};
  bool input_processing{true};
  bool output_processing{true};
  unsigned int read_timeout_ds{0}; /* VTIME in deciseconds, 0 = blocking. */
};

/* Terminal dimensions and (optional) pixel dimensions. */
struct winsize
{
  unsigned short rows{0};
  unsigned short columns{0};
  unsigned short xpixel{0};
  unsigned short ypixel{0};
};

/* Wraps an already-open terminal descriptor.  The current termios state
   is saved on open (when the descriptor is a terminal) and restored on
   close unless the options disabled it. */
class tty_session final
    : private detail::unique_handle<tty_session, DT_TTYSession *, dt_tty_close>
{
  using handle_base
      = detail::unique_handle<tty_session, DT_TTYSession *, dt_tty_close>;

public:
  tty_session () noexcept = default;
  /* Adopt an existing session (transferred reference). */
  explicit tty_session (DT_TTYSession *raw) noexcept : handle_base (raw) {}

  static tty_session open (const tty_options &options)
  {
    DT_TTYOptions raw_options;
    dt_tty_options_init (&raw_options);
    raw_options.fd = options.fd;
    raw_options.take_ownership = options.take_ownership;
    raw_options.no_restore = options.no_restore;
    raw_options.nonblocking = options.nonblocking;
    DT_Error error_raw{};
    DT_TTYSession *raw_session = dt_tty_open (&raw_options, &error_raw);
    if (raw_session == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code
                                             : DT_ERR_INVALID_ARGUMENT,
                    error_raw);
    return tty_session (raw_session);
  }

  /* Borrowed descriptor for poll/select/epoll/kqueue. */
  int fd () const noexcept { return dt_tty_fd (native_handle ()); }
  int poll_fd () const noexcept { return dt_tty_pollfd (native_handle ()); }

  tty_settings mode () const
  {
    DT_TTYMode raw_mode{};
    DT_Error error_raw{};
    detail::check (dt_tty_get_mode (const_cast<DT_TTYSession *> (native_handle ()),
                                    &raw_mode, &error_raw),
                   error_raw);
    tty_settings out;
    out.canonical = raw_mode.canonical;
    out.echo = raw_mode.echo;
    out.signals = raw_mode.signals;
    out.input_processing = raw_mode.input_processing;
    out.output_processing = raw_mode.output_processing;
    out.read_timeout_ds = raw_mode.read_timeout_ds;
    return out;
  }

  void set_mode (const tty_settings &settings)
  {
    DT_TTYMode raw_mode{};
    raw_mode.canonical = settings.canonical;
    raw_mode.echo = settings.echo;
    raw_mode.signals = settings.signals;
    raw_mode.input_processing = settings.input_processing;
    raw_mode.output_processing = settings.output_processing;
    raw_mode.read_timeout_ds = settings.read_timeout_ds;
    DT_Error error_raw{};
    detail::check (dt_tty_set_mode (native_handle (), &raw_mode, &error_raw),
                   error_raw);
  }

  /* Raw: no processing, no echo, byte at a time. */
  void set_raw ()
  {
    DT_Error error_raw{};
    detail::check (dt_tty_set_raw (native_handle (), &error_raw), error_raw);
  }

  /* Cooked: canonical input, echo, signal generation, output mapping. */
  void set_cooked ()
  {
    DT_Error error_raw{};
    detail::check (dt_tty_set_cooked (native_handle (), &error_raw), error_raw);
  }

  /* Explicit termios save/restore; close restores automatically. */
  void save ()
  {
    DT_Error error_raw{};
    detail::check (dt_tty_save (native_handle (), &error_raw), error_raw);
  }

  void restore ()
  {
    DT_Error error_raw{};
    detail::check (dt_tty_restore (native_handle (), &error_raw), error_raw);
  }

  winsize size () const
  {
    DT_Winsize raw_size{};
    DT_Error error_raw{};
    detail::check (
        dt_tty_get_winsize (const_cast<DT_TTYSession *> (native_handle ()),
                            &raw_size, &error_raw),
        error_raw);
    return winsize{raw_size.rows, raw_size.columns, raw_size.xpixel,
                   raw_size.ypixel};
  }

  void set_size (const winsize &size)
  {
    const DT_Winsize raw_size{size.rows, size.columns, size.xpixel,
                              size.ypixel};
    DT_Error error_raw{};
    detail::check (dt_tty_set_winsize (native_handle (), &raw_size, &error_raw),
                   error_raw);
  }

  void set_nonblocking (bool nonblocking)
  {
    DT_Error error_raw{};
    detail::check (
        dt_tty_set_nonblocking (native_handle (), nonblocking, &error_raw),
        error_raw);
  }

  /* One read.  Partial reads are normal; end of stream, a VTIME timeout
     and would-block are reported through the result state. */
  transfer read (void *buffer, std::size_t capacity)
  {
    std::size_t moved = 0;
    DT_Error error_raw{};
    const DT_Status state = dt_tty_read (native_handle (), buffer, capacity,
                                         &moved, &error_raw);
    if (state != DT_OK)
      return transfer::failure (static_cast<status> (state),
                                detail::diagnose (state, error_raw));
    return transfer::success (moved);
  }

  /* Read into a fresh buffer; the bytes are copied out. */
  result<std::string> read_some (std::size_t capacity = 4096)
  {
    if (capacity == 0)
      {
        diagnostic info;
        info.state = status::invalid_argument;
        info.message = "read capacity must be positive";
        return result<std::string>::failure (status::invalid_argument,
                                             std::move (info));
      }
    std::string buffer (capacity, '\0');
    const transfer moved = read (buffer.data (), capacity);
    if (!moved)
      return result<std::string>::failure (moved.state (), moved.info ());
    buffer.resize (moved.value ());
    return result<std::string>::success (std::move (buffer));
  }

  /* One write.  Partial writes are normal; would-block and a closed
     peer are reported through the result state. */
  transfer write (const void *data, std::size_t size)
  {
    std::size_t moved = 0;
    DT_Error error_raw{};
    const DT_Status state = dt_tty_write (native_handle (), data, size,
                                          &moved, &error_raw);
    if (state != DT_OK)
      return transfer::failure (static_cast<status> (state),
                                detail::diagnose (state, error_raw));
    return transfer::success (moved);
  }

  transfer write (std::string_view text) { return write (text.data (), text.size ()); }

  transfer write (const std::vector<std::uint8_t> &bytes)
  {
    return write (bytes.data (), bytes.size ());
  }

  /* Loop until every byte is written.  Reports progress and state
     separately, so a partial write is never lost. */
  written write_all (std::string_view text)
  {
    std::size_t total = 0;
    while (total < text.size ())
      {
        const transfer moved
            = write (text.data () + total, text.size () - total);
        if (!moved)
          return written::from (moved.state (), moved.info (), total);
        total += moved.value ();
      }
    return written::from (status::ok, diagnostic{}, total);
  }

  using handle_base::native_handle;
  using handle_base::release;
  using handle_base::reset;
  using handle_base::valid;
};

/* ------------------------------------------------------------------ */
/* PTY sessions.                                                       */
/* ------------------------------------------------------------------ */

/* Mirrors DT_PTYOptions.  The strings and vectors are borrowed for the
   duration of spawn() only. */
struct pty_options
{
  std::string_view terminal_name;      /* Sets TERM in the child. */
  std::string_view working_directory; /* Empty: inherit. */
  std::vector<std::string_view> argv; /* argv[0] is required. */
  std::vector<std::string_view> environment; /* Empty: inherit environ. */
  unsigned short rows{24};
  unsigned short columns{80};
  bool nonblocking{false};   /* O_NONBLOCK on the master. */
  bool kill_on_close{true};  /* Terminate a live child when closing. */
  int kill_signal{SIGKILL};  /* Signal used when kill_on_close. */
};

/* Master descriptor and child pid.  The master stays owned by the
   session: do not close it. */
struct pty_info
{
  int master_fd{-1};
  int child_pid{-1};
};

/* How a child terminated.  `code` is the exit status for a normal exit
   and 128 + signal number for a signal death. */
struct child_exit
{
  int code{0};
  bool exited_normally{false};
};

/* Raw waitpid() status inspection. */
inline bool
wait_status_exited (int raw_status, int &code) noexcept
{
  return dt_pty_status_exited (raw_status, &code);
}

inline bool
wait_status_signaled (int raw_status, int &signal_number) noexcept
{
  return dt_pty_status_signaled (raw_status, &signal_number);
}

inline bool
wait_status_stopped (int raw_status, int &stop_signal) noexcept
{
  return dt_pty_status_stopped (raw_status, &stop_signal);
}

/* Owns a master/slave PTY pair and the child attached to it. */
class pty_session final
    : private detail::unique_handle<pty_session, DT_PTYSession *, dt_pty_close>
{
  using handle_base
      = detail::unique_handle<pty_session, DT_PTYSession *, dt_pty_close>;

public:
  pty_session () noexcept = default;
  /* Adopt an existing session (transferred reference). */
  explicit pty_session (DT_PTYSession *raw) noexcept : handle_base (raw) {}

  /* Create the pair, fork, make the child a session leader with the
     slave as controlling terminal, apply the window size, working
     directory and TERM, then exec argv.  Any failure before exec cleans
     up every descriptor and reaps the child; an exec failure is reported
     as status::system with the child's errno preserved. */
  static pty_session spawn (const pty_options &options)
  {
    if (options.argv.empty ())
      {
        diagnostic info;
        info.state = status::invalid_argument;
        info.message = "argv with argv[0] is required";
        throw error (std::move (info));
      }
    const std::string terminal_name
        = detail::c_string (options.terminal_name, "terminal name");
    const std::string working_directory
        = detail::c_string (options.working_directory, "working directory");
    const std::vector<char *> argv
        = detail::mutable_arguments (options.argv);
    const std::vector<char *> envp
        = detail::mutable_arguments (options.environment);

    DT_PTYOptions raw_options;
    dt_pty_options_init (&raw_options);
    raw_options.terminal_name = terminal_name.empty ()
                                     ? nullptr
                                     : terminal_name.c_str ();
    raw_options.working_directory
        = working_directory.empty () ? nullptr : working_directory.c_str ();
    raw_options.argv = argv.data ();
    raw_options.envp = envp.empty () ? nullptr : envp.data ();
    raw_options.rows = options.rows;
    raw_options.columns = options.columns;
    raw_options.nonblocking = options.nonblocking;
    raw_options.kill_on_close = options.kill_on_close;
    raw_options.kill_signal = options.kill_signal;

    DT_Error error_raw{};
    DT_PTYSession *raw_session = dt_pty_spawn (&raw_options, &error_raw);
    if (raw_session == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_SYSTEM,
                    error_raw);
    return pty_session (raw_session);
  }

  /* Borrowed master descriptor for poll/select/epoll/kqueue. */
  int fd () const noexcept { return dt_pty_pollfd (native_handle ()); }
  int poll_fd () const noexcept { return dt_pty_pollfd (native_handle ()); }

  pty_info info () const
  {
    DT_PTYInfo raw_info{};
    DT_Error error_raw{};
    detail::check (
        dt_pty_get_info (const_cast<DT_PTYSession *> (native_handle ()),
                         &raw_info, &error_raw),
        error_raw);
    pty_info out;
    out.master_fd = raw_info.master_fd;
    out.child_pid = raw_info.child_pid;
    return out;
  }

  void resize (unsigned short rows, unsigned short columns)
  {
    DT_Error error_raw{};
    detail::check (
        dt_pty_resize (native_handle (), rows, columns, &error_raw), error_raw);
  }

  winsize size () const
  {
    DT_Winsize raw_size{};
    DT_Error error_raw{};
    detail::check (
        dt_pty_get_winsize (const_cast<DT_PTYSession *> (native_handle ()),
                            &raw_size, &error_raw),
        error_raw);
    return winsize{raw_size.rows, raw_size.columns, raw_size.xpixel,
                   raw_size.ypixel};
  }

  void set_nonblocking (bool nonblocking)
  {
    DT_Error error_raw{};
    detail::check (
        dt_pty_set_nonblocking (native_handle (), nonblocking, &error_raw),
        error_raw);
  }

  transfer read (void *buffer, std::size_t capacity)
  {
    std::size_t moved = 0;
    DT_Error error_raw{};
    const DT_Status state = dt_pty_read (native_handle (), buffer, capacity,
                                         &moved, &error_raw);
    if (state != DT_OK)
      return transfer::failure (static_cast<status> (state),
                                detail::diagnose (state, error_raw));
    return transfer::success (moved);
  }

  /* Read into a fresh buffer; the bytes are copied out. */
  result<std::string> read_some (std::size_t capacity = 65536)
  {
    if (capacity == 0)
      {
        diagnostic info;
        info.state = status::invalid_argument;
        info.message = "read capacity must be positive";
        return result<std::string>::failure (status::invalid_argument,
                                             std::move (info));
      }
    std::string buffer (capacity, '\0');
    const transfer moved = read (buffer.data (), capacity);
    if (!moved)
      return result<std::string>::failure (moved.state (), moved.info ());
    buffer.resize (moved.value ());
    return result<std::string>::success (std::move (buffer));
  }

  transfer write (const void *data, std::size_t size)
  {
    std::size_t moved = 0;
    DT_Error error_raw{};
    const DT_Status state = dt_pty_write (native_handle (), data, size,
                                          &moved, &error_raw);
    if (state != DT_OK)
      return transfer::failure (static_cast<status> (state),
                                detail::diagnose (state, error_raw));
    return transfer::success (moved);
  }

  transfer write (std::string_view text)
  {
    return write (text.data (), text.size ());
  }

  transfer write (const std::vector<std::uint8_t> &bytes)
  {
    return write (bytes.data (), bytes.size ());
  }

  /* Loop until every byte is written.  Reports progress and state
     separately, so a partial write is never lost. */
  written write_all (std::string_view text)
  {
    std::size_t total = 0;
    while (total < text.size ())
      {
        const transfer moved
            = write (text.data () + total, text.size () - total);
        if (!moved)
          return written::from (moved.state (), moved.info (), total);
        total += moved.value ();
      }
    return written::from (status::ok, diagnostic{}, total);
  }

  /* Blocking reap.  Idempotent afterwards: the recorded status is
     returned again without touching the child.  Fails with
     status::child_exited when no child is attached. */
  child_exit wait ()
  {
    int code = 0;
    bool exited_normally = false;
    DT_Error error_raw{};
    detail::check (
        dt_pty_wait (native_handle (), &code, &exited_normally, &error_raw),
        error_raw);
    return child_exit{code, exited_normally};
  }

  /* Non-blocking reap: ok() with a value means the child was reaped;
     status::would_block means it is still running. */
  result<child_exit> poll ()
  {
    bool exited = false;
    int code = 0;
    bool exited_normally = false;
    DT_Error error_raw{};
    const DT_Status state = dt_pty_poll (native_handle (), &exited, &code,
                                         &exited_normally, &error_raw);
    if (state != DT_OK)
      return result<child_exit>::failure (static_cast<status> (state),
                                          detail::diagnose (state, error_raw));
    return result<child_exit>::success (child_exit{code, exited_normally});
  }

  /* Raw waitpid status (reaped at most once). */
  int wait_raw (bool blocking)
  {
    int raw_status = 0;
    DT_Error error_raw{};
    detail::check (
        dt_pty_wait_status (native_handle (), &raw_status, blocking,
                            &error_raw),
        error_raw);
    return raw_status;
  }

  void terminate (int signal_number)
  {
    DT_Error error_raw{};
    detail::check (
        dt_pty_terminate (native_handle (), signal_number, &error_raw),
        error_raw);
  }

  using handle_base::native_handle;
  using handle_base::release;
  using handle_base::reset;
  using handle_base::valid;
};

/* ------------------------------------------------------------------ */
/* Recording and replay.                                                */
/* ------------------------------------------------------------------ */

enum class record_type : std::uint8_t
{
  input = DT_RECORD_INPUT,
  output = DT_RECORD_OUTPUT,
  resize = DT_RECORD_RESIZE,
  meta = DT_RECORD_META,
  heartbeat = DT_RECORD_HEARTBEAT
};

inline const char *
to_string (record_type type) noexcept
{
  return dt_record_type_string (static_cast<DT_RecordType> (type));
}

/* Replay pacing. */
enum class replay_mode
{
  realtime, /* Honour the recorded timestamps. */
  fast,     /* Deliver as fast as possible (the default). */
  step,     /* Deliver one event per step() call. */
  paused    /* next() reports status::would_block. */
};

/* Renderers for a recording. */
enum class replay_target
{
  termscript,
  javascript,
  wasm,
  webgpu,
  manim,
  processing
};

/* One record.  `data` is owned by the event; `timestamp_ns` of 0 asks
   the recorder to stamp the event with the current CLOCK_REALTIME in
   nanoseconds since the Unix epoch. */
struct record_event
{
  record_type type{record_type::heartbeat};
  std::uint64_t timestamp_ns{0};
  std::vector<std::uint8_t> data;
  unsigned short rows{0};
  unsigned short columns{0};

  static record_event input (bytes_view payload,
                             std::uint64_t timestamp_ns = 0);
  static record_event output (bytes_view payload,
                              std::uint64_t timestamp_ns = 0);
  static record_event meta (std::string_view text,
                            std::uint64_t timestamp_ns = 0);
  static record_event resize_to (unsigned short rows, unsigned short columns,
                                 std::uint64_t timestamp_ns = 0);
  static record_event heartbeat_at (std::uint64_t timestamp_ns = 0);

  /* Payload as text (input, output and meta records). */
  std::string text () const
  {
    return std::string (reinterpret_cast<const char *> (data.data ()),
                        data.size ());
  }
};

inline record_event
record_event::input (bytes_view payload, std::uint64_t timestamp_ns)
{
  record_event event;
  event.type = record_type::input;
  event.timestamp_ns = timestamp_ns;
  event.data.assign (payload.begin (), payload.end ());
  return event;
}

inline record_event
record_event::output (bytes_view payload, std::uint64_t timestamp_ns)
{
  record_event event;
  event.type = record_type::output;
  event.timestamp_ns = timestamp_ns;
  event.data.assign (payload.begin (), payload.end ());
  return event;
}

inline record_event
record_event::meta (std::string_view text, std::uint64_t timestamp_ns)
{
  record_event event;
  event.type = record_type::meta;
  event.timestamp_ns = timestamp_ns;
  event.data.assign (text.begin (), text.end ());
  return event;
}

inline record_event
record_event::resize_to (unsigned short rows, unsigned short columns,
                         std::uint64_t timestamp_ns)
{
  record_event event;
  event.type = record_type::resize;
  event.timestamp_ns = timestamp_ns;
  event.rows = rows;
  event.columns = columns;
  return event;
}

inline record_event
record_event::heartbeat_at (std::uint64_t timestamp_ns)
{
  record_event event;
  event.type = record_type::heartbeat;
  event.timestamp_ns = timestamp_ns;
  return event;
}

/* Writes versioned session recordings (magic "DMTR", little-endian,
   CRC32 per record).  The stream is borrowed unless take_ownership is
   set, in which case it is fclose()d when the recorder is destroyed. */
class recorder final
    : private detail::unique_handle<recorder, DT_Recorder *, dt_recorder_free>
{
  using handle_base
      = detail::unique_handle<recorder, DT_Recorder *, dt_recorder_free>;

public:
  recorder () noexcept = default;
  /* Adopt an existing recorder (transferred reference). */
  explicit recorder (DT_Recorder *raw) noexcept : handle_base (raw) {}

  /* Create a recorder and write the file header to `stream`. */
  static recorder create (std::FILE *stream, bool take_ownership = false)
  {
    DT_Error error_raw{};
    DT_Recorder *raw_recorder
        = dt_recorder_create (stream, take_ownership, &error_raw);
    if (raw_recorder == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_IO,
                    error_raw);
    return recorder (raw_recorder);
  }

  /* Append one record.  The event payload is borrowed for the call and
     payloads above max_record_payload fail with status::limit. */
  void write (const record_event &event)
  {
    DT_RecordEvent raw_event{};
    raw_event.type = static_cast<DT_RecordType> (event.type);
    raw_event.timestamp_ns = event.timestamp_ns;
    raw_event.data = event.data.empty () ? nullptr : event.data.data ();
    raw_event.size = event.data.size ();
    raw_event.rows = event.rows;
    raw_event.columns = event.columns;
    DT_Error error_raw{};
    detail::check (
        dt_recorder_write (native_handle (), &raw_event, &error_raw), error_raw);
  }

  void write_input (bytes_view payload, std::uint64_t timestamp_ns = 0)
  {
    write (record_event::input (payload, timestamp_ns));
  }

  void write_output (bytes_view payload, std::uint64_t timestamp_ns = 0)
  {
    write (record_event::output (payload, timestamp_ns));
  }

  void write_meta (std::string_view text, std::uint64_t timestamp_ns = 0)
  {
    write (record_event::meta (text, timestamp_ns));
  }

  void write_resize (unsigned short rows, unsigned short columns,
                     std::uint64_t timestamp_ns = 0)
  {
    write (record_event::resize_to (rows, columns, timestamp_ns));
  }

  void write_heartbeat (std::uint64_t timestamp_ns = 0)
  {
    write (record_event::heartbeat_at (timestamp_ns));
  }

  void flush ()
  {
    DT_Error error_raw{};
    detail::check (dt_recorder_flush (native_handle (), &error_raw),
                   error_raw);
  }

  using handle_base::native_handle;
  using handle_base::release;
  using handle_base::reset;
  using handle_base::valid;
};

/* Reads versioned session recordings with deterministic pacing.  The
   stream is borrowed unless take_ownership is set.  Events returned by
   next()/step() own their payload, so they stay valid until destroyed. */
class replayer final
    : private detail::unique_handle<replayer, DT_Replayer *, dt_replayer_free>
{
  using handle_base
      = detail::unique_handle<replayer, DT_Replayer *, dt_replayer_free>;

public:
  replayer () noexcept = default;
  /* Adopt an existing replayer (transferred reference). */
  explicit replayer (DT_Replayer *raw) noexcept : handle_base (raw) {}

  /* Open a recording.  Fails with status::protocol on a bad magic or
     reserved header flags and status::unsupported on a foreign format
     version. */
  static replayer create (std::FILE *stream, bool take_ownership = false)
  {
    DT_Error error_raw{};
    DT_Replayer *raw_replayer
        = dt_replayer_create (stream, take_ownership, &error_raw);
    if (raw_replayer == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_PROTOCOL,
                    error_raw);
    return replayer (raw_replayer);
  }

  replay_mode mode () const noexcept
  {
    switch (dt_replayer_get_mode (native_handle ()))
      {
      case DT_REPLAY_REALTIME:
        return replay_mode::realtime;
      case DT_REPLAY_STEP:
        return replay_mode::step;
      case DT_REPLAY_PAUSED:
        return replay_mode::paused;
      case DT_REPLAY_FAST:
      default:
        return replay_mode::fast;
      }
  }

  void set_mode (replay_mode mode) noexcept
  {
    DT_ReplayMode raw_mode = DT_REPLAY_FAST;
    switch (mode)
      {
      case replay_mode::realtime:
        raw_mode = DT_REPLAY_REALTIME;
        break;
      case replay_mode::step:
        raw_mode = DT_REPLAY_STEP;
        break;
      case replay_mode::paused:
        raw_mode = DT_REPLAY_PAUSED;
        break;
      case replay_mode::fast:
        raw_mode = DT_REPLAY_FAST;
        break;
      }
    dt_replayer_set_mode (native_handle (), raw_mode);
  }

  /* Speed multiplier for replay_mode::realtime (> 0, 1 = recorded pace). */
  void set_speed (double speed) noexcept
  {
    dt_replayer_set_speed (native_handle (), speed);
  }

  void pause () noexcept { dt_replayer_pause (native_handle ()); }

  /* Resume the mode that was active before pause(). */
  void resume () noexcept { dt_replayer_resume (native_handle ()); }

  /* Deliver the next event.  status::eof ends the recording,
     status::would_block means "paused" or "step mode", and a corrupt
     record fails with status::protocol.  Unknown informational record
     types are skipped for forward compatibility. */
  result<record_event> next ()
  {
    DT_RecordEvent raw_event{};
    DT_Error error_raw{};
    const DT_Status state
        = dt_replayer_next (native_handle (), &raw_event, &error_raw);
    if (state != DT_OK)
      return result<record_event>::failure (static_cast<status> (state),
                                            detail::diagnose (state, error_raw));
    return result<record_event>::success (adopt (raw_event));
  }

  /* Advance exactly one event (same contract as next()). */
  result<record_event> step ()
  {
    DT_RecordEvent raw_event{};
    DT_Error error_raw{};
    const DT_Status state
        = dt_replayer_step (native_handle (), &raw_event, &error_raw);
    if (state != DT_OK)
      return result<record_event>::failure (static_cast<status> (state),
                                            detail::diagnose (state, error_raw));
    return result<record_event>::success (adopt (raw_event));
  }

  /* Rewind to the first record (requires a seekable stream). */
  void rewind ()
  {
    DT_Error error_raw{};
    detail::check (dt_replayer_rewind (native_handle (), &error_raw),
                   error_raw);
  }

  /* Render the remaining recording as a replay program.  Drains the
     stream to the end of file, so call it after replaying.  `tmplate`
     may contain a {{events}} placeholder. */
  std::string compile (std::string_view tmplate, replay_target target)
  {
    const char *code = dt_replayer_compile (
        native_handle (),
        tmplate.empty () ? nullptr
                         : detail::c_string (tmplate, "template").c_str (),
        static_cast<DT_ReplayerTarget> (target));
    if (code == nullptr)
      {
        diagnostic info;
        info.state = status::protocol;
        info.message = "cannot render the recording for this target";
        throw error (std::move (info));
      }
    return std::string (code);
  }

private:
  /* Copy the borrowed C event payload into an owning event. */
  static record_event
  adopt (const DT_RecordEvent &raw)
  {
    record_event event;
    event.type = static_cast<record_type> (raw.type);
    event.timestamp_ns = raw.timestamp_ns;
    event.rows = raw.rows;
    event.columns = raw.columns;
    if (raw.size != 0 && raw.data != nullptr)
      event.data.assign (raw.data, raw.data + raw.size);
    return event;
  }

  using handle_base::native_handle;
  using handle_base::release;
  using handle_base::valid;
};

/* ------------------------------------------------------------------ */
/* Terminal orchestration.                                              */
/* ------------------------------------------------------------------ */

/* Mirrors DT_PuppeteerOptions.  `terminal_name` selects the Terminfo
   profile to load for the session; `recorder_sink` is borrowed and must
   outlive the puppeteer. */
struct puppeteer_options
{
  pty_options pty;
  std::string_view terminal_name;
  bool record{false};
  recorder *recorder_sink{nullptr};
};

/* Owns a PTY session, an optional Terminfo profile and (borrowed)
   recording.  Closing it closes the PTY, which terminates and reaps a
   still-running child. */
class puppeteer final
    : private detail::unique_handle<puppeteer, DT_Puppeteer *,
                                    dt_puppeteer_free>
{
  using handle_base
      = detail::unique_handle<puppeteer, DT_Puppeteer *, dt_puppeteer_free>;

public:
  puppeteer () noexcept = default;
  /* Adopt an existing puppeteer (transferred reference). */
  explicit puppeteer (DT_Puppeteer *raw) noexcept : handle_base (raw) {}

  /* Spawn the child described by `options` and load the matching
     Terminfo profile.  `options.recorder_sink` is borrowed: it must
     outlive this puppeteer.  When options.pty.terminal_name is empty
     the session terminal name is used for TERM in the child. */
  static puppeteer create (const puppeteer_options &options)
  {
    const std::string terminal_name
        = detail::c_string (options.terminal_name, "terminal name");
    const std::string child_terminal
        = options.pty.terminal_name.empty ()
              ? terminal_name
              : detail::c_string (options.pty.terminal_name, "terminal name");
    const std::string working_directory = detail::c_string (
        options.pty.working_directory, "working directory");
    const std::vector<char *> argv
        = detail::mutable_arguments (options.pty.argv);
    const std::vector<char *> envp
        = detail::mutable_arguments (options.pty.environment);

    DT_PuppeteerOptions raw_options;
    dt_puppeteer_options_init (&raw_options);
    raw_options.terminal_name
        = terminal_name.empty () ? nullptr : terminal_name.c_str ();
    raw_options.pty.terminal_name
        = child_terminal.empty () ? nullptr : child_terminal.c_str ();
    raw_options.pty.working_directory
        = working_directory.empty () ? nullptr : working_directory.c_str ();
    raw_options.pty.argv = argv.data ();
    raw_options.pty.envp = envp.empty () ? nullptr : envp.data ();
    raw_options.pty.rows = options.pty.rows;
    raw_options.pty.columns = options.pty.columns;
    raw_options.pty.nonblocking = options.pty.nonblocking;
    raw_options.pty.kill_on_close = options.pty.kill_on_close;
    raw_options.pty.kill_signal = options.pty.kill_signal;
    raw_options.record = options.record;
    raw_options.recorder = options.recorder_sink != nullptr
                               ? options.recorder_sink->native_handle ()
                               : nullptr;

    DT_Error error_raw{};
    DT_Puppeteer *raw_puppeteer
        = dt_puppeteer_create (&raw_options, &error_raw);
    if (raw_puppeteer == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_SYSTEM,
                    error_raw);
    puppeteer out (raw_puppeteer);
    out.recorder_ = options.recorder_sink;
    return out;
  }

  /* Move one chunk of PTY output into the recorder as an OUTPUT event.
     timeout_ms < 0 blocks, 0 polls, > 0 waits.  status::timeout means
     "idle" and status::eof means the child side closed.  The chunk
     itself is consumed by the recorder; use read() to see the bytes. */
  result<void> pump_once (int timeout_ms)
  {
    DT_Error error_raw{};
    const DT_Status state = dt_puppeteer_pump_once (
        native_handle (), timeout_ms, &error_raw);
    if (state != DT_OK)
      return result<void>::failure (static_cast<status> (state),
                                    detail::diagnose (state, error_raw));
    return result<void>::success ();
  }

  /* Resize the PTY and record a RESIZE event. */
  void resize (unsigned short rows, unsigned short columns)
  {
    DT_Error error_raw{};
    detail::check (dt_puppeteer_resize (native_handle (), rows, columns,
                                        &error_raw),
                   error_raw);
  }

  /* Async-signal-safe and thread-safe: queue a resize that the next
     pump applies.  Safe to call from a SIGWINCH handler. */
  void
  request_resize (unsigned short rows, unsigned short columns) noexcept
  {
    dt_puppeteer_request_resize (native_handle (), rows, columns);
  }

  child_exit wait ()
  {
    int code = 0;
    bool exited_normally = false;
    DT_Error error_raw{};
    detail::check (dt_puppeteer_wait (native_handle (), &code,
                                      &exited_normally, &error_raw),
                   error_raw);
    return child_exit{code, exited_normally};
  }

  /* Borrowed descriptors of the owned PTY, for readiness integration. */
  int fd () const noexcept { return dt_pty_pollfd (dt_puppeteer_pty (native_handle ())); }
  int poll_fd () const noexcept { return fd (); }

  /* Borrowed Terminfo profile; empty when no terminal name was given. */
  profile_view terminfo () const noexcept
  {
    return profile_view (dt_puppeteer_profile (native_handle ()));
  }

  /* Borrowed recorder; null unless recording was requested. */
  recorder *recording () const noexcept { return recorder_; }

  /* Pass-through to the owned PTY. */
  transfer read (void *buffer, std::size_t capacity)
  {
    DT_PTYSession *raw = dt_puppeteer_pty (native_handle ());
    std::size_t moved = 0;
    DT_Error error_raw{};
    const DT_Status state
        = dt_pty_read (raw, buffer, capacity, &moved, &error_raw);
    if (state != DT_OK)
      return transfer::failure (static_cast<status> (state),
                                detail::diagnose (state, error_raw));
    return transfer::success (moved);
  }

  result<std::string> read_some (std::size_t capacity = 65536)
  {
    if (capacity == 0)
      {
        diagnostic info;
        info.state = status::invalid_argument;
        info.message = "read capacity must be positive";
        return result<std::string>::failure (status::invalid_argument,
                                             std::move (info));
      }
    std::string buffer (capacity, '\0');
    const transfer moved = read (buffer.data (), capacity);
    if (!moved)
      return result<std::string>::failure (moved.state (), moved.info ());
    buffer.resize (moved.value ());
    return result<std::string>::success (std::move (buffer));
  }

  transfer write (std::string_view text)
  {
    DT_PTYSession *raw = dt_puppeteer_pty (native_handle ());
    std::size_t moved = 0;
    DT_Error error_raw{};
    const DT_Status state = dt_pty_write (raw, text.data (), text.size (),
                                          &moved, &error_raw);
    if (state != DT_OK)
      return transfer::failure (static_cast<status> (state),
                                detail::diagnose (state, error_raw));
    return transfer::success (moved);
  }

  written write_all (std::string_view text)
  {
    std::size_t total = 0;
    while (total < text.size ())
      {
        const transfer moved = write (text.substr (total));
        if (!moved)
          return written::from (moved.state (), moved.info (), total);
        total += moved.value ();
      }
    return written::from (status::ok, diagnostic{}, total);
  }

  result<child_exit> poll ()
  {
    DT_PTYSession *raw = dt_puppeteer_pty (native_handle ());
    bool exited = false;
    int code = 0;
    bool exited_normally = false;
    DT_Error error_raw{};
    const DT_Status state = dt_pty_poll (raw, &exited, &code, &exited_normally,
                                         &error_raw);
    if (state != DT_OK)
      return result<child_exit>::failure (static_cast<status> (state),
                                          detail::diagnose (state, error_raw));
    return result<child_exit>::success (child_exit{code, exited_normally});
  }

  using handle_base::native_handle;
  using handle_base::release;
  using handle_base::reset;
  using handle_base::valid;

private:
  /* Borrowed recorder supplied through the options. */
  recorder *recorder_{};
};

/* ------------------------------------------------------------------ */
/* Messages, envelopes and connections.                                 */
/* ------------------------------------------------------------------ */

enum class message_type
{
  data = DT_MESSAGE_DATA,
  resize = DT_MESSAGE_RESIZE,
  close = DT_MESSAGE_CLOSE,
  error = DT_MESSAGE_ERROR,
  heartbeat = DT_MESSAGE_HEARTBEAT,
  auth = DT_MESSAGE_AUTH
};

inline const char *
to_string (message_type type) noexcept
{
  return dt_message_type_string (static_cast<DT_MessageType> (type));
}

enum class connection_state
{
  closed = DT_CONN_CLOSED,
  connecting = DT_CONN_CONNECTING,
  open = DT_CONN_OPEN,
  shutdown = DT_CONN_SHUTDOWN
};

inline const char *
to_string (connection_state state) noexcept
{
  return dt_conn_state_string (static_cast<DT_ConnState> (state));
}

/* Non-owning view over a message.  The payload borrows the message. */
class message_view
{
public:
  message_view () noexcept = default;
  explicit message_view (DT_Message *raw) noexcept : raw_ (raw) {}

  message_type type () const noexcept
  {
    return raw_ != nullptr
               ? static_cast<message_type> (dt_message_type (raw_))
               : message_type::error;
  }

  /* Borrowed payload (empty for resize and heartbeat messages). */
  bytes_view payload () const noexcept
  {
    std::size_t size = 0;
    const std::uint8_t *data = dt_message_data (raw_, &size);
    return bytes_view (data, size);
  }

  /* Payload as text. */
  std::string text () const { return payload ().to_string (); }

  /* Terminal size for resize messages; nullopt for anything else. */
  std::optional<winsize> resize () const noexcept
  {
    unsigned short rows = 0;
    unsigned short columns = 0;
    if (dt_message_resize (raw_, &rows, &columns) != DT_OK)
      return std::nullopt;
    return winsize{rows, columns, 0, 0};
  }

  bool valid () const noexcept { return raw_ != nullptr; }
  DT_Message *native_handle () const noexcept { return raw_; }

protected:
  DT_Message *raw_{};
};

/* Owns a message and its payload. */
class message final : public message_view,
                       private detail::unique_handle<message, DT_Message *,
                                                    dt_message_free>
{
  using handle_base
      = detail::unique_handle<message, DT_Message *, dt_message_free>;

public:
  message () noexcept = default;
  /* Adopt an existing message (transferred reference). */
  explicit message (DT_Message *raw) noexcept : message_view (raw),
                                                handle_base (raw)
  {
  }

  static message make_data (bytes_view payload)
  {
    DT_Error error_raw{};
    DT_Message *raw_message = dt_message_create (
        DT_MESSAGE_DATA, payload.data (), payload.size (), &error_raw);
    if (raw_message == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code
                                             : DT_ERR_INVALID_ARGUMENT,
                    error_raw);
    return message (raw_message);
  }

  static message make_data (std::string_view text)
  {
    return make_data (bytes_view (text));
  }

  static message make_resize (unsigned short rows, unsigned short columns)
  {
    DT_Error error_raw{};
    DT_Message *raw_message
        = dt_message_create_resize (rows, columns, &error_raw);
    if (raw_message == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_SYSTEM,
                    error_raw);
    return message (raw_message);
  }

  static message make_resize (const winsize &size)
  {
    return make_resize (size.rows, size.columns);
  }

  static message make_close ()
  {
    DT_Error error_raw{};
    DT_Message *raw_message
        = dt_message_create (DT_MESSAGE_CLOSE, nullptr, 0, &error_raw);
    if (raw_message == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code
                                             : DT_ERR_INVALID_ARGUMENT,
                    error_raw);
    return message (raw_message);
  }

  static message make_heartbeat ()
  {
    DT_Error error_raw{};
    DT_Message *raw_message = dt_message_create_heartbeat (&error_raw);
    if (raw_message == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_SYSTEM,
                    error_raw);
    return message (raw_message);
  }

  static message
  make_error (int code, std::string_view text)
  {
    const std::string owned = detail::c_string (text, "error text");
    DT_Error error_raw{};
    DT_Message *raw_message
        = dt_message_create_error_msg (code, owned.c_str (), &error_raw);
    if (raw_message == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_SYSTEM,
                    error_raw);
    return message (raw_message);
  }

  using handle_base::native_handle;
  using handle_base::release;
  using handle_base::reset;
  using handle_base::valid;
};

/* A message plus its 64-bit identifier: the unit of transport framing. */
class envelope final
    : private detail::unique_handle<envelope, DT_Envelope *, dt_envelope_free>
{
  using handle_base
      = detail::unique_handle<envelope, DT_Envelope *, dt_envelope_free>;

public:
  envelope () noexcept = default;
  /* Adopt an existing envelope (transferred reference). */
  explicit envelope (DT_Envelope *raw) noexcept : handle_base (raw) {}

  /* Wrap `payload`, taking ownership of it. */
  envelope (std::uint64_t message_id, message payload)
  {
    DT_Error error_raw{};
    DT_Envelope *raw_envelope = dt_envelope_create (
        message_id, payload.native_handle (), &error_raw);
    if (raw_envelope == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code
                                             : DT_ERR_INVALID_ARGUMENT,
                    error_raw);
    payload.release (); /* Ownership moved into the envelope. */
    reset (raw_envelope);
  }

  static envelope make_data (std::uint64_t message_id, bytes_view payload)
  {
    return envelope (message_id, message::make_data (payload));
  }

  static envelope make_data (std::uint64_t message_id, std::string_view text)
  {
    return envelope (message_id, message::make_data (text));
  }

  static envelope
  make_resize (std::uint64_t message_id, unsigned short rows,
               unsigned short columns)
  {
    return envelope (message_id, message::make_resize (rows, columns));
  }

  static envelope make_close (std::uint64_t message_id)
  {
    return envelope (message_id, message::make_close ());
  }

  static envelope make_heartbeat (std::uint64_t message_id)
  {
    return envelope (message_id, message::make_heartbeat ());
  }

  static envelope
  make_error (std::uint64_t message_id, int code, std::string_view text)
  {
    return envelope (message_id, message::make_error (code, text));
  }

  std::uint64_t id () const noexcept
  {
    return dt_envelope_message_id (native_handle ());
  }

  /* Borrowed message; valid while this envelope lives. */
  message_view body () const noexcept
  {
    return message_view (dt_envelope_message (native_handle ()));
  }

  /* Transfer the message out and destroy the envelope; *this is left
     empty. */
  message take_body () noexcept
  {
    DT_Envelope *raw = release ();
    if (raw == nullptr)
      return message ();
    return message (dt_envelope_take_message (raw));
  }

  using handle_base::native_handle;
  using handle_base::release;
  using handle_base::reset;
  using handle_base::valid;
};

/* Transport-neutral connection interface shared by the local and remote
   transports.  Implementations are move-only and are not thread-safe
   except for cancel(). */
class connection
{
public:
  virtual ~connection () = default;

  /* Borrowed descriptor for poll/select/epoll/kqueue (-1 when closed). */
  virtual int fd () const noexcept = 0;
  virtual connection_state state () const noexcept = 0;

  /* Frame and send one envelope.  Partial sends are handled internally;
     the caller sees success, a normal condition or a failure. */
  virtual result<void> send (const envelope &message) = 0;
  /* Receive one whole envelope.  status::eof is an orderly peer
     shutdown; status::timeout/would_block mean "nothing yet"; a
     corrupt or unknown frame fails with status::protocol. */
  virtual result<envelope> receive () = 0;

  /* Thread-safe: unblock a thread waiting in receive()/send(). */
  virtual void cancel () noexcept = 0;

  /* Send a heartbeat with the given message id. */
  virtual void ping (std::uint64_t message_id) = 0;
};

/* Mirrors DT_LocalConnOptions.  The descriptor must already be connected
   (socketpair, pipe or socket). */
struct local_options
{
  int fd{-1};
  bool take_ownership{false};
  bool nonblocking{false};
  /* < 0 blocks forever, 0 polls, > 0 waits. */
  int read_timeout_ms{-1};
  int write_timeout_ms{-1};
};

/* Wraps an existing connected stream descriptor. */
class local_connection final : public connection,
                                private detail::unique_handle<local_connection,
                                                             DT_LocalConn *,
                                                             dt_localconn_close>
{
  using handle_base = detail::unique_handle<local_connection, DT_LocalConn *,
                                            dt_localconn_close>;

public:
  local_connection () noexcept = default;
  /* Adopt an existing connection (transferred reference). */
  explicit local_connection (DT_LocalConn *raw) noexcept : handle_base (raw) {}

  static local_connection open (const local_options &options)
  {
    DT_LocalConnOptions raw_options;
    dt_localconn_options_init (&raw_options);
    raw_options.fd = options.fd;
    raw_options.take_ownership = options.take_ownership;
    raw_options.nonblocking = options.nonblocking;
    raw_options.read_timeout_ms = options.read_timeout_ms;
    raw_options.write_timeout_ms = options.write_timeout_ms;
    DT_Error error_raw{};
    DT_LocalConn *raw_connection
        = dt_localconn_open (&raw_options, &error_raw);
    if (raw_connection == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code
                                             : DT_ERR_INVALID_ARGUMENT,
                    error_raw);
    return local_connection (raw_connection);
  }

  int fd () const noexcept override
  {
    return dt_localconn_fd (native_handle ());
  }

  connection_state state () const noexcept override
  {
    return static_cast<connection_state> (dt_localconn_state (native_handle ()));
  }

  result<void> send (const envelope &message) override
  {
    DT_Error error_raw{};
    const DT_Status state = dt_localconn_send (native_handle (),
                                               message.native_handle (),
                                               &error_raw);
    if (state != DT_OK)
      return result<void>::failure (static_cast<status> (state),
                                    detail::diagnose (state, error_raw));
    return result<void>::success ();
  }

  result<envelope> receive () override
  {
    DT_Envelope *raw_envelope = nullptr;
    DT_Error error_raw{};
    const DT_Status state = dt_localconn_receive (
        native_handle (), &raw_envelope, &error_raw);
    if (state != DT_OK)
      return result<envelope>::failure (static_cast<status> (state),
                                        detail::diagnose (state, error_raw));
    return result<envelope>::success (envelope (raw_envelope));
  }

  void cancel () noexcept override { dt_localconn_cancel (native_handle ()); }

  void ping (std::uint64_t message_id) override
  {
    DT_Error error_raw{};
    detail::check (
        dt_localconn_ping (native_handle (), message_id, &error_raw), error_raw);
  }

  using handle_base::native_handle;
  using handle_base::release;
  using handle_base::reset;
  using handle_base::valid;
};

/* Mirrors DT_RemoteConnOptions.  SECURITY MODEL: the transport is raw
   TCP with no encryption, no integrity beyond the framing CRC, no
   peer-credential verification and no replay protection.  `auth_token`
   is a bearer secret sent in the clear.  Never use this over an
   untrusted network without an external secure tunnel. */
struct remote_options
{
  std::string_view host; /* e.g. "127.0.0.1". */
  std::uint16_t port{0};
  unsigned int connect_timeout_ms{5000};
  std::string_view auth_token; /* Borrowed bearer secret, empty = none. */
  int read_timeout_ms{-1};
  int write_timeout_ms{-1};
  int keepalive_ms{0}; /* Reserved; the application drives pings. */
};

/* TCP client with a version/authentication handshake
   ("DT/1.0 HELLO proto=1 [token=...]" then OK / DENIED /
   VERSION-MISMATCH).  status::auth means the peer rejected the
   credentials; status::protocol means a version or framing problem. */
class remote_connection final
    : public connection,
      private detail::unique_handle<remote_connection, DT_RemoteConn *,
                                   dt_remoteconn_close>
{
  using handle_base = detail::unique_handle<remote_connection, DT_RemoteConn *,
                                            dt_remoteconn_close>;

public:
  remote_connection () noexcept = default;
  /* Adopt an existing connection (transferred reference). */
  explicit remote_connection (DT_RemoteConn *raw) noexcept : handle_base (raw)
  {
  }

  static remote_connection open (const remote_options &options)
  {
    const std::string host = detail::c_string (options.host, "host");
    const std::string token
        = detail::c_string (options.auth_token, "auth token");
    DT_RemoteConnOptions raw_options;
    dt_remoteconn_options_init (&raw_options);
    raw_options.host = host.c_str ();
    raw_options.port = options.port;
    raw_options.connect_timeout_ms = options.connect_timeout_ms;
    raw_options.auth_token = token.empty () ? nullptr : token.c_str ();
    raw_options.read_timeout_ms = options.read_timeout_ms;
    raw_options.write_timeout_ms = options.write_timeout_ms;
    raw_options.keepalive_ms = options.keepalive_ms;
    DT_Error error_raw{};
    DT_RemoteConn *raw_connection
        = dt_remoteconn_open (&raw_options, &error_raw);
    if (raw_connection == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_SYSTEM,
                    error_raw);
    return remote_connection (raw_connection);
  }

  int fd () const noexcept override
  {
    return dt_remoteconn_fd (native_handle ());
  }

  connection_state state () const noexcept override
  {
    return static_cast<connection_state> (
        dt_remoteconn_state (native_handle ()));
  }

  result<void> send (const envelope &message) override
  {
    DT_Error error_raw{};
    const DT_Status state = dt_remoteconn_send (native_handle (),
                                                 message.native_handle (),
                                                 &error_raw);
    if (state != DT_OK)
      return result<void>::failure (static_cast<status> (state),
                                    detail::diagnose (state, error_raw));
    return result<void>::success ();
  }

  result<envelope> receive () override
  {
    DT_Envelope *raw_envelope = nullptr;
    DT_Error error_raw{};
    const DT_Status state = dt_remoteconn_receive (
        native_handle (), &raw_envelope, &error_raw);
    if (state != DT_OK)
      return result<envelope>::failure (static_cast<status> (state),
                                        detail::diagnose (state, error_raw));
    return result<envelope>::success (envelope (raw_envelope));
  }

  void cancel () noexcept override
  {
    dt_remoteconn_cancel (native_handle ());
  }

  void ping (std::uint64_t message_id) override
  {
    DT_Error error_raw{};
    detail::check (
        dt_remoteconn_ping (native_handle (), message_id, &error_raw),
        error_raw);
  }

  using handle_base::native_handle;
  using handle_base::release;
  using handle_base::reset;
  using handle_base::valid;
};

/* ------------------------------------------------------------------ */
/* Termscript: the automation language substrate.                      */
/* ------------------------------------------------------------------ */

enum class term_value_type
{
  nil = DT_TERM_NIL,
  boolean = DT_TERM_BOOL,
  integer = DT_TERM_INT,
  text = DT_TERM_STRING,
  handle = DT_TERM_HANDLE
};

inline const char *
to_string (term_value_type type) noexcept
{
  switch (type)
    {
    case term_value_type::nil:
      return "nil";
    case term_value_type::boolean:
      return "boolean";
    case term_value_type::integer:
      return "integer";
    case term_value_type::text:
      return "text";
    case term_value_type::handle:
      return "handle";
    }
  return "unknown";
}

/* A Termscript value.  Text values own their buffer; handles are
   retained references released by the library. */
class term_value final
    : private detail::unique_handle<term_value, DT_TermValue *,
                                    dt_termvalue_free>
{
  using handle_base = detail::unique_handle<term_value, DT_TermValue *,
                                            dt_termvalue_free>;

  friend class script_vm;

public:
  term_value () noexcept : handle_base (raw_storage ()) {}
  /* Adopt an existing value (transferred reference). */
  explicit term_value (DT_TermValue *raw) noexcept : handle_base (raw) {}

  static term_value nil () noexcept { return term_value (); }

  static term_value boolean (bool value) noexcept
  {
    return term_value (make (value));
  }

  static term_value integer (std::int64_t value) noexcept
  {
    return term_value (make (value));
  }

  /* Copies the text. */
  static term_value text (std::string_view value)
  {
    DT_Error error_raw{};
    DT_TermValue *raw = raw_storage ();
    const DT_Status state
        = dt_rt_mkstring (raw, detail::c_string (value, "value").c_str (),
                          &error_raw);
    if (state != DT_OK)
      detail::fail (state, error_raw);
    return term_value (raw);
  }

  /* Borrow an opaque native handle (e.g. a terminfo profile). */
  static term_value handle (void *opaque) noexcept
  {
    return term_value (make (opaque));
  }

  term_value_type type () const noexcept
  {
    const DT_TermValue *raw = native_handle ();
    return raw != nullptr ? static_cast<term_value_type> (raw->type)
                           : term_value_type::nil;
  }

  bool as_boolean () const noexcept
  {
    const DT_TermValue *raw = native_handle ();
    return raw != nullptr && raw->type == DT_TERM_BOOL && raw->as.boolean;
  }

  std::int64_t as_integer () const noexcept
  {
    const DT_TermValue *raw = native_handle ();
    return raw != nullptr && raw->type == DT_TERM_INT ? raw->as.integer : 0;
  }

  std::string_view as_text () const noexcept
  {
    const DT_TermValue *raw = native_handle ();
    if (raw == nullptr || raw->type != DT_TERM_STRING
        || raw->as.string == nullptr)
      return std::string_view ();
    return std::string_view (raw->as.string);
  }

  void *as_handle () const noexcept
  {
    const DT_TermValue *raw = native_handle ();
    return raw != nullptr && raw->type == DT_TERM_HANDLE ? raw->as.handle
                                                          : nullptr;
  }

  /* Rendering used by the language runtime (numbers, text, booleans). */
  std::string to_string () const
  {
    char *text = nullptr;
    DT_Error error_raw{};
    const DT_Status state
        = dt_termvalue_to_string (native_handle (), &text, &error_raw);
    if (state != DT_OK)
      detail::fail (state, error_raw);
    std::string out = text != nullptr ? std::string (text) : std::string ();
    std::free (text);
    return out;
  }

  /* Termscript truthiness: nil is false, 0 is false, "" is false. */
  bool truthy () const noexcept { return dt_rt_truthy (native_handle ()); }

  /* Borrowed native value for the dt_rt_* runtime. */
  DT_TermValue *native_handle () const noexcept
  {
    return handle_base::native_handle ();
  }

  using handle_base::release;
  using handle_base::reset;
  using handle_base::valid;

private:
  /* Heap storage for values that do not come from the C API. */
  static DT_TermValue *raw_storage ()
  {
    DT_TermValue *raw
        = static_cast<DT_TermValue *> (std::calloc (1, sizeof (DT_TermValue)));
    if (raw != nullptr)
      raw->type = DT_TERM_NIL;
    return raw;
  }

  static DT_TermValue *make (bool value) noexcept
  {
    DT_TermValue *raw = raw_storage ();
    if (raw != nullptr)
      {
        raw->type = DT_TERM_BOOL;
        raw->as.boolean = value;
      }
    return raw;
  }

  static DT_TermValue *make (std::int64_t value) noexcept
  {
    DT_TermValue *raw = raw_storage ();
    if (raw != nullptr)
      {
        raw->type = DT_TERM_INT;
        raw->as.integer = value;
      }
    return raw;
  }

  static DT_TermValue *make (void *opaque) noexcept
  {
    DT_TermValue *raw = raw_storage ();
    if (raw != nullptr)
      {
        raw->type = DT_TERM_HANDLE;
        raw->as.handle = opaque;
      }
    return raw;
  }
};

/* One entry of a native module function table. */
struct term_function
{
  std::string_view name;
  DT_TermFunc function{nullptr};
};

/* A native Termscript module.  The registry keeps a reference to the
   table, so the module object must outlive every VM that uses it and
   must not be moved or modified after registration. */
class term_module
{
public:
  term_module (std::string_view name, std::vector<term_function> functions)
  {
    name_ = detail::c_string (name, "module name");
    /* Names are copied first so that the pointers stored in the table
       stay valid for the lifetime of this object. */
    names_.reserve (functions.size ());
    for (const term_function &entry : functions)
      names_.push_back (entry.name.empty ()
                            ? std::string ()
                            : detail::c_string (entry.name, "function name"));
    table_.reserve (functions.size () + 1);
    for (std::size_t i = 0; i < functions.size (); ++i)
      {
        DT_TermFuncDef definition{};
        definition.name
            = names_[i].empty () ? nullptr : names_[i].c_str ();
        definition.func = functions[i].function;
        table_.push_back (definition);
      }
    DT_TermFuncDef terminator{};
    terminator.name = nullptr;
    terminator.func = nullptr;
    table_.push_back (terminator);
    native_.name = name_.c_str ();
    native_.funcs = table_.data ();
  }

  /* Register the module.  Thread-safe; duplicate names fail. */
  void register_module () const
  {
    DT_Error error_raw{};
    detail::check (dt_termscript_register_module (&native_, &error_raw),
                   error_raw);
  }

  const std::string &name () const noexcept { return name_; }
  const DT_TermModule *native () const noexcept { return &native_; }

private:
  std::string name_;
  std::vector<std::string> names_;
  std::vector<DT_TermFuncDef> table_;
  DT_TermModule native_{};
};

/* A Termscript virtual machine bound to the Termlib standard library
   ("G", "std.*" and every registered native module).  Link against
   libtermscript to use this class. */
class script_vm final
    : private detail::unique_handle<script_vm, DT_TermVM *, dt_termscript_free>
{
  using handle_base
      = detail::unique_handle<script_vm, DT_TermVM *, dt_termscript_free>;

public:
  script_vm ()
  {
    DT_Error error_raw{};
    DT_TermVM *raw_vm = dt_termscript_create (&error_raw);
    if (raw_vm == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_NO_MEMORY,
                    error_raw);
    reset (raw_vm);
  }

  /* Run source text and return the captured G:puts output. */
  std::string run (std::string_view source)
  {
    char *text = nullptr;
    DT_Error error_raw{};
    const DT_Status state = dt_termscript_run_string (
        native_handle (), detail::c_string (source, "source").c_str (), &text,
        &error_raw);
    return captured (state, error_raw, text);
  }

  /* Run a script file and return the captured G:puts output. */
  std::string run_file (std::string_view path)
  {
    char *text = nullptr;
    DT_Error error_raw{};
    const DT_Status state = dt_termscript_run_file (
        native_handle (), detail::c_string (path, "path").c_str (), &text,
        &error_raw);
    return captured (state, error_raw, text);
  }

  /* Transpile to C for the ahead-of-time backend.  Generated code calls
     the dt_rt_* runtime and must be linked against libtermscript (plus
     libtermscript_stdlib for the std.* natives). */
  std::string compile_to_c (std::string_view source, std::string_view unit_name)
  {
    DT_Error error_raw{};
    const char *code = dt_termscript_compile_to_c (
        native_handle (), detail::c_string (source, "source").c_str (),
        unit_name.empty () ? nullptr
                          : detail::c_string (unit_name, "unit name").c_str (),
        &error_raw);
    if (code == nullptr)
      detail::fail (error_raw.code != DT_OK ? error_raw.code : DT_ERR_PARSE,
                    error_raw);
    return std::string (code);
  }

  /* Borrowed captured output accumulated so far ("" when empty). */
  std::string_view output () const noexcept { return dt_rt_output (native_handle ()); }

  /* Define a global variable.  On success the value is adopted by the
     VM; on failure `value` is released and the state is reported. */
  void set (std::string_view name, term_value value, bool is_const = false,
            unsigned int line = 0)
  {
    const std::string owned = detail::c_string (name, "variable name");
    DT_Error error_raw{};
    const DT_Status state = dt_rt_set (native_handle (), owned.c_str (),
                                       value.native_handle (), is_const, line,
                                       &error_raw);
    if (state != DT_OK)
      {
        /* Not adopted: free it here and report. */
        value.reset ();
        detail::fail (state, error_raw);
      }
    value.release (); /* Adopted by the VM. */
  }

  /* Read a global variable. */
  std::optional<term_value> get (std::string_view name,
                                 unsigned int line = 0)
  {
    const std::string owned = detail::c_string (name, "variable name");
    DT_TermValue *raw = term_value::raw_storage ();
    DT_Error error_raw{};
    const DT_Status state = dt_rt_get (native_handle (), owned.c_str (), raw,
                                       line, &error_raw);
    if (state != DT_OK)
      {
        std::free (raw);
        diagnostic info = detail::diagnose (state, error_raw);
        if (info.state == status::not_found || info.state == status::parse)
          return std::nullopt;
        throw error (std::move (info));
      }
    return term_value (raw);
  }

  /* Charge one step against the shared execution budget. */
  void step ()
  {
    DT_Error error_raw{};
    detail::check (dt_rt_step (native_handle (), &error_raw), error_raw);
  }

  /* Call a native function through the compiled-code runtime.  `ret` is
     overwritten on success; failures report the Termscript line. */
  result<void> call (const term_value &module, std::string_view module_name,
                     std::string_view function,
                     const std::vector<term_value> &arguments, term_value &ret,
                     unsigned int line = 0)
  {
    const std::string owned_module
        = detail::c_string (module_name, "module name");
    const std::string owned_function
        = detail::c_string (function, "function name");
    std::vector<DT_TermValue> argv;
    argv.reserve (arguments.size ());
    for (const term_value &argument : arguments)
      {
        /* Shallow copies: the runtime copies what it needs. */
        argv.push_back (*argument.native_handle ());
      }
    DT_TermValue *raw_ret = term_value::raw_storage ();
    DT_Error error_raw{};
    const DT_Status state = dt_rt_call (
        native_handle (), module.native_handle (), owned_module.c_str (),
        owned_function.c_str (), argv.empty () ? nullptr : argv.data (),
        argv.size (), raw_ret, line, &error_raw);
    if (state != DT_OK)
      {
        std::free (raw_ret);
        return result<void>::failure (static_cast<status> (state),
                                      detail::diagnose (state, error_raw));
      }
    ret = term_value (raw_ret);
    return result<void>::success ();
  }

  /* Borrowed embedding VM (the standalone Termscript runtime). */
  TS_VM *inner_handle () const noexcept
  {
    return dt_termscript_inner_vm (native_handle ());
  }

  using handle_base::native_handle;
  using handle_base::release;
  using handle_base::valid;

private:
  /* The C runtime hands back a malloc'd buffer the caller must free. */
  static std::string
  captured (DT_Status state, const DT_Error &error_raw, char *text)
  {
    std::string out = text != nullptr ? std::string (text) : std::string ();
    std::free (text);
    if (state != DT_OK)
      detail::fail (state, error_raw);
    return out;
  }
};

} /* namespace termlib */

#endif /* TERMLIB_HPP */
