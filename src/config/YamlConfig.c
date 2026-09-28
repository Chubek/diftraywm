#include "YamlConfig.h"
#include <cyaml/cyaml.h>
#include <stdio.h>

static const cyaml_schema_field_t fields[] = {
#define CONFIG_FIELD(key) CYAML_FIELD_STRING_PTR(#key, CYAML_FLAG_OPTIONAL, DiftrayYamlSection, key, 0, 65536),
#include "ConfigFields.def"
#undef CONFIG_FIELD
  CYAML_FIELD_END
};
static const cyaml_schema_field_t sections[] = {
  CYAML_FIELD_MAPPING_PTR("general", CYAML_FLAG_OPTIONAL, DiftrayYaml, general, fields),
  CYAML_FIELD_MAPPING_PTR("terminal", CYAML_FLAG_OPTIONAL, DiftrayYaml, terminal, fields),
  CYAML_FIELD_END
};
static const cyaml_schema_value_t schema = {
  CYAML_VALUE_MAPPING(CYAML_FLAG_POINTER, DiftrayYaml, sections)
};
static const cyaml_config_t config = {
  .mem_fn = cyaml_mem,
  .log_level = CYAML_LOG_ERROR,
};
int diftray_yaml_load(const char *source, size_t length, DiftrayYaml **document,
                     char *error, size_t capacity) {
  cyaml_err_t result = cyaml_load_data((const unsigned char *)source, length,
      &config, &schema, (cyaml_data_t **)document, NULL);
  if (result != CYAML_OK) snprintf(error, capacity, "YAML: %s", cyaml_strerror(result));
  return result == CYAML_OK;
}
void diftray_yaml_free(DiftrayYaml *document) {
  cyaml_free(&config, &schema, document, 0);
}
