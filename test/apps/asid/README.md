# ASID Profiling and Testing Suite

This directory contains a comprehensive suite of tools and tests for profiling and analyzing the Address Space ID (ASID) implementation in Asterinas OS.

## Overview

The ASID (Address Space ID) system in Asterinas OS provides hardware-assisted address space isolation using x86-64 PCID (Process-Context Identifier) features. This profiling suite allows you to:

- Monitor ASID allocation and deallocation patterns
- Track TLB flush operations and their performance impact
- Analyze context switch efficiency
- Measure the effectiveness of ASID reuse strategies
- Identify performance bottlenecks in memory management

## Components

### 1. ASID Profiling Infrastructure (`ostd/src/mm/asid_profiling.rs`)

The core profiling module that tracks:
- **Allocation Statistics**: Total allocations, failures, generation rollovers
- **TLB Operations**: Different types of TLB flushes and their timing
- **Context Switches**: Frequency and performance of VM space activations
- **Search Operations**: Bitmap and map search efficiency
- **Per-ASID Metrics**: Individual ASID usage patterns

### 2. Test Applications

#### `asid_test.c` - Basic Functionality Test
Tests the correctness of the ASID mechanism by creating multiple threads that perform memory operations and verify data integrity.

**Features:**
- Creates 8 threads, each using 2MB of memory
- Performs random memory access patterns
- Verifies memory integrity to detect ASID-related bugs
- Reports success/failure for each thread

**Usage:**
```bash
cd /test && ./asid_test
```

#### `asid_time.c` - Performance Benchmark
Measures the performance impact of the ASID system under various workloads.

**Features:**
- Configurable number of threads and memory operations
- Measures memory access latency and throughput
- Calculates performance metrics and statistics
- Provides detailed timing analysis

**Usage:**
```bash
cd /test && ./asid_time
```

#### `asid_profiling_demo.c` - Stress Testing
Demonstrates ASID profiling by creating a complex workload that exercises all aspects of the ASID system.

**Features:**
- Creates multiple processes with multiple threads each
- Generates intensive memory access patterns
- Triggers frequent context switches
- Stresses ASID allocation and reuse mechanisms
- Runs for a configurable duration

**Usage:**
```bash
cd /test && ./asid_profiling_demo
```

#### `asid_profiler.c` - Statistics Viewer
A command-line utility to view and manage ASID profiling statistics.

**Features:**
- Display detailed ASID statistics
- Show efficiency metrics and performance analysis
- Print reports to kernel log
- Reset profiling counters
- Real-time monitoring capabilities

**Usage:**
```bash
# Display basic statistics
cd /test && ./asid_profiler

# Show efficiency metrics
cd /test && ./asid_profiler --efficiency

# Display all information
cd /test && ./asid_profiler --all

# Print detailed report to kernel log
cd /test && ./asid_profiler --log

# Reset all statistics
cd /test && ./asid_profiler --reset
```

### 3. Kernel Integration

#### ASID Allocation Module (`ostd/src/mm/asid_allocation.rs`)
Enhanced with profiling calls to track:
- Allocation timing and success rates
- Search operation efficiency
- Generation rollover events
- ASID reuse patterns

#### TLB Management (`ostd/src/arch/x86/mm/asid.rs`)
Instrumented to monitor:
- Different types of TLB invalidation operations
- Performance impact of INVPCID instructions
- Fallback behavior on systems without PCID support

#### VM Space Management (`ostd/src/mm/vm_space.rs`)
Tracks context switch performance:
- VM space activation timing
- TLB flush requirements during context switches
- ASID generation checks and updates

#### System Call Interface (`kernel/src/syscall/asid_profiling.rs`)
Provides userspace access to profiling data:
- Statistics retrieval
- Efficiency metrics calculation
- Report generation
- Counter reset functionality

## Profiling Metrics

### Basic Statistics
- **Allocations/Deallocations**: Total count and timing
- **Allocation Failures**: When ASID space is exhausted
- **Generation Rollovers**: ASID namespace resets
- **Active ASIDs**: Currently allocated ASID count

### TLB Operations
- **Single Address Flushes**: Page-specific invalidations
- **Single Context Flushes**: ASID-specific invalidations
- **All Context Flushes**: Global TLB invalidations
- **Performance Timing**: Cycles spent in TLB operations

### Context Switch Analysis
- **Total Context Switches**: VM space activations
- **Flush-Required Switches**: Context switches needing TLB flushes
- **Flush Percentage**: Efficiency metric for TLB management
- **Switch Timing**: Performance cost of context switches

