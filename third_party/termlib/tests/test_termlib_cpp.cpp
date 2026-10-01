/* test_termlib_cpp.cpp -- exercises the Termlib C++ API (termlib.hpp).
 *
 * The tests are self-contained: the compiled Terminfo entry is built in
 * memory, so no local Terminfo installation is required, and every other
 * subsystem is exercised through real file descriptors, sockets and
 * child processes.
 */

#include "termlib.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{

int failures = 0;
int checks = 0;

#define CHECK(condition)                                                      \
  do                                                                          \
    {                                                                         \
      ++checks;                                                               \
      if (!(condition))                                                       \
        {                                                                     \
          ++failures;                                                         \
          std::fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                        #condition);                                          \
        }                                                                     \
    }                                                                         \
  while (0)

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

void
put_u16 (std::vector<std::uint8_t> &out, std::uint16_t value)
{
  out.push_back (static_cast<std::uint8_t> (value & 0xff));
  out.push_back (static_cast<std::uint8_t> ((value >> 8) & 0xff));
}

void
put_bytes (std::vector<std::uint8_t> &out, std::string_view text)
{
  for (const char character : text)
    out.push_back (static_cast<std::uint8_t> (character));
}

/* Build a compiled 16-bit Terminfo entry (magic 0432) from parts.  An
   empty capability string means "absent". */
std::vector<std::uint8_t>
compile_entry (std::string names, const std::vector<bool> &bools,
               const std::vector<int> &numbers,
               const std::vector<std::string> &strings)
{
  std::vector<std::uint8_t> table;
  std::vector<std::uint16_t> offsets;
  offsets.reserve (strings.size ());
  for (const std::string &text : strings)
    {
      if (text.empty ())
        {
          offsets.push_back (0xffffu);
          continue;
        }
      offsets.push_back (static_cast<std::uint16_t> (table.size ()));
      put_bytes (table, text);
      table.push_back (0);
    }

  std::vector<std::uint8_t> out;
  put_u16 (out, 0432);                     /* magic */
  put_u16 (out, static_cast<std::uint16_t> (names.size () + 1));
  put_u16 (out, static_cast<std::uint16_t> (bools.size ()));
  put_u16 (out, static_cast<std::uint16_t> (numbers.size ()));
  put_u16 (out, static_cast<std::uint16_t> (strings.size ()));
  put_u16 (out, static_cast<std::uint16_t> (table.size ()));
  put_bytes (out, names);
  out.push_back (0);
  for (const bool value : bools)
    out.push_back (value ? 1 : 0);
  if (out.size () % 2 != 0)
    out.push_back (0); /* alignment pad before the numbers */
  for (const int value : numbers)
    put_u16 (out, static_cast<std::uint16_t> (
                      static_cast<std::int16_t> (value < 0 ? -1 : value)));
  for (const std::uint16_t offset : offsets)
    put_u16 (out, offset);
  out.insert (out.end (), table.begin (), table.end ());
  return out;
}

/* The synthetic terminal used by the Terminfo tests.  Capability
   indices follow the ncurses order used by libtermlib: bool 1 is "am",
   num 0 is "cols", num 2 is "lines", num 13 is "colors"; strings 5, 9
   and 10 are "clear", "cmdch" and "cup". */
std::vector<std::uint8_t>
sample_entry ()
{
  return compile_entry (
      "testterm|tt|tt2",
      std::vector<bool> (11, true),
      std::vector<int>{80, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                       256},
      std::vector<std::string>{"\a", "^G", "\r", "\\E[%i%p1%d;%p2%dr", "",
                               "\\E[H\\E[2J", "", "", "\\E[%i%p1%dG", "%l",
                               "\\E[%i%p1%d;%p2%dH"});
}

void
write_file (const std::string &path, const std::vector<std::uint8_t> &data)
{
  std::ofstream out (path, std::ios::binary | std::ios::trunc);
  out.write (reinterpret_cast<const char *> (data.data ()),
             static_cast<std::streamsize> (data.size ()));
}

std::string
make_temp_directory ()
{
  std::string pattern = "/tmp/termlib-cxx-XXXXXX";
  std::vector<char> buffer (pattern.begin (), pattern.end ());
  buffer.push_back ('\0');
  const char *directory = ::mkdtemp (buffer.data ());
  return directory != nullptr ? std::string (directory) : std::string ();
}

std::string
make_temp_path ()
{
  std::string pattern = "/tmp/termlib-cxx-XXXXXX";
  std::vector<char> buffer (pattern.begin (), pattern.end ());
  buffer.push_back ('\0');
  const int descriptor = ::mkstemp (buffer.data ());
  if (descriptor >= 0)
    ::close (descriptor);
  return std::string (buffer.data ());
}

