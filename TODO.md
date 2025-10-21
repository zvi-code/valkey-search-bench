# TODO: Enhancements and Planned Features

This document tracks planned enhancements and feature ideas for the valkey-search-benchmark project.

## Loader Enhancements

### 1. Ground Truth Query Vector Insertion
**Status:** Planned  
**Description:** Add option to insert all ground truth query vectors into the index.  
**Benefits:** Enables more comprehensive testing and validation scenarios.

### 2. Index Configuration Verification
**Status:** Planned  
**Description:** Verify index configuration for existing indexes. If the configuration doesn't match what's requested, provide option to clean/recreate the index.  
**Benefits:** Prevents test failures due to configuration mismatches and ensures consistency.

### 3. Ground Truth Generation via Flat Search
**Status:** Planned  
**Description:** Generate ground truth vectors using flat search for existing indexes.  
**Benefits:** Allows creation of ground truth data without external dependencies.

### 4. Runtime Configuration Management
**Status:** Planned  
**Description:** Add ability to set configurations before a test is run, including:
- IO threads
- Number of worker threads
- Other engine-side configurations

**Benefits:** More flexible testing scenarios without manual server configuration changes.

### 5. Test Stage and Tag Reporting
**Status:** Planned  
**Description:** Report `test stage` and `test tag` for external tools to collect profiling data.  
**Benefits:** Better integration with profiling and monitoring tools; easier correlation of metrics with test phases.

### 6. Persistent Configuration Storage
**Status:** Planned  
**Description:** Save last configuration in a file. If file exists, use the configuration for any argument not provided by the user.  
**Exclusions:** Should NOT include:
- `-t` option (the command)
- `-h` argument (the host)

**Benefits:** Reduces repetitive command-line arguments and improves user experience.

### 7. Expiry Support
**Status:** Planned  
**Description:** Add ability to set TTL/expiry on inserted vectors.  
**Benefits:** Test scenarios involving data expiration and cache eviction.

### 8. Numeric and Tag Filters
**Status:** Planned  
**Description:** Add ability to set numeric and tag filters for queries.  
**Benefits:** Test filtered search scenarios and mixed workloads.

### 9. Variable Tag Lengths
**Status:** Planned  
**Description:** Add ability to set different tag lengths for testing.  
**Benefits:** Test impact of metadata size on performance.

### 10. Dynamic Thread and Client Scaling
**Status:** Planned  
**Description:** Add dynamic thread and client count adjustment without terminating threads.  
**Benefits:** Test dynamic scaling scenarios and eliminate warmup overhead between tests.

### 11. Valkey Logic Encapsulation
**Status:** Planned  
**Description:** Encapsulate Valkey-specific logic to prepare for future extensions to other vector databases.  
**Benefits:** Better code organization and easier support for multiple backends.

### 17. Dataset Extension
**Status:** Planned  
**Description:** Extend dataset by shifting existing dataset by the diameter of current dataset.  
**Note:** Will not work for cosine similarity.  
**Benefits:** Create larger synthetic datasets from existing ones.

### 18. JSON Support
**Status:** Planned  
**Description:** Add support for search data in JSON data type (HASH is the default).
**Benefits:** Improved usability and integration with external tools.

## 19. Allow Range based address specification
**Status:** Planned  
**Description:** Allow specifying a range of addresses not starting from 0.  
**Benefits:** More flexible data addressing schemes.

### 20. Support Multiple Indexes
**Status:** Planned  
**Description:** Add support for multiple indexes in a single benchmark run.  
**Benefits:** Test scenarios involving multiple indexes and their interactions.

### 21. Support nested key addressing
**Status:** Planned  
**Description:** Support nested key addressing like fields in hash or json data types. Allow specifying field ranges per key. So you can run random hset load, with varying number of fields in key and length.
**Benefits:** More realistic testing scenarios with complex data structures.

### 22. Extend support of delete operations
**Status:** Planned  
**Description:** Extend delete operations to support deleting by range, by % or by capacity target.  
**Benefits:** More flexible data management scenarios.

### 23. Support for mixed workloads + hybrid search
**Status:** Planned  
**Description:** Add support for mixed workloads involving different operation types (e.g., search, insert, delete) in a single benchmark run. This will also support having non-search operations running in the background while search operations are being benchmarked.
**Benefits:** More realistic testing scenarios that mimic production workloads.

