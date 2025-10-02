#!/bin/bash

# Script to compile and run all tests under gtest directory
# Usage: ./run_all_tests.sh [options]
# Options:
#   -b, --build-only    Only build tests, don't run them
#   -r, --run-only      Only run tests, don't build them
#   -j, --jobs N        Number of parallel jobs for building (default: $(nproc))
#   -v, --verbose       Verbose output
#   -f, --filter REGEX  Run only tests matching regex pattern
#   -l, --list          List available tests
#   -h, --help          Show this help

set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
BUILD_TESTS=true
RUN_TESTS=true
VERBOSE=false
JOBS=$(nproc)
FILTER=""
LIST_ONLY=false
USE_CTEST=true
CUSTOM_BUILD_TARGET=""
EXPLICIT_TARGETS=()
FAILED_BUILD_TARGETS=()
EACH_MODE=false
PRESET="debug-asan"

# Project directories
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
TESTS_BUILD_DIR="${BUILD_DIR}/tests"

# Function to print colored output
print_status() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Function to show help
show_help() {
    cat << EOF
Script to compile and run all tests for http_client project

Usage: $0 [options]

Options:
  -b, --build-only    Only build tests, don't run them
  -r, --run-only      Only run tests, don't build them
  -j, --jobs N        Number of parallel jobs for building (default: $(nproc))
  -v, --verbose       Verbose output
  -f, --filter REGEX  Run only tests matching regex pattern
  -l, --list          List available tests
        -C, --ctest         Use CTest to run tests (default for this project)
        -M, --manual        Use manual test runner instead of CTest
        -t, --target NAME   Explicit build target (overrides auto-detect). Avoid 'test' for build-only.
            --targets "a b c"   Space-separated explicit test executable names to build (ignores auto-detect).
            --each            Build each discovered test target separately and report failures.
    -p, --preset NAME   CMake preset to configure/build (default: debug-asan)
  -h, --help          Show this help

Examples:
  $0                           # Build and run all tests
  $0 -b                        # Only build tests
  $0 -r                        # Only run tests (assuming they're built)
  $0 -j 8                      # Build with 8 parallel jobs
  $0 -f "cert_.*_test"         # Run only tests matching pattern
  $0 -v                        # Verbose output
  $0 -l                        # List available tests

EOF
}

# Function to parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -b|--build-only)
                BUILD_TESTS=true
                RUN_TESTS=false
                shift
                ;;
            -r|--run-only)
                BUILD_TESTS=false
                RUN_TESTS=true
                shift
                ;;
            -j|--jobs)
                JOBS="$2"
                shift 2
                ;;
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            -f|--filter)
                FILTER="$2"
                shift 2
                ;;
            -l|--list)
                LIST_ONLY=true
                shift
                ;;
            -C|--ctest)
                USE_CTEST=true
                shift
                ;;
            -M|--manual)
                USE_CTEST=false
                shift
                ;;
            -t|--target)
                CUSTOM_BUILD_TARGET="$2"
                shift 2
                ;;
            --targets)
                IFS=' ' read -r -a EXPLICIT_TARGETS <<< "$2"
                shift 2
                ;;
            --each)
                EACH_MODE=true
                shift
                ;;
            -p|--preset)
                PRESET="$2"
                shift 2
                ;;
            -h|--help)
                show_help
                exit 0
                ;;
            *)
                print_error "Unknown option: $1"
                show_help
                exit 1
                ;;
        esac
    done
}

# Function to get all test source files
get_test_sources() {
    find "${SCRIPT_DIR}/tests" -name "*test*.cpp" -type f | sort
}

# Function to get test names from source files
get_test_names() {
    local sources=($(get_test_sources))
    local test_names=()
    
    for source in "${sources[@]}"; do
        local basename=$(basename "$source" .cpp)
        test_names+=("$basename")
    done
    
    printf '%s\n' "${test_names[@]}"
}

# Function to get built test executables
get_built_tests() {
    if [[ -d "$TESTS_BUILD_DIR" ]]; then
        find "$TESTS_BUILD_DIR" -maxdepth 1 -name "*test*" -type f -executable | sort
    fi
}

