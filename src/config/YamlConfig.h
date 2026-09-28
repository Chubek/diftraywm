#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct DiftrayYamlSection {
#define CONFIG_FIELD(key) char *key;
#include "ConfigFields.def"
#undef CONFIG_FIELD
} DiftrayYamlSection;
typedef struct DiftrayYaml {
  DiftrayYamlSection *general;
  DiftrayYamlSection *terminal;
} DiftrayYaml;
int diftray_yaml_load(const char *, size_t, DiftrayYaml **, char *, size_t);
void diftray_yaml_free(DiftrayYaml *);
#ifdef __cplusplus
}
#endif
