/* pty.c -- std.pty: child processes under a pseudoterminal.
 *
 * A standalone-safe minipty (no libdomterm).  spawn() forks /bin/sh -c under
 * a fresh pty pair; read()/write() move bytes; wait() reaps (nil while
 * running); close() kills and reaps.
 *
 * Also the parts a real terminal host needs and the original five functions
 * did not provide:
 *   - winsize(pty) and set_winsize(pty, rows, cols).  A child that starts with
 *     a 0x0 window size breaks every full-screen program, and TIOCSWINSZ makes
 *     the kernel deliver SIGWINCH to the foreground process group, which is
 *     why set_winsize does not signal by hand.
 *   - raw(pty) and restore(pty) save and set the pty's line discipline.  The
 *     master and slave share one termios, so the master descriptor is the
 *     right place to work; restore puts back what was saved, which is what a
 *     host needs on its way out.
 *   - read_ready(pty, max?) distinguishes the three states read() conflates:
 *     a string when bytes arrived, false when the pty would block, and nil at
 *     end of file.  Blocking read() cannot tell those apart, and a VM that
 *     blocks inside read() stops its whole event loop.
 *   - signal(pty, sig), pid(pty) and alive(pty) for job control.
 */
#include "pty/pty.h"

#include "common/ts_std_common.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

typedef struct
{
  int master;
  pid_t pid;
  bool reaped;
  int exit_code;
  struct termios saved_termios;
  bool termios_saved;
} pty_t;

static const char pty_tag_id = 0;

static void
pty_free (void *p)
{
  pty_t *h = p;
  int st;
  if (!h)
    return;
  if (h->termios_saved && h->master >= 0)
    tcsetattr (h->master, TCSANOW, &h->saved_termios);
  if (!h->reaped && h->pid > 0)
    {
      kill (h->pid, SIGKILL);
      while (waitpid (h->pid, &st, 0) < 0 && errno == EINTR)
        ;
    }
  if (h->master >= 0)
    close (h->master);
  free (h);
}

static pty_t *
pty_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  return ts_std_handle (v, &pty_tag_id, error, what);
}

/* Signal numbers by name for the handful a host actually sends.  Anything else
 * must be passed as a number, or looked up with std.signal:number. */
static bool
pty_signal_by_name (const char *name, int *out)
{
  static const struct
  {
    const char *name;
    int sig;
  } table[] = {
    { "HUP", SIGHUP },   { "INT", SIGINT },   { "QUIT", SIGQUIT },
    { "KILL", SIGKILL }, { "TERM", SIGTERM }, { "STOP", SIGSTOP },
    { "TSTP", SIGTSTP }, { "CONT", SIGCONT }, { "WINCH", SIGWINCH },
  };
  size_t i;
  for (i = 0; i < sizeof table / sizeof table[0]; i++)
    if (strcmp (name, table[i].name) == 0
        || strcmp (name, table[i].name + 0) == 0)
      {
        *out = table[i].sig;
        return true;
      }
  return false;
}