/* CRC32-IEEE, matching the framing checksum (poly 0xEDB88320). */
std::uint32_t
crc32 (const std::uint8_t *data, std::size_t size)
{
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i)
    {
      crc ^= data[i];
      for (int bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1u) + 1u));
    }
  return ~crc;
}

void
put_u32be (std::vector<std::uint8_t> &out, std::uint32_t value)
{
  out.push_back (static_cast<std::uint8_t> ((value >> 24) & 0xff));
  out.push_back (static_cast<std::uint8_t> ((value >> 16) & 0xff));
  out.push_back (static_cast<std::uint8_t> ((value >> 8) & 0xff));
  out.push_back (static_cast<std::uint8_t> (value & 0xff));
}

void
put_u64be (std::vector<std::uint8_t> &out, std::uint64_t value)
{
  for (int shift = 56; shift >= 0; shift -= 8)
    out.push_back (static_cast<std::uint8_t> ((value >> shift) & 0xff));
}

void
write_all (int descriptor, const std::vector<std::uint8_t> &bytes)
{
  std::size_t offset = 0;
  while (offset < bytes.size ())
    {
      const ssize_t moved = ::write (descriptor, bytes.data () + offset,
                                     bytes.size () - offset);
      if (moved <= 0)
        break;
      offset += static_cast<std::size_t> (moved);
    }
}

/* A raw framed message: u32 length, u64 id, u8 type, payload, u32 crc. */
std::vector<std::uint8_t>
frame (std::uint64_t message_id, std::uint8_t type,
        const std::vector<std::uint8_t> &payload)
{
  std::vector<std::uint8_t> body;
  put_u64be (body, message_id);
  body.push_back (type);
  body.insert (body.end (), payload.begin (), payload.end ());
  std::vector<std::uint8_t> out;
  put_u32be (out, static_cast<std::uint32_t> (body.size () + 4));
  out.insert (out.end (), body.begin (), body.end ());
  put_u32be (out, crc32 (body.data (), body.size ()));
  return out;
}

/* ------------------------------------------------------------------ */
/* library information                                                 */
/* ------------------------------------------------------------------ */

void
test_library ()
{
  CHECK (termlib::version_string () == "0.1.0");
  CHECK (termlib::version_number () == (0u << 16 | 1u << 8 | 0u));
  CHECK (termlib::version_major == DT_VERSION_MAJOR);
  CHECK (termlib::has_feature ("pty"));
  CHECK (termlib::has_feature ("terminfo"));
  CHECK (!termlib::has_feature ("tls"));
  CHECK (!termlib::has_feature ("nonsense"));
  CHECK (std::string (termlib::to_string (termlib::status::would_block))
         == "WOULD_BLOCK");
  CHECK (termlib::is_ok (termlib::status::ok));
  CHECK (termlib::is_end_of_stream (termlib::status::eof));
  CHECK (termlib::is_retryable (termlib::status::timeout));
  CHECK (!termlib::is_retryable (termlib::status::system));
  CHECK (termlib::max_message_bytes == DT_CONN_MAX_MESSAGE);
}

/* ------------------------------------------------------------------ */
/* Terminfo                                                            */
/* ------------------------------------------------------------------ */