### 24. Evaluate base latency
**Status:** ✅ Completed  
**Description:** Measure baseline network latency using PING commands at the beginning of a benchmark run. The baseline is measured with 10,000 PING operations using single-threaded, single-client configuration to establish pure network RTT. The baseline latency is displayed separately and included in both console output and CSV exports, showing processing overhead (operation latency - baseline latency).
**Benefits:** More accurate latency measurements by separating network overhead from operation-specific processing time.
**Implementation:** Enabled by default (no flags needed). Runs silently before benchmarks. Use `--no-baseline` to disable. Results include avg, p50, p90, p95, p99, and max latencies in both console and CSV output.


---
## Wrapper Scripts Enhancements

### 1. Max QPS at Target Recall
**Status:** Planned  
**Description:** Add wrapper to find maximum QPS achievable at a specified recall threshold.  
**Benefits:** Automated performance envelope discovery.

### 2. Max QPS at Recall and Latency Thresholds
**Status:** Planned  
**Description:** Add wrapper to find maximum QPS while maintaining both recall and latency thresholds.  
**Benefits:** More realistic performance testing with SLA constraints.

### 3. Optimal Configuration Discovery
**Status:** Planned  
**Description:** Add wrapper to find optimal thread and client count configurations.  
**Benefits:** Automated tuning for specific hardware and workload combinations.

### 4. Profiling Integration with Test Stages
**Status:** Planned  
**Description:** Use `test stage` and `test tag` to trigger data collectors while in specific stages and tag the output accordingly.  
**Benefits:** Automated profiling workflow with properly labeled data.

### 5. Memory Saturation Testing
**Status:** Planned  
**Description:** Wrapper for testing with 100% memory utilization.  
**Benefits:** Test behavior under memory pressure and eviction scenarios.

### 6. Payload Impact Testing
**Status:** Planned  
**Description:** Add wrapper to test the impact of different payload sizes and types.  
**Benefits:** Understand memory and performance tradeoffs with different metadata configurations.

---

## Priority Levels (TBD)

Items should be prioritized based on:
- User demand
- Implementation complexity
- Dependencies between features
- Impact on testing capabilities

## Contributing

When implementing any of these features:
1. Update this document with implementation status
2. Add relevant documentation to README.md or ADVANCED.md
3. Include tests where applicable
4. Update BENCHMARKING.md if the feature affects benchmarking workflows

---

## Implementation Plan: #6 Persistent Configuration Storage

### Overview
Implement a configuration persistence system that saves the last used configuration to a file and automatically loads it for subsequent runs when arguments are not explicitly provided.

### Goals
- Reduce repetitive command-line arguments for users
- Maintain configuration state between runs
- Allow selective override of saved configuration
- Exclude transient options (command `-t`, host `-h`) from persistence

### Design Decisions

#### Configuration File
- **Location**: `~/.valkey-benchmark/config.json` or `./.valkey-benchmark.json` (workspace-specific)
- **Format**: JSON for human readability and easy parsing
- **Scope**: User preference (global vs. workspace-local configuration)

#### Storage Strategy
```json
{
  "version": "1.0",
  "last_updated": "2025-10-21T12:34:56Z",
  "config": {
    "dataset": "openai-large-5m",
    "num_clients": 10,
    "num_threads": 4,
    "ef_search": 100,
    "index_type": "HNSW",
    "m": 16,
    "ef_construction": 200,
    "distance_metric": "L2",
    "dimension": 1536,
    "batch_size": 1000
  }
}
```

#### Excluded Parameters
Must NOT be persisted:
- `-t` (test command/operation)
- `-h` (host address)
- `--help`
- `--version`

### Implementation Steps

#### Phase 1: Core Infrastructure
1. **Create configuration module** (`config_persist.c`, `config_persist.h`)
   - Define configuration structure
   - Implement JSON serialization/deserialization (consider using cJSON or similar)
   - Add file I/O functions (read/write/create)

2. **Add configuration file path resolution**
   - Check for workspace-local config first (`./.valkey-benchmark.json`)
   - Fall back to user-global config (`~/.valkey-benchmark/config.json`)
   - Create directories as needed