static TS_Status
p_spawn (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *cmd;
  int64_t rows = 0, cols = 0;
  int master, slave;
  char *slave_name;
  pid_t pid;
  pty_t *h;
  char desc[96];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 3, error, "spawn") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &cmd, error, "spawn") != TS_OK)
    return TS_ERR_INVAL;
  /* Optional starting window size: a child left at 0x0 misbehaves. */
  if (argc >= 2 && ts_std_int (&argv[1], &rows, error, "spawn") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 3 && ts_std_int (&argv[2], &cols, error, "spawn") != TS_OK)
    return TS_ERR_INVAL;
  if (rows < 0 || rows > 100000 || cols < 0 || cols > 100000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "spawn: bad size");
      return TS_ERR_INVAL;
    }

  master = posix_openpt (O_RDWR | O_NOCTTY);
  if (master < 0)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "spawn: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  if (grantpt (master) != 0 || unlockpt (master) != 0)
    {
      int e = errno;
      close (master);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "spawn: %s", strerror (e));
      return TS_ERR_SYSTEM;
    }
  slave_name = ptsname (master);
  if (!slave_name)
    {
      int e = errno;
      close (master);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "spawn: %s", strerror (e));
      return TS_ERR_SYSTEM;
    }
  if (rows > 0 && cols > 0)
    {
      struct winsize ws;
      memset (&ws, 0, sizeof ws);
      ws.ws_row = (unsigned short) rows;
      ws.ws_col = (unsigned short) cols;
      ioctl (master, TIOCSWINSZ, &ws);
    }
  slave = open (slave_name, O_RDWR | O_NOCTTY);
  if (slave < 0)
    {
      int e = errno;
      close (master);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "spawn: %s", strerror (e));
      return TS_ERR_SYSTEM;
    }
  pid = fork ();
  if (pid < 0)
    {
      int e = errno;
      close (slave);
      close (master);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "spawn: %s", strerror (e));
      return TS_ERR_SYSTEM;
    }
  if (pid == 0)
    {
      /* Child: new session, controlling terminal, stdio on slave. */
      setsid ();
      ioctl (slave, TIOCSCTTY, 0);
      dup2 (slave, 0);
      dup2 (slave, 1);
      dup2 (slave, 2);
      if (slave > 2)
        close (slave);
      close (master);
      execl ("/bin/sh", "sh", "-c", cmd, (char *) NULL);
      _exit (127);
    }
  close (slave);
  h = calloc (1, sizeof *h);
  if (!h)
    {
      int st;
      kill (pid, SIGKILL);
      while (waitpid (pid, &st, 0) < 0 && errno == EINTR)
        ;
      close (master);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  h->master = master;
  h->pid = pid;
  snprintf (desc, sizeof desc, "<pty pid=%d>", (int) pid);
  if (ts_value_make_handle (ret, h, pty_free, desc, &pty_tag_id) != TS_OK)
    {
      pty_free (h);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
p_read (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  int64_t max = 65536;
  char *buf;
  ssize_t r;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "read") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "read");
  if (!h)
    return TS_ERR_INVAL;
  if (h->master < 0)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  if (argc == 2 && ts_std_int (&argv[1], &max, error, "read") != TS_OK)
    return TS_ERR_INVAL;
  if (max < 1 || max > 1000000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "read: bad size");
      return TS_ERR_INVAL;
    }
  buf = malloc ((size_t) max + 1);
  if (!buf)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  for (;;)
    {
      r = read (h->master, buf, (size_t) max);
      if (r < 0 && errno == EINTR)
        continue;
      break;
    }
  if (r <= 0)
    {
      free (buf);
      ts_std_ret_nil (ret); /* EOF, or EIO once the child has gone. */
      return TS_OK;
    }
  buf[r] = '\0';
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = buf;
  return TS_OK;
}

/* read_ready(pty, max?) -> string | false | nil.
 *
 * string: bytes were available.  false: nothing to read right now (EAGAIN).
 * nil: end of file, or the handle is closed.  This is the three-way split
 * that read() cannot express, and it never blocks, so it is safe to call from
 * a polling loop. */
