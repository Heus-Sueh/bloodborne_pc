#ifndef BB_RUNTIME_REGISTRY_H
#define BB_RUNTIME_REGISTRY_H

#include <stdint.h>
#include <stddef.h>
#include "runtime_error.h"

#ifdef __cplusplus
extern "C" {
#endif

enum ExportKind {
    EXPORT_FUNCTION = 0,
    EXPORT_DATA = 1,
};

typedef struct {
    const char *name;
    void *function;
} RuntimeExport;

/* Initializes the symbol registry and NID lookup table */
void runtime_registry_init(void);

/* Register function exports into the central registry */
void runtime_register_exports(const RuntimeExport *table, size_t count);

/* Register data exports into the central registry */
void runtime_register_data_exports(const RuntimeExport *table, size_t count);

/* Lookup symbol in the central registry */
uintptr_t runtime_registry_resolve(const char *name, int is_data);

/* O(1) lookup of symbol name from NID */
const char *runtime_symbol(const char *nid);

/* Lookup helper for specific tables */
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);

#ifdef __cplusplus
}
#endif

#endif
