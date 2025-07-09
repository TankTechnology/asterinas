# ASID Profiling and Testing Suite

This directory contains a comprehensive suite of tools and tests for profiling and analyzing the Address Space ID (ASID) implementation in Asterinas OS.

## Overview

The ASID (Address Space ID) system in Asterinas OS provides hardware-assisted address space isolation using x86-64 PCID (Process-Context Identifier) features. This profiling suite allows you to:

- Monitor ASID allocation and deallocation patterns
- Track TLB flush operations and their performance impact
- Analyze context switch efficiency
- Measure the effectiveness of ASID reuse strategies
- Identify performance bottlenecks in memory management
- Test correctness of the new unified ASID manager implementation

## Components

### 1. ASID Profiling Infrastructure (`ostd/src/mm/asid_profiling.rs`)

The core profiling module that tracks:
- **Allocation Statistics**: Total allocations, failures, generation rollovers
- **TLB Operations**: Different types of TLB flushes and their timing
- **Context Switches**: Frequency and performance of VM space activations
- **Search Operations**: Bitmap and map search efficiency
- **Per-ASID Metrics**: Individual ASID usage patterns

### 2. New Test Applications for ASID Implementation Validation

#### `asid_correctness_test.c` - Comprehensive Correctness Testing
**NEW** - Tests the correctness of the new unified ASID manager implementation.

**Features:**
- Basic ASID allocation/deallocation functionality testing
- Concurrent access from multiple threads and processes
- Multi-process ASID operations validation
- Generation rollover behavior verification
- Edge cases and error condition handling
- Memory integrity checks under ASID stress
- Comprehensive test result reporting with pass/fail indicators

**Usage:**
```bash
cd /test && ./asid_correctness_test
```

**What it tests:**
- ✅ Basic ASID functionality works correctly
- ✅ Concurrent threads can safely access memory without corruption
- ✅ Multi-process workloads maintain memory integrity
- ✅ Generation rollover doesn't break system functionality
- ✅ Edge cases like rapid allocation/deallocation cycles

#### `asid_efficiency_monitor.c` - Performance Test with Detailed Monitoring
**NEW** - Measures efficiency improvements while recording detailed metrics.

**Features:**
- Comprehensive TLB flush counting and analysis
- Context switch efficiency measurement
- ASID allocation/deallocation pattern tracking
- Generation rollover impact analysis
- Real-time monitoring during test execution
- Detailed performance metrics and timeline data
- Multiple test configurations (light, medium, heavy load)

**Usage:**
```bash
# Run default medium load test
cd /test && ./asid_efficiency_monitor

# Run specific test configuration
cd /test && ./asid_efficiency_monitor 1  # Light load
cd /test && ./asid_efficiency_monitor 2  # Medium load  
cd /test && ./asid_efficiency_monitor 3  # Heavy load
```

**What it measures:**
- 📊 TLB flush operations (single address, single context, all context, full)
- 📊 Context switch rates and flush requirements
- 📊 ASID allocation success rates and patterns
- 📊 Generation rollover frequency and impact
- 📊 Memory bandwidth and operation throughput
- 📊 Timeline data for performance graphing

#### `asid_efficiency_clean.c` - Clean Performance Measurement
**NEW** - Pure performance testing without monitoring overhead.

**Features:**
- Minimal overhead for baseline performance measurement
- Multiple test configurations for comparison
- Memory access latency micro-benchmarks
- Pure throughput and bandwidth measurement
- Clean baseline for comparison with monitored tests

**Usage:**
```bash
# Run default performance test
cd /test && ./asid_efficiency_clean

# Run comparison suite with multiple configurations
cd /test && ./asid_efficiency_clean compare

# Run memory access latency tests
cd /test && ./asid_efficiency_clean latency
```

**What it measures:**
- ⚡ Raw memory operation throughput
- ⚡ Memory access latency patterns
- ⚡ Context switch performance impact
- ⚡ Multi-process scaling efficiency
- ⚡ Pure performance without monitoring cost

### 3. Existing Test Applications

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

### 4. Kernel Integration

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

## Testing the New ASID Implementation

### Recommended Testing Workflow

1. **Correctness First**: Verify the implementation works correctly
   ```bash
   cd /test && ./asid_correctness_test
   ```

2. **Performance with Monitoring**: Measure detailed performance metrics
   ```bash
   cd /test && ./asid_efficiency_monitor 2  # Medium load test
   ```

3. **Clean Performance**: Get baseline performance numbers
   ```bash
   cd /test && ./asid_efficiency_clean
   ```

4. **Compare Results**: Analyze the difference between monitored and clean tests to understand monitoring overhead

### Expected Results

**Correctness Test:**
- All tests should pass (✅ status indicators)
- No memory corruption detected
- System should handle concurrent access correctly
- Generation rollover should work seamlessly

**Efficiency Tests:**
- Lower TLB flush rates compared to old implementation
- Higher context switch efficiency (lower flush percentage)
- Improved memory access performance
- Better ASID reuse patterns

### Performance Comparison

To compare the new implementation with the old one:

1. **Before applying changes**: Run efficiency tests and save results
2. **After applying changes**: Run the same tests
3. **Compare metrics**:
   - TLB flush frequency (should decrease)
   - Context switch flush percentage (should decrease) 
   - Memory operation throughput (should increase)
   - ASID allocation efficiency (should improve)

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

# Run comprehensive correctness test
./asid_correctness_test

# Check for any failures or memory corruption
# If tests pass, ASID mechanism is working correctly
```

### 2. Performance Analysis
```bash
# Reset statistics before testing
./asid_profiler --reset

# Run performance benchmark with monitoring
./asid_efficiency_monitor 2

# Run clean performance test
./asid_efficiency_clean

# View detailed results
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
./asid_efficiency_clean
./asid_profiler --efficiency > baseline.txt

# After code changes, compare results
./asid_profiler --reset
./asid_efficiency_clean
./asid_profiler --efficiency > updated.txt

# Compare baseline.txt vs updated.txt
```

### 5. New Implementation Validation
```bash
# Test correctness of new unified ASID manager
./asid_correctness_test

# Measure efficiency improvements with detailed monitoring
./asid_efficiency_monitor 1  # Light load
./asid_efficiency_monitor 2  # Medium load  
./asid_efficiency_monitor 3  # Heavy load

# Get clean performance baseline
./asid_efficiency_clean compare

# Analyze the results for improvement verification
```

## Key Performance Indicators (KPIs)

When evaluating the new ASID implementation, focus on these metrics:

1. **TLB Efficiency**: Lower flush rates, especially "all context" flushes
2. **Context Switch Efficiency**: Lower percentage of switches requiring TLB flushes
3. **Memory Performance**: Higher throughput and lower latency
4. **ASID Utilization**: Better reuse patterns and fewer allocation failures
5. **Generation Rollover**: Less frequent rollovers indicating better ASID management

## Troubleshooting

**If correctness tests fail:**
- Check for memory corruption patterns
- Verify concurrent access is working properly
- Look for generation rollover issues

**If performance is worse than expected:**
- Compare monitored vs clean test results
- Check TLB flush patterns
- Analyze ASID allocation efficiency
- Look for excessive generation rollovers

**If tests crash or hang:**
- Check memory allocation patterns
- Verify thread synchronization
- Look for deadlocks in ASID management 