static TS_Status
p_read_ready (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
              TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  int64_t max = 65536;
  char *buf;
  ssize_t r;
  int flags, saved_flags = -1;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "read_ready") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "read_ready");
  if (!h)
    return TS_ERR_INVAL;
  if (h->master < 0)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  if (argc == 2 && ts_std_int (&argv[1], &max, error, "read_ready") != TS_OK)
    return TS_ERR_INVAL;
  if (max < 1 || max > 1000000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "read_ready: bad size");
      return TS_ERR_INVAL;
    }
  buf = malloc ((size_t) max + 1);
  if (!buf)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }

  /* Toggle O_NONBLOCK around the read so a caller that never asked for a
   * non-blocking pty keeps blocking semantics for read(). */
  flags = fcntl (h->master, F_GETFL, 0);
  if (flags >= 0)
    {
      if (fcntl (h->master, F_SETFL, flags | O_NONBLOCK) == 0)
        saved_flags = flags;
    }

  for (;;)
    {
      r = read (h->master, buf, (size_t) max);
      if (r < 0 && errno == EINTR)
        continue;
      break;
    }
  if (saved_flags >= 0)
    fcntl (h->master, F_SETFL, saved_flags);

  if (r > 0)
    {
      buf[r] = '\0';
      ts_value_free (ret);
      ret->type = TS_STRING;
      ret->as.string = buf;
      return TS_OK;
    }
  free (buf);
  if (r == 0)
    {
      ts_std_ret_nil (ret);       /* clean end of file */
      return TS_OK;
    }
  if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  if (errno == EIO)
    {
      /* The last slave descriptor closed when the child exited. */
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "read_ready: %s",
                strerror (errno));
  return TS_ERR_SYSTEM;
}

static TS_Status
p_write (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  const char *text;
  size_t n, sent = 0;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "write") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "write");
  if (!h)
    return TS_ERR_INVAL;
  if (h->master < 0)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  if (ts_std_str (&argv[1], &text, error, "write") != TS_OK)
    return TS_ERR_INVAL;
  n = strlen (text);
  while (sent < n)
    {
      ssize_t r = write (h->master, text + sent, n - sent);
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          ts_std_ret_bool (ret, false);
          return TS_OK;
        }
      sent += (size_t) r;
    }
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static TS_Status
p_wait (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  int st;
  pid_t r;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "wait") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "wait");
  if (!h)
    return TS_ERR_INVAL;
  if (h->reaped)
    {
      ts_std_ret_int (ret, h->exit_code);
      return TS_OK;
    }
  r = waitpid (h->pid, &st, WNOHANG);
  if (r < 0)
    {
      /* Already reaped elsewhere: report that rather than failing. */
      if (errno == ECHILD)
        {
          h->reaped = true;
          ts_std_ret_int (ret, h->exit_code);
          return TS_OK;
        }
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "wait: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  if (!r)
    {
      ts_std_ret_nil (ret); /* Still running. */
      return TS_OK;
    }
  h->reaped = true;
  h->exit_code = WIFEXITED (st) ? WEXITSTATUS (st) :
                 WIFSIGNALED (st) ? 128 + WTERMSIG (st) : 127;
  ts_std_ret_int (ret, h->exit_code);
  return TS_OK;
}

static TS_Status
p_pid (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "pid") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "pid");
  if (!h)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) h->pid);
  return TS_OK;
}

static TS_Status
p_alive (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "alive") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "alive");
  if (!h)
    return TS_ERR_INVAL;
  if (h->reaped || h->master < 0)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  if (kill (h->pid, 0) == 0)
    {
      ts_std_ret_bool (ret, true);
      return TS_OK;
    }
  ts_std_ret_bool (ret, false);
  return TS_OK;
}

static TS_Status
p_signal (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  int64_t sig = 0;
  int resolved;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "signal") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "signal");
  if (!h)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[1], &sig, error, "signal") == TS_OK)
    resolved = (int) sig;
  else
    {
      const char *name;
      ts_error_clear (error);
      if (ts_std_str (&argv[1], &name, error, "signal") != TS_OK)
        return TS_ERR_INVAL;
      if (!pty_signal_by_name (name, &resolved))
        {
          ts_error_set (error, TS_ERR_INVAL, 0, 0,
                        "signal: unknown signal '%s' "
                        "(pass a number or a name such as INT)", name);
          return TS_ERR_INVAL;
        }
    }
  if (h->master < 0 || h->reaped || kill (h->pid, resolved) != 0)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

