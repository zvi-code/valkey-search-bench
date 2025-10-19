# Valkey Benchmark Module System

This document describes the modular benchmark architecture introduced to valkey-benchmark and provides instructions for extending it with custom data types, placeholder replacements, and commands.

## Overview

The valkey-benchmark tool has been refactored from a monolithic architecture to a modular system that allows:
- **Pluggable benchmark modules** for different data types and use cases
- **Custom placeholder replacement** strategies per module
- **Thread-safe per-command state management**
- **Dynamic test discovery** without hardcoded test lists
- **Unified processing pipeline** for all modules

## Architecture

### Core Components

1. **Module Interface** (`valkey-benchmark-module.h`): Defines the callback-based interface for benchmark modules
2. **Core Module** (`valkey-benchmark-core.c`): Implements all standard Valkey benchmark tests
3. **Main Program** (`valkey-benchmark.c`): Module loader and execution engine
4. **Module System**: Registration, initialization, and lifecycle management

### Key Design Principles

- **No hardcoded test lists**: Tests are discovered dynamically from modules
- **Per-command placeholder state**: Thread-safe replacement without global state
- **Unified pipeline**: All modules processed through same callback chain
- **Self-contained modules**: Each module manages its own tests, titles, and logic

## Module Interface Reference

```c
typedef struct benchmark_module {
    /* Module metadata */
    const char *name;
    int version;
    
    /* Lifecycle management */
    int (*init)(void **private_data, const char *config_str);
    void (*cleanup)(void *private_data);
    int (*pre_benchmark)(void *private_data);
    void (*post_benchmark)(void *private_data);
    
    /* Per-command placeholder processing (thread-safe) */
    int (*init_placeholders)(void *module_ctx, const char *cmd, size_t cmd_len, 
                           placeholder_info_t **placeholder_state);
    int (*replace_placeholders)(void *module_ctx, char *cmd, size_t cmd_len, 
                              uint64_t seq, int pipeline_idx, const placeholder_info_t *placeholders);
    void (*free_placeholders)(placeholder_info_t *placeholder_state);
    
    /* Command transformation pipeline */
    int (*should_process_command)(void *module_ctx, const char *cmd, size_t len);
    char* (*transform_key)(uint64_t worker_id, void *module_ctx, struct _client *c, 
                          const char *key, size_t *new_len);
    
    /* Test management */
    int (*get_test_command)(uint64_t worker_id, void *module_ctx, const char *test_name,
                          char **cmd_template, int *len);
    int (*get_available_tests)(void *module_ctx, const char ***test_names, const char ***test_titles);
    
    /* Help and configuration */
    void (*print_help)(void);
    int (*get_config_help)(char *buf, size_t buf_len);
} benchmark_module_t;
```

## Creating a Custom Module

### 1. Module Structure Template

```c
#include "valkey-benchmark-module.h"
#include <stdlib.h>
#include <string.h>

/* Module private data structure */
typedef struct {
    char *custom_config;
    int some_parameter;
    /* Add your module-specific data */
} my_module_data_t;

/* Test command definitions */
typedef struct {
    const char *name;
    int (*generator)(my_module_data_t *data, char **cmd_template, int *cmd_len);
} my_test_entry_t;

static my_test_entry_t my_tests[] = {
    {"my_custom_test", generate_my_test},
    {"another_test", generate_another_test},
    {NULL, NULL}
};

/* Module implementation functions */
static int my_module_init(void **private_data, const char *config_str) {
    my_module_data_t *data = malloc(sizeof(my_module_data_t));
    if (!data) return -1;
    
    /* Initialize your module data */
    data->custom_config = config_str ? strdup(config_str) : NULL;
    data->some_parameter = 42;  /* default value */
    
    *private_data = data;
    return 0;
}

static void my_module_cleanup(void *private_data) {
    my_module_data_t *data = (my_module_data_t*)private_data;
    if (data) {
        free(data->custom_config);
        free(data);
    }
}

static int my_module_get_available_tests(void *private_data, const char ***test_names, const char ***test_titles) {
    /* Count tests */
    int count = 0;
    for (my_test_entry_t *entry = my_tests; entry->name; entry++) {
        count++;
    }
    
    /* Allocate arrays */
    const char **names = malloc((count + 1) * sizeof(const char*));
    const char **titles = malloc((count + 1) * sizeof(const char*));
    
    if (!names || !titles) {
        free(names);
        free(titles);
        return -1;
    }
    
    /* Fill arrays */
    int i = 0;
    for (my_test_entry_t *entry = my_tests; entry->name; entry++) {
        names[i] = entry->name;
        titles[i] = entry->name; /* or create custom titles */
        i++;
    }
    
    names[count] = NULL;
    titles[count] = NULL;
    
    *test_names = names;
    *test_titles = titles;
    
    return count;
}

/* Module interface definition */
static benchmark_module_t my_module = {
    .name = "my_custom_module",
    .version = 1,
    .init = my_module_init,
    .cleanup = my_module_cleanup,
    .get_available_tests = my_module_get_available_tests,
    .get_test_command = my_module_get_test_command,
    /* Add other callbacks as needed */
};

/* Module entry point */
benchmark_module_t *get_my_module(void) {
    return &my_module;
}
```

