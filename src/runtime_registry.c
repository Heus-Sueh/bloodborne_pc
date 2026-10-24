#include "runtime_registry.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const struct { const char *nid, *symbol; } import_names[] = {
#include "import_names.inc"
#include "import_names_extra.inc"
};

#define NID_TABLE_SIZE 2048
#define REGISTRY_SIZE 4096

static int16_t nid_index_table[NID_TABLE_SIZE];
static int nid_index_ready = 0;

static inline uint32_t hash_str_len(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static inline uint32_t hash_str(const char *s) {
    return hash_str_len(s, strlen(s));
}

static void init_nid_index(void) {
    if (nid_index_ready) return;
    for (size_t i = 0; i < NID_TABLE_SIZE; ++i) nid_index_table[i] = -1;
    const size_t count = sizeof(import_names) / sizeof(import_names[0]);
    for (size_t i = 0; i < count; ++i) {
        const char *nid = import_names[i].nid;
        size_t len = strcspn(nid, "#");
        uint32_t h = hash_str_len(nid, len);
        size_t mask = NID_TABLE_SIZE - 1;
        size_t slot = h & mask;
        while (nid_index_table[slot] != -1) {
            slot = (slot + 1) & mask;
        }
        nid_index_table[slot] = (int16_t)i;
    }
    nid_index_ready = 1;
}

const char *runtime_symbol(const char *nid) {
    if (!nid) return NULL;
    if (!nid_index_ready) init_nid_index();
    const size_t len = strcspn(nid, "#");
    const uint32_t h = hash_str_len(nid, len);
    const size_t mask = NID_TABLE_SIZE - 1;
    size_t slot = h & mask;
    while (nid_index_table[slot] != -1) {
        const int idx = nid_index_table[slot];
        const char *entry = import_names[idx].nid;
        if (!strncmp(nid, entry, len) && (entry[len] == '#' || entry[len] == '\0')) {
            return import_names[idx].symbol;
        }
        slot = (slot + 1) & mask;
    }
    return NULL;
}

typedef struct {
    const char *name;
    void *address;
    uint32_t hash;
    uint8_t is_data;
    uint8_t occupied;
} RegistrySlot;

static RegistrySlot registry_table[REGISTRY_SIZE];

static void insert_registry_slot(const char *key, void *addr, int is_data) {
    if (!key || !*key) return;
    const uint32_t h = hash_str(key);
    const size_t mask = REGISTRY_SIZE - 1;
    size_t slot = h & mask;
    while (registry_table[slot].occupied) {
        if (registry_table[slot].hash == h &&
            !strcmp(registry_table[slot].name, key) &&
            registry_table[slot].is_data == is_data) {
            registry_table[slot].address = addr;
            return;
        }
        slot = (slot + 1) & mask;
    }
    registry_table[slot].name = key;
    registry_table[slot].address = addr;
    registry_table[slot].hash = h;
    registry_table[slot].is_data = (uint8_t)is_data;
    registry_table[slot].occupied = 1;
}

void runtime_register_exports(const RuntimeExport *table, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!table[i].name) continue;
        insert_registry_slot(table[i].name, table[i].function, 0);
        const char *hash_pos = strchr(table[i].name, '#');
        if (hash_pos) {
            size_t len = (size_t)(hash_pos - table[i].name);
            char *prefix = malloc(len + 1);
            if (prefix) {
                memcpy(prefix, table[i].name, len);
                prefix[len] = '\0';
                insert_registry_slot(prefix, table[i].function, 0);
            }
        }
    }
}

void runtime_register_data_exports(const RuntimeExport *table, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!table[i].name) continue;
        insert_registry_slot(table[i].name, table[i].function, 1);
        const char *hash_pos = strchr(table[i].name, '#');
        if (hash_pos) {
            size_t len = (size_t)(hash_pos - table[i].name);
            char *prefix = malloc(len + 1);
            if (prefix) {
                memcpy(prefix, table[i].name, len);
                prefix[len] = '\0';
                insert_registry_slot(prefix, table[i].function, 1);
            }
        }
    }
}

static uintptr_t lookup_key(const char *key, size_t len, int is_data) {
    if (!key || len == 0) return 0;
    const uint32_t h = hash_str_len(key, len);
    const size_t mask = REGISTRY_SIZE - 1;
    size_t slot = h & mask;
    while (registry_table[slot].occupied) {
        if (registry_table[slot].hash == h &&
            registry_table[slot].is_data == is_data &&
            !strncmp(registry_table[slot].name, key, len) &&
            registry_table[slot].name[len] == '\0') {
            return (uintptr_t)registry_table[slot].address;
        }
        slot = (slot + 1) & mask;
    }
    return 0;
}

uintptr_t runtime_registry_resolve(const char *name, int is_data) {
    if (!name) return 0;
    size_t len = strcspn(name, "#");
    uintptr_t addr = lookup_key(name, len, is_data);
    if (addr) return addr;

    if (name[len] != '\0') {
        addr = lookup_key(name, strlen(name), is_data);
        if (addr) return addr;
    }

    const char *sym = runtime_symbol(name);
    if (sym) {
        addr = lookup_key(sym, strlen(sym), is_data);
        if (addr) return addr;
    }

    return 0;
}

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    const char *symbol = runtime_symbol(nid);
    if (!symbol) return 0;
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(symbol, table[i].name)) return (uintptr_t)table[i].function;
    }
    return 0;
}

void runtime_libc_register(void);
void runtime_mutex_register(void);
void runtime_rwlock_register(void);
void runtime_sema_register(void);
void runtime_thread_register(void);
void runtime_content_register(void);
void runtime_kernel_register(void);
void runtime_file_register(void);
void runtime_memory_register(void);
void runtime_services_register(void);
void runtime_ajm_register(void);
void runtime_audio_register(void);
void runtime_pad_register(void);
void runtime_rtc_register(void);
void runtime_savedata_register(void);
void runtime_toypad_register(void);
void runtime_ngs2_register(void);
void runtime_compat_register(void);

static int registry_initialized = 0;

void runtime_registry_init(void) {
    if (registry_initialized) return;
    init_nid_index();
    registry_initialized = 1;
    runtime_libc_register();
    runtime_mutex_register();
    runtime_rwlock_register();
    runtime_sema_register();
    runtime_thread_register();
    runtime_content_register();
    runtime_kernel_register();
    runtime_file_register();
    runtime_memory_register();
    runtime_services_register();
    runtime_ajm_register();
    runtime_audio_register();
    runtime_pad_register();
    runtime_rtc_register();
    runtime_savedata_register();
    runtime_toypad_register();
    runtime_ngs2_register();
    runtime_compat_register();
}
