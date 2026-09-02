#ifndef DIFTRAYWM_PLUGIN_H
#define DIFTRAYWM_PLUGIN_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum diftraywm_command_scope {
  DIFTRAYWM_COMMAND_SCOPE_CELL = 0,
  DIFTRAYWM_COMMAND_SCOPE_NCURSOR_GLOBAL = 1,
} diftraywm_command_scope_t;

typedef struct diftraywm_api diftraywm_api_t;

typedef int (*diftraywm_plugin_init_fn)(const diftraywm_api_t *api);
typedef void (*diftraywm_plugin_cleanup_fn)(void);
typedef const char *(*diftraywm_plugin_version_fn)(void);
typedef void (*diftraywm_command_callback_t)(const char **tokens, int token_count);

struct diftraywm_api {
  void (*log)(const char *message);
  void (*emit_status)(const char *message);
  void (*register_command)(const char *name,
                           diftraywm_command_callback_t callback,
                           diftraywm_command_scope_t scope);
  void (*unregister_command)(const char *name);
  void (*subscribe_frame)(void (*callback)(void *userdata), void *userdata);
  void (*subscribe_view_event)(void (*callback)(void *userdata), void *userdata);
  void (*subscribe_input_event)(void (*callback)(void *userdata), void *userdata);
};

int diftraywm_plugin_init(const diftraywm_api_t *api);
void diftraywm_plugin_cleanup(void);
const char *diftraywm_plugin_version(void);

#ifdef __cplusplus
}
#endif

#endif
