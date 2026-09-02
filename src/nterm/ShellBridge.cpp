#include <unistd.h>

bool diftraywm_shell_available(const char *path) {
  return path && *path && ::access(path, X_OK) == 0;
}