### 2. Implementing Custom Placeholder Replacement

For modules that need custom placeholder replacement logic:

```c
/* Custom placeholder state structure */
typedef struct {
    int custom_counter;
    char *special_data;
    /* Your custom state */
} my_placeholder_state_t;

static int my_init_placeholders(void *module_ctx, const char *cmd, size_t cmd_len, 
                               placeholder_info_t **placeholder_state) {
    placeholder_info_t *state = malloc(sizeof(placeholder_info_t));
    if (!state) return -1;
    
    /* Initialize base placeholder state */
    memset(state, 0, sizeof(placeholder_info_t));
    
    /* Add custom state */
    my_placeholder_state_t *custom_state = malloc(sizeof(my_placeholder_state_t));
    if (!custom_state) {
        free(state);
        return -1;
    }
    
    custom_state->custom_counter = 0;
    custom_state->special_data = strdup("custom_value");
    
    state->module_private = custom_state;  /* Store custom state */
    *placeholder_state = state;
    return 0;
}

static int my_replace_placeholders(void *module_ctx, char *cmd, size_t cmd_len, 
                                  uint64_t seq, int pipeline_idx, const placeholder_info_t *placeholders) {
    my_placeholder_state_t *custom_state = (my_placeholder_state_t*)placeholders->module_private;
    
    /* Implement your custom placeholder replacement logic */
    char *pos = strstr(cmd, "__my_custom_placeholder__");
    if (pos) {
        char replacement[32];
        snprintf(replacement, sizeof(replacement), "custom_%d", custom_state->custom_counter++);
        
        /* Replace in-place (be careful with buffer bounds) */
        /* Implementation depends on your specific needs */
    }
    
    return 0;  /* Success */
}

static void my_free_placeholders(placeholder_info_t *placeholder_state) {
    if (placeholder_state && placeholder_state->module_private) {
        my_placeholder_state_t *custom_state = (my_placeholder_state_t*)placeholder_state->module_private;
        free(custom_state->special_data);
        free(custom_state);
    }
    free(placeholder_state);
}
```

### 3. Command Generation Examples

```c
static int generate_my_test(my_module_data_t *data, char **cmd_template, int *cmd_len) {
    /* Example: Custom SET command with special formatting */
    const char *template = "*3\r\n$3\r\nSET\r\n$12\r\nmy_key:__my_custom_placeholder__\r\n$10\r\n__data__\r\n";
    
    *cmd_template = strdup(template);
    if (!*cmd_template) return -1;
    
    *cmd_len = strlen(template);
    return 0;
}

static int generate_json_test(my_module_data_t *data, char **cmd_template, int *cmd_len) {
    /* Example: JSON.SET command for RedisJSON compatibility */
    const char *template = "*4\r\n$8\r\nJSON.SET\r\n$15\r\njson_key:__rand_int__\r\n$1\r\n$\r\n$20\r\n{\"value\":__rand_int__}\r\n";
    
    *cmd_template = strdup(template);
    if (!*cmd_template) return -1;
    
    *cmd_len = strlen(template);
    return 0;
}
```

### 4. Adding Custom Data Types Support

For new data types (e.g., JSON, TimeSeries, Probabilistic):

```c
/* Example: JSON module */
typedef struct {
    char *json_template;
    int max_depth;
    int array_size;
} json_module_data_t;

static json_test_entry_t json_tests[] = {
    {"json_set", generate_json_set},
    {"json_get", generate_json_get},
    {"json_mget", generate_json_mget},
    {"json_del", generate_json_del},
    {"json_arrappend", generate_json_arrappend},
    {NULL, NULL}
};

static int generate_json_set(json_module_data_t *data, char **cmd_template, int *cmd_len) {
    /* Generate JSON.SET command with complex JSON structure */
    char json_value[512];
    snprintf(json_value, sizeof(json_value), 
             "{\"id\":__rand_int__,\"name\":\"item__rand_int__\",\"data\":[__rand_int__,__rand_int__]}");
    
    int max_len = 1024;
    *cmd_template = malloc(max_len);
    if (!*cmd_template) return -1;
    
    *cmd_len = snprintf(*cmd_template, max_len,
                       "*4\r\n$8\r\nJSON.SET\r\n$16\r\njson:__rand_int__\r\n$1\r\n$\r\n$%d\r\n%s\r\n",
                       (int)strlen(json_value), json_value);
    
    return 0;
}
```