/* winsize(pty) -> "rows,cols". */
static TS_Status
p_winsize (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  struct winsize ws;
  char buf[64];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "winsize") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "winsize");
  if (!h)
    return TS_ERR_INVAL;
  memset (&ws, 0, sizeof ws);
  if (h->master < 0)
    {
      /* Report the real reason; errno would be stale here. */
      ts_error_set (error, TS_ERR_SYSTEM, 0, 0, "winsize: pty is closed");
      return TS_ERR_SYSTEM;
    }
  if (ioctl (h->master, TIOCGWINSZ, &ws) != 0)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "winsize: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  snprintf (buf, sizeof buf, "%u,%u", (unsigned) ws.ws_row,
            (unsigned) ws.ws_col);
  return ts_std_ret_str (ret, buf, error);
}

/* set_winsize(pty, rows, cols) -> bool.  The kernel raises SIGWINCH in the
 * terminal's foreground process group, so this needs no explicit signalling. */
static TS_Status
p_set_winsize (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
               TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  int64_t rows, cols;
  struct winsize ws;
  (void) ud;
  if (ts_std_argc (vm, argc, 3, 3, error, "set_winsize") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "set_winsize");
  if (!h)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[1], &rows, error, "set_winsize") != TS_OK
      || ts_std_int (&argv[2], &cols, error, "set_winsize") != TS_OK)
    return TS_ERR_INVAL;
  if (rows < 0 || rows > 65535 || cols < 0 || cols > 65535)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "set_winsize: bad size");
      return TS_ERR_INVAL;
    }
  memset (&ws, 0, sizeof ws);
  ws.ws_row = (unsigned short) rows;
  ws.ws_col = (unsigned short) cols;
  if (h->master < 0 || ioctl (h->master, TIOCSWINSZ, &ws) != 0)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

/* raw(pty) -> bool: put the line discipline into raw mode, saving the old
 * settings so restore() can put them back. */
static TS_Status
p_raw (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  struct termios tio;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "raw") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "raw");
  if (!h)
    return TS_ERR_INVAL;
  if (h->master < 0 || tcgetattr (h->master, &tio) != 0)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  if (!h->termios_saved)
    {
      h->saved_termios = tio;
      h->termios_saved = true;
    }
  cfmakeraw (&tio);
  if (tcsetattr (h->master, TCSANOW, &tio) != 0)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static TS_Status
p_restore (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "restore") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "restore");
  if (!h)
    return TS_ERR_INVAL;
  if (!h->termios_saved || h->master < 0
      || tcsetattr (h->master, TCSANOW, &h->saved_termios) != 0)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static TS_Status
p_close (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  int st;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "close") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "close");
  if (!h)
    return TS_ERR_INVAL;
  /* Give the line discipline back before the child is torn down. */
  if (h->termios_saved && h->master >= 0)
    {
      tcsetattr (h->master, TCSANOW, &h->saved_termios);
      h->termios_saved = false;
    }
  if (!h->reaped && h->pid > 0)
    {
      kill (h->pid, SIGKILL);
      while (waitpid (h->pid, &st, 0) < 0 && errno == EINTR)
        ;
      h->reaped = true;
    }
  if (h->master >= 0)
    {
      close (h->master);
      h->master = -1;
    }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static const TS_FuncDef pty_funcs[] = {
  { "spawn", p_spawn, NULL },
  { "read", p_read, NULL },
  { "write", p_write, NULL },
  { "wait", p_wait, NULL },
  { "close", p_close, NULL },
  { "read_ready", p_read_ready, NULL },
  { "pid", p_pid, NULL },
  { "alive", p_alive, NULL },
  { "signal", p_signal, NULL },
  { "winsize", p_winsize, NULL },
  { "set_winsize", p_set_winsize, NULL },
  { "raw", p_raw, NULL },
  { "restore", p_restore, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_pty_module = { "std.pty", pty_funcs };
