#include "DiftrayWM-Plugin.h"
static const diftraywm_api_t *host;
static void command(const char **tokens, int count) {
  host->emit_status(count == 2 ? tokens[1] : "fixture command");
}
static void event(void *data) { host->emit_status((const char *)data); }
int diftraywm_plugin_init(const diftraywm_api_t *api) {
  host = api;
  api->register_command("fixture", command, DIFTRAYWM_COMMAND_SCOPE_CELL);
  api->subscribe_frame(event, "plugin frame");
  api->subscribe_view_event(event, "plugin view");
  api->subscribe_input_event(event, "plugin input");
#ifdef FAIL_INIT
  return 1;
#else
  return 0;
#endif
}
void diftraywm_plugin_cleanup(void) { host->unregister_command("fixture"); }
const char *diftraywm_plugin_version(void) { return "1.0"; }