3. **Implement configuration merge logic**
   - Load saved configuration
   - Override with command-line arguments
   - Maintain command-line precedence

#### Phase 2: Integration with Argument Parsing
1. **Modify `valkey-benchmark.c` argument parsing**
   - Load configuration before parsing arguments
   - Use saved values as defaults
   - Apply command-line overrides

2. **Add configuration save trigger**
   - Save configuration after successful argument validation
   - Skip saving if `--no-save-config` flag is present
   - Update timestamp on each save

3. **Add control flags**
   - `--save-config`: Force save configuration
   - `--no-save-config`: Skip saving this run
   - `--clear-config`: Delete saved configuration
   - `--show-config`: Display current effective configuration

#### Phase 3: Validation and Error Handling
1. **Configuration validation**
   - Verify JSON structure on load
   - Handle corrupted configuration files gracefully
   - Validate version compatibility

2. **Error handling**
   - Gracefully handle missing files (first run)
   - Handle permission errors
   - Handle invalid JSON
   - Provide informative error messages

3. **Migration support**
   - Version field for future schema changes
   - Migration functions for config format updates

#### Phase 4: User Features
1. **Verbose output**
   - Show which configuration was loaded (if `-v` flag)
   - Display configuration source (saved vs. defaults vs. CLI)
   - Show final merged configuration in verbose mode

2. **Configuration inspection**
   - Implement `--show-config` to display:
     - Configuration file path
     - Saved values
     - Command-line overrides
     - Final effective configuration

3. **Configuration management**
   - Implement `--clear-config` to reset to defaults
   - Consider `--edit-config` to open in editor
   - Document configuration file format

#### Phase 5: Testing
1. **Unit tests**
   - Test configuration save/load
   - Test merge logic with various combinations
   - Test excluded parameter handling
   - Test error conditions

2. **Integration tests**
   - Test full workflow: save → load → override
   - Test workspace-local vs. global config precedence
   - Test concurrent access (if applicable)

3. **Edge cases**
   - Empty configuration file
   - Partial configuration
   - Invalid JSON
   - Permission issues

### Dependencies
- JSON parsing library (cJSON, jansson, or similar)
- File system utilities (already present)
- Configuration structure definitions

### Code Locations
Primary files to modify:
- `loader/valkey-benchmark.c` - Main argument parsing
- `loader/config_persist.c` (new) - Configuration persistence logic
- `loader/config_persist.h` (new) - Configuration persistence interface
- `loader/utils.c` - Utility functions if needed
- `loader/CMakeLists.txt` - Add new files to build

### Compatibility Considerations
- Ensure backward compatibility (tool works without config file)
- Don't break existing scripts/workflows
- Make persistence opt-out if needed
- Document behavior changes in README.md

### Example Usage

```bash
# First run - specify all options
./valkey-benchmark -t search -d openai-large-5m -c 10 --threads 4 --ef-search 100

# Second run - reuses saved configuration
./valkey-benchmark -t search

# Override specific parameters
./valkey-benchmark -t search -c 20  # Uses saved config but with 20 clients

# View current configuration
./valkey-benchmark --show-config

# Clear saved configuration
./valkey-benchmark --clear-config

# Run without saving this configuration
./valkey-benchmark -t search -d test-dataset --no-save-config
```

### Documentation Updates
- **README.md**: Add section on configuration persistence
- **ADVANCED.md**: Detail configuration file format and management
- **BENCHMARKING.md**: Explain how persistence affects benchmarking workflows
- **Help text**: Update `--help` output with new flags

### Success Criteria
- [ ] Configuration is saved automatically after successful runs
- [ ] Saved configuration is loaded and applied correctly
- [ ] Command-line arguments override saved values
- [ ] `-t` and `-h` are never persisted
- [ ] Configuration file format is documented
- [ ] Error handling covers all edge cases
- [ ] Tests pass for all scenarios
- [ ] Documentation is updated
- [ ] No breaking changes to existing workflows

### Future Enhancements
- Multiple named configuration profiles
- Environment-specific configurations (dev/staging/prod)
- Configuration templates for common scenarios
- Configuration import/export functionality
- Shell completion based on saved configs

---

*Last Updated: October 21, 2025*