void
test_terminfo ()
{
  const std::string root = make_temp_directory ();
  if (root.empty ())
    {
      CHECK (false);
      return;
    }
  ::mkdir ((root + "/t").c_str (), 0700);
  /* The compiled database is looked up as <path>/<first letter>/<name>,
     so the entry is filed under the alias the lookup uses. */
  write_file (root + "/t/tt", sample_entry ());
  write_file (root + "/t/testterm", sample_entry ());

  termlib::database db = termlib::database::open ({root});
  CHECK (db.valid ());

  /* Loaded through an alias; the entry declares three names. */
  termlib::profile prof = db.load ("tt");
  CHECK (prof.name () == "testterm");
  CHECK (prof.alias_count () == 2);
  CHECK (prof.alias (0) == "tt");
  CHECK (prof.alias (1) == "tt2");
  CHECK (prof.alias (99).empty ());
  CHECK (prof.aliases ().size () == 2);

  /* Typed lookups. */
  CHECK (prof.has ("cup"));
  CHECK (!prof.has ("cud1")); /* not in this trimmed entry */
  CHECK (!prof.has ("nosuchcap"));
  CHECK (prof.boolean ("am").value_or (false) == true);
  CHECK (!prof.boolean ("cup").has_value ());
  CHECK (prof.number ("cols").value_or (0) == 80);
  CHECK (prof.number ("colors").value_or (0) == 256);
  CHECK (!prof.number ("lines").has_value ()); /* absent (-1) */
  CHECK (prof.text ("cup").value_or ("") == "\\E[%i%p1%d;%p2%dH");
  const std::optional<termlib::capability_value> value
      = prof.value_of ("cols");
  CHECK (value && std::holds_alternative<std::int32_t> (*value));
  CHECK (!prof.value_of ("nope").has_value ());

  /* Expansion. */
  CHECK (prof.expand ("cup", {3, 5}) == "\x1b[4;6H");
  CHECK (prof.expand ("clear") == "\x1b[H\x1b[2J");
  CHECK (prof.expand_tagged ("cup",
                             {termlib::parameter (3), termlib::parameter (5)})
         == "\x1b[4;6H");
  CHECK (prof.expand_tagged ("cmdch",
                             {termlib::parameter (std::string_view ("xy"))})
         == "xy");

  /* expand_into reports the required size and truncates to fit. */
  char small[4] = {0, 0, 0, 0};
  CHECK (prof.expand_into ("cup", small, sizeof small,
                           {termlib::parameter (3), termlib::parameter (5)})
         == 6);
  CHECK (std::string (small) == "\x1b[4");

  /* A number where %l expects text is a hard parse error. */
  termlib::status failure = termlib::status::ok;
  try
    {
      prof.expand_tagged ("cmdch", {termlib::parameter (7)});
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::parse);

  /* Unknown and non-string capabilities are distinguished. */
  failure = termlib::status::ok;
  try
    {
      prof.expand ("nosuchcap");
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::not_found);

  failure = termlib::status::ok;
  try
    {
      prof.expand ("cols");
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::invalid_argument);

  /* NUL bytes in names are rejected instead of truncating. */
  failure = termlib::status::ok;
  try
    {
      prof.expand (std::string_view ("cu\0p", 4));
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::invalid_argument);

  /* Introspection. */
  const std::optional<termlib::capability> info = prof.capability_info ("cup");
  CHECK (info && info->name == "cup");
  CHECK (info && info->type == termlib::capability_type::text);
  CHECK (info && info->present);
  CHECK (!prof.capability_info ("nope").has_value ());
  CHECK (termlib::profile_view::capability_count () > 400);
  CHECK (termlib::profile_view::capability_by_index (0).has_value ());
  CHECK (!termlib::profile_view::capability_by_index (
              termlib::profile_view::capability_count ())
              .has_value ());

  /* Loading an unknown terminal fails with not_found. */
  failure = termlib::status::ok;
  try
    {
      db.load ("nosuchterm");
    }
  catch (termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::not_found);

  /* The primary name resolves to the same entry. */
  CHECK (db.load ("testterm").name () == "testterm");

  /* A database with no search paths cannot load anything. */
  termlib::database empty;
  failure = termlib::status::ok;
  try
    {
      empty.load ("testterm");
    }
  catch (termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::not_found);

  /* Views borrow: a profile_view outlives its owner's scope safely as
     long as the profile is alive. */
  {
    termlib::profile_view view = prof;
    CHECK (view.expand ("cup", {0, 0}) == "\x1b[1;1H");
  }

  /* Move semantics leave the source empty. */
  termlib::profile moved = std::move (prof);
  CHECK (moved.valid ());
  CHECK (!prof.valid ());
  CHECK (moved.name () == "testterm");

  ::unlink ((root + "/t/tt").c_str ());
  ::unlink ((root + "/t/testterm").c_str ());
  ::rmdir ((root + "/t").c_str ());
  ::rmdir (root.c_str ());
}

void
test_terminfo_parser ()
{
  const std::vector<std::uint8_t> entry = sample_entry ();

  /* Incremental feed across chunk boundaries, then take. */
  termlib::entry_parser parser;
  CHECK (parser.feed (entry.data (), 7, false) == 7);
  CHECK (parser.feed (entry.data () + 7, entry.size () - 7, true)
         == entry.size () - 7);
  const std::optional<termlib::profile> parsed = parser.take ();
  CHECK (parsed.has_value ());
  if (parsed)
    CHECK (parsed->expand ("cup", {1, 1}) == "\x1b[2;2H");
  /* The buffer was consumed: a second take yields nothing. */
  CHECK (!parser.take ().has_value ());

  /* take() without a final chunk yields nothing. */
  termlib::entry_parser partial;
  partial.feed (entry.data (), entry.size (), false);
  CHECK (!partial.take ().has_value ());

  /* Truncated input is rejected. */
  termlib::status failure = termlib::status::ok;
  try
    {
      termlib::entry_parser::parse (
          termlib::bytes_view (entry.data (), entry.size () - 4));
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::parse);

  /* Unknown magic numbers are unsupported, not malformed. */
  std::vector<std::uint8_t> foreign = entry;
  foreign[0] = 0x99;
  foreign[1] = 0x99;
  failure = termlib::status::ok;
  try
    {
      termlib::entry_parser::parse (termlib::bytes_view::of (foreign));
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::unsupported);

  /* Trailing garbage after a valid entry is malformed. */
  std::vector<std::uint8_t> padded = entry;
  padded.push_back (0);
  padded.push_back (0);
  failure = termlib::status::ok;
  try
    {
      termlib::entry_parser::parse (termlib::bytes_view::of (padded));
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::parse);

  /* An out-of-range string offset is rejected. */
  std::vector<std::uint8_t> bad_offset = entry;
  const std::size_t offsets_at = 12 + 11 + 11 + 28;
  bad_offset[offsets_at] = 0xfe;
  bad_offset[offsets_at + 1] = 0x00;
  failure = termlib::status::ok;
  try
    {
      termlib::entry_parser::parse (termlib::bytes_view::of (bad_offset));
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::parse);

  /* Feeding after the final chunk is a protocol violation. */
  termlib::entry_parser done;
  done.feed (entry.data (), entry.size (), true);
  failure = termlib::status::ok;
  try
    {
      done.feed (entry.data (), 1, false);
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::protocol);
}