### Efficiency Metrics
- **Allocation Success Rate**: Percentage of successful ASID allocations
- **Reuse Efficiency**: How effectively ASIDs are reused
- **Flush Efficiency**: Percentage of context switches avoiding TLB flushes
- **Performance Ratios**: Cycles per operation for different activities

## Usage Scenarios

### 1. Development and Debugging
```bash
# Run basic functionality test
./asid_test

# Check for any failures or memory corruption
# If test passes, ASID mechanism is working correctly
```

### 2. Performance Analysis
```bash
# Reset statistics before testing
./asid_profiler --reset

# Run performance benchmark
./asid_time

# View results
./asid_profiler --all
```

### 3. System Stress Testing
```bash
# Clear previous statistics
./asid_profiler --reset

# Run stress test with monitoring
./asid_profiling_demo &

# Monitor statistics in real-time (in another terminal)
watch -n 1 './asid_profiler --stats'

# View final results after stress test completes
./asid_profiler --efficiency
```

### 4. Regression Testing
```bash
# Establish baseline
./asid_profiler --reset
./asid_time
./asid_profiler --efficiency > baseline.txt

# After code changes, compare results
./asid_profiler --reset
./asid_time
./asid_profiler --efficiency > current.txt
diff baseline.txt current.txt
```

## Interpreting Results

### Good Performance Indicators
- **High Allocation Success Rate** (>99%): ASID allocation rarely fails
- **High Flush Efficiency** (>80%): Most context switches avoid TLB flushes
- **Low Generation Rollovers**: ASID space is efficiently utilized
- **Reasonable Timing**: Low cycles per allocation/context switch

### Performance Warning Signs
- **High Allocation Failures**: May indicate ASID exhaustion
- **Low Flush Efficiency**: Excessive TLB flushes hurt performance
- **Frequent Generation Rollovers**: ASID space may be too small
- **High Operation Latency**: Implementation may need optimization

### Optimization Opportunities
- **High Map Searches vs Bitmap Searches**: Bitmap allocator may be inefficient
- **Uneven ASID Usage**: Load balancing issues
- **High TLB Flush Times**: Hardware-specific optimization needed

## Configuration

### Build Options
The profiling system can be configured at build time:
- `ASID_PROFILING_ENABLED`: Enable/disable profiling (default: enabled)
- `ASID_DETAILED_LOGGING`: Enable detailed debug logging
- `ASID_PER_CPU_STATS`: Track per-CPU statistics (future enhancement)

### Runtime Configuration
Some aspects can be tuned at runtime:
- Statistics collection can be paused/resumed
- Individual metric categories can be enabled/disabled
- Sampling rates can be adjusted for overhead control

## Troubleshooting

### Common Issues

1. **"Failed to get ASID statistics" Error**
   - Ensure the `sys_asid_profiling` syscall is implemented
   - Check that the syscall number matches between kernel and userspace
   - Verify proper privileges for system monitoring

2. **All Statistics Show Zero**
   - Profiling may be disabled at build time
   - Statistics may have been recently reset
   - System may not support PCID (check PCID enabled flag)

3. **High Allocation Failures**
   - ASID space (4096 entries) may be exhausted
   - Consider increasing generation rollover threshold
   - Check for ASID leaks (allocations without deallocations)

4. **Poor Performance Results**
   - Verify PCID is enabled in hardware and kernel
   - Check for excessive debug logging overhead
   - Consider workload characteristics and system load

### Debug Tips

1. **Enable Detailed Logging**
   ```bash
   # View kernel logs for detailed ASID operations
   ./asid_profiler --log
   dmesg | grep ASID_PROF
   ```

2. **Monitor Real-Time**
   ```bash
   # Watch statistics change in real-time
   watch -n 1 './asid_profiler --stats'
   ```

3. **Compare Before/After**
   ```bash
   # Capture baseline and compare after changes
   ./asid_profiler > before.txt
   # ... make changes or run workload ...
   ./asid_profiler > after.txt
   diff before.txt after.txt
   ```

## Contributing

When modifying the ASID system:

1. **Add Profiling Calls**: New operations should include appropriate profiling
2. **Test Thoroughly**: Run all test applications before submitting changes
3. **Update Documentation**: Keep this README and code comments current
4. **Performance Analysis**: Use profiling data to validate optimizations

## Future Enhancements

Planned improvements to the profiling system:
- Per-CPU statistics for SMP systems
- Histogram-based timing analysis
- Automated performance regression detection
- Integration with system-wide performance monitoring
- Machine-readable output formats (JSON, CSV)
- Real-time alerting for performance anomalies 