## Integration Steps

### 1. Build System Integration

Add your module to `CMakeLists.txt`:

```cmake
# Add your module source
target_sources(valkey-benchmark PRIVATE
    src/valkey-benchmark-my-module.c
)
```

### 2. Registration in Main Program

In `valkey-benchmark.c`, add module registration:

```c
static void initCustomModules(void) {
    extern benchmark_module_t *get_my_module(void);
    
    benchmark_module_t *my_mod = get_my_module();
    if (registerModule(my_mod, "param1=value1;param2=value2") >= 0) {
        printf("Registered module: %s\n", my_mod->name);
    }
}
```

### 3. Command Line Options

Add module-specific options to `parseOptions()`:

```c
} else if (!strcmp(argv[i], "--enable-my-module")) {
    config.enable_my_module = 1;
} else if (!strcmp(argv[i], "--my-module-config")) {
    if (lastarg) goto invalid;
    config.my_module_config = argv[++i];
```

## Testing Your Module

### 1. Basic Functionality Test

```bash
# Test module registration
./valkey-benchmark --search --enable-my-module -n 0

# Test specific commands
./valkey-benchmark --enable-my-module -t my_custom_test -n 1000 -c 10

# Test with custom placeholders
./valkey-benchmark --enable-my-module -t my_custom_test -r 10000 -n 1000
```

### 2. Thread Safety Test

```bash
# Test with multiple threads
./valkey-benchmark --enable-my-module -t my_custom_test --threads 4 -n 10000 -c 50
```

### 3. Pipeline Test

```bash
# Test pipelining
./valkey-benchmark --enable-my-module -t my_custom_test -P 10 -n 10000
```

## Best Practices

### 1. Memory Management
- Always free allocated memory in cleanup callbacks
- Use proper error handling for malloc failures
- Avoid memory leaks in placeholder processing

### 2. Thread Safety
- Use per-command placeholder state, not global variables
- Ensure your module data is read-only during benchmarks
- Use atomic operations for shared counters if needed

### 3. Performance
- Pre-allocate command templates when possible
- Minimize string operations in hot paths
- Use efficient placeholder replacement algorithms

### 4. Error Handling
- Return appropriate error codes from callbacks
- Provide meaningful error messages
- Handle edge cases gracefully

## Advanced Features

### 1. Cluster Support

Implement cluster routing logic:

```c
static int my_apply_cluster_routing(uint64_t worker_id, void *module_ctx, client *c) {
    /* Custom cluster key routing logic */
    return 0;
}
```

### 2. Custom Latency Tracking

```c
static int my_get_latency_info(uint64_t worker_id, void *module_ctx, char *buf, size_t buf_len) {
    /* Return custom latency metrics */
    return snprintf(buf, buf_len, "custom_metric=%.2f", my_custom_latency);
}
```

### 3. Reply Processing

```c
static void my_process_reply(void *module_ctx, void *reply) {
    /* Process server replies for validation or metrics */
}
```

## Migration from Legacy Code

If you have existing benchmark code to migrate:

1. **Extract test definitions**: Move hardcoded commands to generator functions
2. **Implement module interface**: Create the required callbacks
3. **Update placeholder logic**: Convert global state to per-command state
4. **Test thoroughly**: Ensure thread safety and performance

## Troubleshooting

### Common Issues

1. **Compilation errors**: Check include paths and function signatures
2. **Segmentation faults**: Usually memory management issues in callbacks
3. **Wrong test results**: Verify placeholder replacement logic
4. **Performance degradation**: Profile placeholder processing overhead

### Debug Tips

1. Use `--search -n 0` to test module registration without running benchmarks
2. Add debug prints to callback functions during development
3. Test with single-threaded mode first (`--threads 0`)
4. Verify command generation with small request counts

## Examples

See the core module (`valkey-benchmark-core.c`) for a complete reference implementation that includes:
- All standard Valkey commands (SET, GET, LPUSH, etc.)
- Complex placeholder replacement logic
- Thread-safe per-command state management
- Dynamic test discovery
- Proper memory management

This architecture provides a solid foundation for extending valkey-benchmark with new data types, custom benchmarking scenarios, and specialized testing requirements while maintaining performance and thread safety.