/* ------------------------------------------------------------------ */
/* TTY sessions                                                        */
/* ------------------------------------------------------------------ */

void
test_tty ()
{
  int pair[2] = {-1, -1};
  if (::socketpair (AF_UNIX, SOCK_STREAM, 0, pair) != 0)
    {
      CHECK (false);
      return;
    }

  termlib::tty_options options;
  options.fd = pair[0];
  options.take_ownership = true;
  options.nonblocking = true;
  termlib::tty_session session = termlib::tty_session::open (options);
  CHECK (session.valid ());
  CHECK (session.fd () == pair[0]);
  CHECK (session.poll_fd () == pair[0]);

  /* A plain stream supports read/write; termios calls do not. */
  CHECK (session.write ("ping").value () == 4);
  char buffer[8] = {0};
  CHECK (::read (pair[1], buffer, sizeof buffer) == 4);
  CHECK (std::string (buffer) == "ping");

  /* Nothing pending: would-block is a state, not an exception. */
  const termlib::transfer drained = session.read (buffer, sizeof buffer);
  CHECK (!drained.ok ());
  CHECK (drained.state () == termlib::status::would_block);

  CHECK (::write (pair[1], "pong", 4) == 4);
  const termlib::result<std::string> received = session.read_some (8);
  CHECK (received.ok ());
  if (received)
    CHECK (received.value () == "pong");

  /* termios operations fail on a non-terminal descriptor. */
  termlib::status failure = termlib::status::ok;
  try
    {
      session.set_raw ();
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::system);

  /* write_all reports progress and state separately. */
  const termlib::written written = session.write_all ("0123456789");
  CHECK (written.ok ());
  CHECK (written.bytes == 10);

  /* Closing the peer produces end of stream. */
  ::close (pair[1]);
  const termlib::transfer ended = session.read (buffer, sizeof buffer);
  CHECK (ended.state () == termlib::status::eof);
  CHECK (termlib::is_end_of_stream (ended.state ()));

  /* An invalid descriptor is rejected up front. */
  failure = termlib::status::ok;
  try
    {
      termlib::tty_session::open (termlib::tty_options{});
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::invalid_argument);

  /* Move semantics: the source releases nothing. */
  termlib::tty_session moved = std::move (session);
  CHECK (moved.valid ());
  CHECK (!session.valid ());
  CHECK (session.fd () == -1);
}

/* ------------------------------------------------------------------ */
/* PTY sessions                                                        */
/* ------------------------------------------------------------------ */