# Function to list available tests
list_tests() {
    print_status "Available test source files:"
    local sources=($(get_test_sources))
    for source in "${sources[@]}"; do
        local basename=$(basename "$source" .cpp)
        echo "  - $basename ($(basename "$source"))"
    done
    
    echo
    print_status "Built test executables:"
    local built_tests=($(get_built_tests))
    if [[ ${#built_tests[@]} -eq 0 ]]; then
        print_warning "No built test executables found. Run with -b to build tests first."
    else
        for test in "${built_tests[@]}"; do
            echo "  - $(basename "$test")"
        done
    fi
}

# Function to check prerequisites
check_prerequisites() {
    # Check if we're in the right directory
    if [[ ! -f "CMakeLists.txt" ]]; then
        print_error "CMakeLists.txt not found. Please run this script from the project root directory."
        exit 1
    fi
    
    # Check if build directory exists
    if [[ ! -d "$BUILD_DIR" ]]; then
        if [[ "$BUILD_TESTS" == "true" ]]; then
            print_warning "Build directory not found at $BUILD_DIR" 
            print_status "It will be created during configure step using preset '$PRESET'."
        else
            print_error "Build directory not found at $BUILD_DIR"
            print_status "Please run this script with --build-only (default) or configure manually via 'cmake --preset $PRESET'."
            exit 1
        fi
    fi
    
    # Check for required tools
    if ! command -v cmake &> /dev/null; then
        print_error "cmake not found. Please install cmake."
        exit 1
    fi
    
    if ! command -v ninja &> /dev/null && ! command -v make &> /dev/null; then
        print_error "Neither ninja nor make found. Please install a build system."
        exit 1
    fi
}

# Function to build tests
build_tests() {
    print_status "Configuring project with preset '$PRESET'..."
    if ! cmake --preset "$PRESET"; then
        print_error "Configuration failed for preset '$PRESET'"
        exit 1
    fi

    print_status "Building tests with $JOBS parallel jobs..."
    cd "$BUILD_DIR"

    # If explicit test targets specified, build them individually and return.
    if [[ ${#EXPLICIT_TARGETS[@]} -gt 0 ]]; then
        print_status "Explicit targets specified: ${EXPLICIT_TARGETS[*]}"
        local fail=0
        for t in "${EXPLICIT_TARGETS[@]}"; do
            print_status "Building target: $t"
            if [[ "$VERBOSE" == "true" ]]; then
                if ! cmake --build . --target "$t" --parallel "$JOBS" --verbose; then
                    print_error "Build failed: $t"
                    FAILED_BUILD_TARGETS+=("$t")
                    fail=1
                else
                    print_success "Built: $t"
                fi
            else
                if ! cmake --build . --target "$t" --parallel "$JOBS"; then
                    print_error "Build failed: $t"
                    FAILED_BUILD_TARGETS+=("$t")
                    fail=1
                else
                    print_success "Built: $t"
                fi
            fi
        done
        cd "$SCRIPT_DIR"
        if [[ ${#FAILED_BUILD_TARGETS[@]} -gt 0 ]]; then
            print_error "Failed build targets: ${FAILED_BUILD_TARGETS[*]}"
            exit 1
        fi
        print_success "All selected targets built successfully"
        return
    fi

    local selected=""
    if [[ -n "$CUSTOM_BUILD_TARGET" ]]; then
        selected="$CUSTOM_BUILD_TARGET"
        print_status "Using explicit target override: $selected"
    else
        # Detect aggregated build target (avoid CMake 'test'/'tests' which execute tests)
        local candidate_targets=(all)
        if [[ -f build.ninja ]]; then
            for t in "${candidate_targets[@]}"; do
                if grep -q "^build $t:" build.ninja; then selected="$t"; break; fi
            done
        fi
        if [[ -z "$selected" ]]; then
            # Fallback parse help
            local help_out="$(cmake --build . --target help 2>/dev/null || true)"
            for t in "${candidate_targets[@]}"; do
                if echo "$help_out" | grep -qw "$t"; then selected="$t"; break; fi
            done
        fi
        if [[ -z "$selected" ]]; then
            selected="all"
            print_warning "No aggregate test build target found; using 'all'."
        fi
    fi
    if [[ "$selected" == "test" || "$selected" == "tests" ]]; then
        print_warning "Detected '$selected' target which runs tests; replacing with 'all' for build-only. Use -t test if you really want to execute via build step."
        selected="all"
    fi

    print_status "Using build target: $selected"

    local aggregated_failed=false
    if [[ "$VERBOSE" == "true" ]]; then
        if ! cmake --build . --target "$selected" --parallel "$JOBS" --verbose; then
            print_error "Aggregated build failed for target $selected"
            aggregated_failed=true
        fi
    else
        if ! cmake --build . --target "$selected" --parallel "$JOBS"; then
            print_error "Aggregated build failed for target $selected"
            aggregated_failed=true
        fi
    fi

        if [[ "$aggregated_failed" == "true" || "$EACH_MODE" == "true" ]]; then
        print_warning "Falling back to per-target build enumeration..."
        local srcs=($(get_test_sources))
        for s in "${srcs[@]}"; do
            local tname=$(basename "$s" .cpp)
            # Heuristic: ensure target appears in build graph
            if ! grep -q "build tests/.*${tname}" build.ninja 2>/dev/null && ! grep -q "build .*${tname}:" build.ninja 2>/dev/null; then
                [[ "$VERBOSE" == "true" ]] && print_warning "Target not found in build.ninja: $tname"
                continue
            fi
            print_status "[each] Building $tname"
            if ! cmake --build . --target "$tname" --parallel "$JOBS" ${VERBOSE:+--verbose}; then
                print_error "[each] Failed: $tname"
                FAILED_BUILD_TARGETS+=("$tname")
            else
                print_success "[each] Built: $tname"
            fi
        done
    fi

    if [[ ${#FAILED_BUILD_TARGETS[@]} -gt 0 ]]; then
        print_error "Build failures encountered (${#FAILED_BUILD_TARGETS[@]}):"
        for ft in "${FAILED_BUILD_TARGETS[@]}"; do
            echo -e "  - $ft"
        done
        cd "$SCRIPT_DIR"
        exit 1
    else
        print_success "All test targets built successfully"
    fi
    # Return to project root so subsequent test execution working directory is project root
    cd "$SCRIPT_DIR"
}

# Function to build specific test
build_specific_test() {
    local test_name="$1"
    print_status "Building test: $test_name"
    
    cd "$BUILD_DIR"
    
    if [[ "$VERBOSE" == "true" ]]; then
        cmake --build . --target "$test_name" --parallel "$JOBS" --verbose
    else
        cmake --build . --target "$test_name" --parallel "$JOBS"
    fi
}

# Function to run a single test
run_single_test() {
    local test_executable="$1"
    local test_name=$(basename "$test_executable")
    
    print_status "Running test: $test_name"
    
    if [[ "$VERBOSE" == "true" ]]; then
        echo "Command: $test_executable"
    fi
    
    # Run the test and capture output
    local start_time=$(date +%s)
    if "$test_executable"; then
        local end_time=$(date +%s)
        local duration=$((end_time - start_time))
        print_success "$test_name passed (${duration}s)"
        return 0
    else
        local end_time=$(date +%s)
        local duration=$((end_time - start_time))
        print_error "$test_name failed (${duration}s)"
        return 1
    fi
}

# Function to run all tests
run_tests() {
    local built_tests=($(get_built_tests))
    
    if [[ ${#built_tests[@]} -eq 0 ]]; then
        print_warning "No built test executables found."
        return 1
    fi
    
    local total_tests=0
    local passed_tests=0
    local failed_tests=()
    
    print_status "Running ${#built_tests[@]} tests..."
    echo
    
    for test_executable in "${built_tests[@]}"; do
        local test_name=$(basename "$test_executable")
        
        # Apply filter if specified
        if [[ -n "$FILTER" ]] && ! [[ "$test_name" =~ $FILTER ]]; then
            if [[ "$VERBOSE" == "true" ]]; then
                print_status "Skipping $test_name (doesn't match filter: $FILTER)"
            fi
            continue
        fi
        
        total_tests=$((total_tests + 1))
        
        if run_single_test "$test_executable"; then
            passed_tests=$((passed_tests + 1))
        else
            failed_tests+=("$test_name")
        fi
        echo
    done
    
    # Print summary
    echo "========================== SUMMARY =========================="
    print_status "Total tests run: $total_tests"
    print_success "Passed: $passed_tests"
    
    if [[ ${#failed_tests[@]} -gt 0 ]]; then
        print_error "Failed: ${#failed_tests[@]}"
        echo "Failed tests:"
        for failed_test in "${failed_tests[@]}"; do
            echo "  - $failed_test"
        done
        return 1
    else
        print_success "All tests passed!"
        return 0
    fi
}

# Function to run tests with CTest
run_tests_with_ctest() {
    print_status "Running tests via CTest..."
    cd "$BUILD_DIR"
    
    local filter_args=""
    if [[ -n "$FILTER" ]]; then
        filter_args="-R $FILTER"
    fi
    
    local verbose_args=""
    if [[ "$VERBOSE" == "true" ]]; then
        verbose_args="--verbose --output-on-failure"
    else
        verbose_args="--output-on-failure"
    fi
    
    if ctest $filter_args $verbose_args --parallel "$JOBS"; then
        print_success "All CTest tests passed"
        cd "$SCRIPT_DIR"
        return 0
    else
        print_error "Some CTest tests failed"
        cd "$SCRIPT_DIR"
        return 1
    fi
}

# Main function
main() {
    parse_args "$@"
    
    if [[ "$LIST_ONLY" == "true" ]]; then
        list_tests
        exit 0
    fi
    
    check_prerequisites
    
    local start_time=$(date +%s)
    
    if [[ "$BUILD_TESTS" == "true" ]]; then
        build_tests
    fi
    
    if [[ "$RUN_TESTS" == "true" ]]; then
        if [[ "$USE_CTEST" == "true" ]]; then
            run_tests_with_ctest
        else
            print_status "Running tests manually (working dir: project root)"
            run_tests
        fi
    fi
    
    local end_time=$(date +%s)
    local total_duration=$((end_time - start_time))
    
    echo
    if [[ ${#FAILED_BUILD_TARGETS[@]} -gt 0 ]]; then
        print_error "Summary: ${#FAILED_BUILD_TARGETS[@]} targets failed to build:"
        for ft in "${FAILED_BUILD_TARGETS[@]}"; do
            echo "  - $ft"
        done
    fi
    print_success "Total execution time: ${total_duration} seconds"
}

# Run main function with all arguments
main "$@"