void
test_pty ()
{
  termlib::pty_options options;
  options.argv = {"/bin/sh", "-c", "printf hello"};
  options.terminal_name = "dumb";
  options.rows = 30;
  options.columns = 100;

  termlib::pty_session shell = termlib::pty_session::spawn (options);
  CHECK (shell.valid ());
  CHECK (shell.fd () >= 0);
  CHECK (shell.info ().child_pid > 0);
  CHECK (shell.info ().master_fd == shell.fd ());

  std::string output;
  for (;;)
    {
      const termlib::result<std::string> chunk = shell.read_some ();
      if (!chunk)
        {
          CHECK (chunk.state () == termlib::status::eof);
          break;
        }
      output += chunk.value ();
    }
  CHECK (output == "hello");

  const termlib::child_exit finished = shell.wait ();
  CHECK (finished.exited_normally);
  CHECK (finished.code == 0);
  /* Reaping is idempotent. */
  const termlib::result<termlib::child_exit> again = shell.poll ();
  CHECK (again.ok ());
  if (again)
    CHECK (again.value ().code == 0);

  int code = 0;
  CHECK (termlib::wait_status_exited (shell.wait_raw (false), code));
  CHECK (code == 0);

  /* Window size round-trips. */
  shell.resize (40, 120);
  const termlib::winsize size = shell.size ();
  CHECK (size.rows == 40);
  CHECK (size.columns == 120);

  /* Signal termination is reported separately from a normal exit. */
  termlib::pty_options killed;
  killed.argv = {"/bin/sh", "-c", "kill -TERM $$; sleep 5"};
  killed.kill_on_close = false;
  termlib::pty_session doomed = termlib::pty_session::spawn (killed);
  const termlib::child_exit signalled = doomed.wait ();
  CHECK (!signalled.exited_normally);
  CHECK (signalled.code == 128 + 15);

  /* A poll before the child exits reports would-block. */
  termlib::pty_options sleeper;
  sleeper.argv = {"/bin/sh", "-c", "sleep 5"};
  termlib::pty_session slow = termlib::pty_session::spawn (sleeper);
  const termlib::result<termlib::child_exit> pending = slow.poll ();
  CHECK (!pending.ok ());
  CHECK (pending.state () == termlib::status::would_block);
  CHECK (termlib::is_retryable (pending.state ()));
  slow.terminate (SIGTERM);
  const termlib::child_exit stopped = slow.wait ();
  CHECK (!stopped.exited_normally);

  /* Nonblocking round trip through `cat`, including write_all. */
  termlib::pty_options echo;
  echo.argv = {"/bin/cat"};
  echo.nonblocking = true;
  termlib::pty_session cat = termlib::pty_session::spawn (echo);
  const termlib::written written = cat.write_all ("ping\n");
  CHECK (written.ok ());
  CHECK (written.bytes == 5);
  std::string echoed;
  for (int attempt = 0; attempt < 100 && echoed.size () < 5; ++attempt)
    {
      const termlib::result<std::string> chunk = cat.read_some ();
      if (chunk)
        echoed += chunk.value ();
      else if (chunk.state () == termlib::status::would_block)
        ::usleep (2000);
      else
        break;
    }
  CHECK (echoed == "ping\n");

  /* An exec failure is reported with the child's errno. */
  termlib::status failure = termlib::status::ok;
  try
    {
      termlib::pty_options broken;
      broken.argv = {"/nonexistent/termlib-binary"};
      termlib::pty_session::spawn (broken);
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
      CHECK (thrown.system_errno () == ENOENT);
    }
  CHECK (failure == termlib::status::system);

  /* argv[0] is mandatory. */
  failure = termlib::status::ok;
  try
    {
      termlib::pty_session::spawn (termlib::pty_options{});
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure == termlib::status::invalid_argument);
}

/* ------------------------------------------------------------------ */
/* Recording and replay                                                */
/* ------------------------------------------------------------------ */

void
test_record_replay ()
{
  const std::string path = make_temp_path ();
  std::FILE *stream = std::fopen (path.c_str (), "w+b");
  if (stream == nullptr)
    {
      CHECK (false);
      return;
    }

  {
    termlib::recorder recorder = termlib::recorder::create (stream, false);
    CHECK (recorder.valid ());
    const std::string input = "in";
    const std::string output = "out";
    recorder.write_input (termlib::bytes_view::of (input));
    recorder.write_output (termlib::bytes_view::of (output), 1000);
    recorder.write_resize (24, 80);
    recorder.write_meta ("key=value\n");
    recorder.write_heartbeat ();
    recorder.flush ();
  }

  std::rewind (stream);
  {
    termlib::replayer replayer = termlib::replayer::create (stream, false);
    CHECK (replayer.mode () == termlib::replay_mode::fast);

    const termlib::result<termlib::record_event> first = replayer.next ();
    CHECK (first.ok ());
    if (first)
      {
        CHECK (first.value ().type == termlib::record_type::input);
        CHECK (first.value ().text () == "in");
        CHECK (first.value ().timestamp_ns != 0); /* stamped by the recorder */
      }

    const termlib::result<termlib::record_event> second = replayer.next ();
    CHECK (second.ok ());
    if (second)
      {
        CHECK (second.value ().type == termlib::record_type::output);
        CHECK (second.value ().text () == "out");
        CHECK (second.value ().timestamp_ns == 1000);
      }

    const termlib::result<termlib::record_event> third = replayer.next ();
    CHECK (third.ok ());
    if (third)
      {
        CHECK (third.value ().type == termlib::record_type::resize);
        CHECK (third.value ().rows == 24);
        CHECK (third.value ().columns == 80);
        CHECK (third.value ().data.empty ());
      }

    const termlib::result<termlib::record_event> fourth = replayer.next ();
    CHECK (fourth.ok ());
    if (fourth)
      CHECK (fourth.value ().type == termlib::record_type::meta);

    const termlib::result<termlib::record_event> fifth = replayer.next ();
    CHECK (fifth.ok ());
    if (fifth)
      CHECK (fifth.value ().type == termlib::record_type::heartbeat);

    const termlib::result<termlib::record_event> end = replayer.next ();
    CHECK (!end.ok ());
    CHECK (end.state () == termlib::status::eof);

    /* Rewind, pause, step and compile. */
    replayer.rewind ();
    replayer.pause ();
    CHECK (replayer.mode () == termlib::replay_mode::paused);
    const termlib::result<termlib::record_event> paused = replayer.next ();
    CHECK (!paused.ok ());
    CHECK (paused.state () == termlib::status::would_block);
    replayer.resume ();
    CHECK (replayer.mode () == termlib::replay_mode::fast);

    replayer.set_mode (termlib::replay_mode::step);
    const termlib::result<termlib::record_event> stepped = replayer.step ();
    CHECK (stepped.ok ());
    replayer.set_mode (termlib::replay_mode::realtime);
    replayer.set_speed (4.0);
    CHECK (replayer.mode () == termlib::replay_mode::realtime);

    replayer.rewind ();
    const std::string program
        = replayer.compile ("# header\n{{events}}", termlib::replay_target::termscript);
    CHECK (program.find ("# header") == 0);
    CHECK (program.find ("G:puts") != std::string::npos);
  }

  std::fclose (stream);

  /* Corrupt one payload byte: the CRC check must reject the record. */
  {
    std::FILE *corrupt = std::fopen (path.c_str (), "r+b");
    if (corrupt != nullptr)
      {
        std::fseek (corrupt, 8 + 16 + 1, SEEK_SET);
        const int byte = std::fgetc (corrupt);
        std::fseek (corrupt, 8 + 16 + 1, SEEK_SET);
        std::fputc (byte ^ 0xff, corrupt);
        std::fclose (corrupt);

        std::FILE *replay_stream = std::fopen (path.c_str (), "rb");
        termlib::status failure = termlib::status::ok;
        try
          {
            termlib::replayer replayer
                = termlib::replayer::create (replay_stream, true);
            const termlib::result<termlib::record_event> event
                = replayer.next ();
            failure = event.state ();
          }
        catch (const termlib::error &thrown)
          {
            failure = thrown.state ();
          }
        CHECK (failure == termlib::status::protocol);
      }
    else
      CHECK (false);
  }

  /* A truncated header is rejected at open time. */
  {
    const std::string truncated_path = make_temp_path ();
    std::FILE *truncated
        = std::fopen (truncated_path.c_str (), "wb");
    if (truncated != nullptr)
      {
        std::fputs ("DM", truncated);
        std::fclose (truncated);
        std::FILE *stream2 = std::fopen (truncated_path.c_str (), "rb");
        termlib::status failure = termlib::status::ok;
        try
          {
            termlib::replayer::create (stream2, true);
          }
        catch (const termlib::error &thrown)
          {
            failure = thrown.state ();
          }
        CHECK (failure == termlib::status::parse);
      }
    ::unlink (truncated_path.c_str ());
  }

  ::unlink (path.c_str ());
}

/* ------------------------------------------------------------------ */
/* Messages, envelopes and local connections                           */
/* ------------------------------------------------------------------ */

void
test_connections ()
{
  int pair[2] = {-1, -1};
  if (::socketpair (AF_UNIX, SOCK_STREAM, 0, pair) != 0)
    {
      CHECK (false);
      return;
    }

  termlib::local_options left_options;
  left_options.fd = pair[0];
  left_options.take_ownership = true;
  left_options.read_timeout_ms = 200;
  left_options.write_timeout_ms = 200;
  termlib::local_options right_options;
  right_options.fd = pair[1];
  right_options.take_ownership = true;
  right_options.read_timeout_ms = 100;
  right_options.write_timeout_ms = 100;
  termlib::local_connection left
      = termlib::local_connection::open (left_options);
  termlib::local_connection right
      = termlib::local_connection::open (right_options);
  CHECK (left.fd () == pair[0]);
  CHECK (left.state () == termlib::connection_state::open);

  /* Message and envelope factories. */
  termlib::message data = termlib::message::make_data ("hello");
  CHECK (data.type () == termlib::message_type::data);
  CHECK (data.text () == "hello");
  CHECK (!data.resize ().has_value ());
  termlib::message sized = termlib::message::make_resize ({30, 100});
  CHECK (sized.type () == termlib::message_type::resize);
  CHECK (sized.resize ().value ().rows == 30);
  CHECK (sized.resize ().value ().columns == 100);
  CHECK (sized.payload ().empty ());
  termlib::message beat = termlib::message::make_heartbeat ();
  CHECK (beat.type () == termlib::message_type::heartbeat);
  termlib::message problem = termlib::message::make_error (42, "boom");
  CHECK (problem.type () == termlib::message_type::error);
  CHECK (problem.text () == "boom");
  CHECK (std::string (termlib::to_string (termlib::message_type::heartbeat))
         == "HEARTBEAT");

  /* Send and receive one envelope. */
  left.send (termlib::envelope::make_data (7, "ping")).raise ();
  termlib::result<termlib::envelope> received = right.receive ();
  CHECK (received.ok ());
  if (received)
    {
      CHECK (received.value ().id () == 7);
      CHECK (received.value ().body ().text () == "ping");
      /* The payload can be taken out of the envelope. */
      const termlib::message taken = received.value ().take_body ();
      CHECK (taken.text () == "ping");
      CHECK (!received.value ().valid ());
    }

  /* A resize message survives the round trip. */
  left.send (termlib::envelope::make_resize (9, 30, 100)).raise ();
  const termlib::result<termlib::envelope> resized = right.receive ();
  CHECK (resized.ok ());
  if (resized)
    {
      const std::optional<termlib::winsize> size
          = resized.value ().body ().resize ();
      CHECK (size && size->rows == 30 && size->columns == 100);
    }

  /* Ping is a heartbeat envelope. */
  left.ping (11);
  const termlib::result<termlib::envelope> beat_in = right.receive ();
  CHECK (beat_in.ok ());
  if (beat_in)
    {
      CHECK (beat_in.value ().id () == 11);
      CHECK (beat_in.value ().body ().type ()
             == termlib::message_type::heartbeat);
    }

  /* Binary payloads keep embedded NUL bytes. */
  const std::uint8_t blob[] = {0x00, 0x41, 0x00, 0xff};
  left.send (termlib::envelope::make_data (
                 12, termlib::bytes_view (blob, sizeof blob)))
      .raise ();
  const termlib::result<termlib::envelope> binary = right.receive ();
  CHECK (binary.ok ());
  if (binary)
    {
      const termlib::bytes_view payload = binary.value ().body ().payload ();
      CHECK (payload.size () == sizeof blob);
      CHECK (payload[3] == 0xff);
      CHECK (payload.size () == payload.to_string ().size ());
    }

  /* Nothing to read: the read timeout is a state, not a failure. */
  const termlib::result<termlib::envelope> idle = right.receive ();
  CHECK (!idle.ok ());
  CHECK (idle.state () == termlib::status::timeout);
  CHECK (termlib::is_retryable (idle.state ()));

  /* value() on a failed result rethrows with the same diagnostic. */
  termlib::status rethrown = termlib::status::ok;
  try
    {
      idle.value ();
    }
  catch (const termlib::error &thrown)
    {
      rethrown = thrown.state ();
    }
  CHECK (rethrown == termlib::status::timeout);

  /* A payload above the frame limit is rejected before allocation. */
  {
    termlib::status failure = termlib::status::ok;
    try
      {
        termlib::message::make_data (
            std::string (termlib::max_message_bytes + 1, 'x'));
      }
    catch (const termlib::error &thrown)
      {
        failure = thrown.state ();
      }
    CHECK (failure == termlib::status::limit);
  }

  /* The transport-neutral interface drives either transport. */
  {
    std::unique_ptr<termlib::connection> transport
        = std::make_unique<termlib::local_connection> (
            termlib::local_connection::open ({pair[0], false}));
    CHECK (transport->fd () >= 0);
    CHECK (transport->state () == termlib::connection_state::open);
  }

  /* An unknown message type on the wire is a protocol error. */
  {
    const std::vector<std::uint8_t> unknown = frame (21, 0x7f, {'a'});
    write_all (pair[0], unknown);
    const termlib::result<termlib::envelope> rejected = right.receive ();
    CHECK (!rejected.ok ());
    CHECK (rejected.state () == termlib::status::protocol);
  }

  /* An oversized frame header is a limit error, not an allocation. */
  {
    std::vector<std::uint8_t> huge;
    put_u32be (huge, static_cast<std::uint32_t> (13 + termlib::max_message_bytes
                                                  + 1));
    write_all (pair[0], huge);
    const termlib::result<termlib::envelope> rejected = right.receive ();
    CHECK (!rejected.ok ());
    CHECK (rejected.state () == termlib::status::limit);
  }

  /* Cancellation unblocks a waiting reader instead of hanging. */
  right.cancel ();
  const termlib::result<termlib::envelope> cancelled = right.receive ();
  CHECK (!cancelled.ok ());
}

void
test_connection_states ()
{
  int pair[2] = {-1, -1};
  if (::socketpair (AF_UNIX, SOCK_STREAM, 0, pair) != 0)
    {
      CHECK (false);
      return;
    }
  termlib::local_connection left
      = termlib::local_connection::open ({pair[0], true});
  termlib::local_connection right
      = termlib::local_connection::open ({pair[1], true});

  /* Sending CLOSE moves the sender to the shutdown state. */
  left.send (termlib::envelope::make_close (1)).raise ();
  CHECK (left.state () == termlib::connection_state::shutdown);
  const termlib::result<termlib::envelope> received = right.receive ();
  CHECK (received.ok ());
  CHECK (right.state () == termlib::connection_state::shutdown);
  if (received)
    CHECK (received.value ().body ().type () == termlib::message_type::close);

  /* A closed connection refuses further traffic. */
  const termlib::result<void> refused
      = left.send (termlib::envelope::make_data (2, "late"));
  CHECK (!refused.ok ());

  ::close (pair[1]);
  ::close (pair[0]);
}

/* ------------------------------------------------------------------ */
/* Puppeteer                                                           */
/* ------------------------------------------------------------------ */

void
test_puppeteer ()
{
  const std::string path = make_temp_path ();
  std::FILE *stream = std::fopen (path.c_str (), "w+b");
  if (stream == nullptr)
    {
      CHECK (false);
      return;
    }

  termlib::recorder recorder = termlib::recorder::create (stream, false);

  termlib::puppeteer_options options;
  options.pty.argv = {"/bin/sh", "-c", "printf hi"};
  options.pty.rows = 24;
  options.pty.columns = 80;
  options.record = true;
  options.recorder_sink = &recorder;

  termlib::puppeteer session = termlib::puppeteer::create (options);
  CHECK (session.valid ());
  CHECK (session.recording () == &recorder);
  CHECK (session.fd () >= 0);
  /* No terminal name was given, so no profile was loaded. */
  CHECK (!session.terminfo ().valid ());
  CHECK (session.terminfo ().name ().empty ());

  session.request_resize (30, 100); /* thread-safe, applied on the next pump */
  const termlib::result<void> pumped = session.pump_once (2000);
  CHECK (pumped.ok ());

  const termlib::child_exit finished = session.wait ();
  CHECK (finished.exited_normally);
  CHECK (finished.code == 0);
  recorder.flush ();

  /* The recorded session contains the child's output. */
  std::rewind (stream);
  termlib::replayer replayer = termlib::replayer::create (stream, false);
  const termlib::result<termlib::record_event> event = replayer.next ();
  CHECK (event.ok ());
  if (event)
    {
      CHECK (event.value ().type == termlib::record_type::output);
      CHECK (event.value ().text () == "hi");
    }
  const termlib::result<termlib::record_event> end = replayer.next ();
  CHECK (!end.ok ());

  std::fclose (stream);
  ::unlink (path.c_str ());
}

/* ------------------------------------------------------------------ */
/* Termscript (only when libtermscript is linked in)                    */
/* ------------------------------------------------------------------ */

#ifdef TERMLIB_WITH_TERMSCRIPT

DT_Status
native_double (DT_TermVM *, const DT_TermValue *argv, std::size_t argc,
                DT_TermValue *ret, DT_Error *error)
{
  if (argc != 1)
    {
      dt_error_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "one argument");
      return DT_ERR_INVALID_ARGUMENT;
    }
  ret->type = DT_TERM_INT;
  ret->as.integer = argv[0].as.integer * 2;
  return DT_OK;
}

void
test_termscript ()
{
  static const DT_TermFuncDef functions[] = {
      {"double", native_double},
      {nullptr, nullptr},
  };
  static const DT_TermModule module = {"test.native", functions};
  CHECK (dt_termscript_register_module (&module, nullptr) == DT_OK);

  termlib::script_vm vm;
  const std::string output = vm.run ("G:puts \"hello\";");
  CHECK (output == "hello\n");
  CHECK (vm.output () == "hello\n");
  CHECK (vm.run_file ("/nonexistent/termlib-script") .empty ());

  termlib::status failure = termlib::status::ok;
  try
    {
      vm.run ("this is not termscript");
    }
  catch (const termlib::error &thrown)
    {
      failure = thrown.state ();
    }
  CHECK (failure != termlib::status::ok);

  /* Values. */
  termlib::term_value number = termlib::term_value::integer (21);
  CHECK (number.type () == termlib::term_value_type::integer);
  CHECK (number.as_integer () == 21);
  CHECK (number.to_string () == "21");
  termlib::term_value text = termlib::term_value::text ("abc");
  CHECK (text.as_text () == "abc");
  CHECK (text.truthy ());
  CHECK (!termlib::term_value::nil ().truthy ());
  CHECK (!termlib::term_value::integer (0).truthy ());

  vm.set ("answer", termlib::term_value::integer (42));
  const std::optional<termlib::term_value> read = vm.get ("answer");
  CHECK (read && read->as_integer () == 42);
  CHECK (!vm.get ("missing_variable").has_value ());

  const std::string generated
      = vm.compile_to_c ("const x = 1;", "unit");
  CHECK (!generated.empty ());
}

#endif /* TERMLIB_WITH_TERMSCRIPT */

} /* namespace */

int
main ()
{
  test_library ();
  test_terminfo ();
  test_terminfo_parser ();
  test_tty ();
  test_pty ();
  test_record_replay ();
  test_connections ();
  test_connection_states ();
  test_puppeteer ();
#ifdef TERMLIB_WITH_TERMSCRIPT
  test_termscript ();
#endif

  std::fprintf (stderr, "%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
