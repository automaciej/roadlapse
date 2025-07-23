#!/bin/bash
#
# Roadlapse Regression Test
# Tests the C roadlapse program to ensure it works correctly
#

set -e
set -u

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test configuration - FIXED: Correct 8x speedup expectations
TEST_DURATION=8       # Input video duration in seconds
# FIXED: 8x speedup means 8s input → 1s output, allow ±20% tolerance
EXPECTED_MIN_DURATION=0.8   # Minimum expected output duration (8x speedup = 1.0s, -20% tolerance)
EXPECTED_MAX_DURATION=1.2   # Maximum expected output duration (8x speedup = 1.0s, +20% tolerance)
EXPECTED_FPS_25=50    # Expected output framerate for 25fps input
EXPECTED_FPS_30=60    # Expected output framerate for 30fps input

function print_header() {
    echo -e "${GREEN}================================${NC}"
    echo -e "${GREEN} Roadlapse Regression Test${NC}"
    echo -e "${GREEN}================================${NC}"
    echo ""
}

function print_step() {
    echo -e "${YELLOW}=== $1 ===${NC}"
}

function print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

function print_error() {
    echo -e "${RED}❌ $1${NC}"
}

function cleanup() {
    if [[ -n "${TEST_DIR:-}" ]] && [[ -d "${TEST_DIR}" ]]; then
        echo ""
        print_step "Cleaning up test directory"
        rm -rf "${TEST_DIR}"
        print_success "Test directory cleaned up"
    fi
}

function check_dependencies() {
    print_step "Checking dependencies"

    if ! command -v ffmpeg >/dev/null 2>&1; then
        print_error "ffmpeg is required but not installed"
        return 1
    fi
    print_success "ffmpeg found"

    if ! command -v ffprobe >/dev/null 2>&1; then
        print_error "ffprobe is required but not installed"
        return 1
    fi
    print_success "ffprobe found"

    if ! command -v bc >/dev/null 2>&1; then
        print_error "bc is required but not installed"
        return 1
    fi
    print_success "bc found"

    echo ""
}

function create_test_video() {
    local output_file="$1"
    local duration="$2"
    local framerate="${3:-25}"

    print_step "Creating test video"
    echo "  Duration: ${duration} seconds"
    echo "  Framerate: ${framerate} fps"
    echo "  Output: ${output_file}"

    # Create a synthetic test video with moving content for stabilization to work on
    # Using a scrolling color pattern that changes over time
    if ! ffmpeg -hide_banner -loglevel warning \
        -f lavfi -i "color=c=black:s=640x480:d=${duration}:r=${framerate},geq=r='255*sin(2*PI*(X+T*50)/100)':g='255*sin(2*PI*(Y+T*30)/80)':b='255*cos(2*PI*(X+Y+T*40)/120)'" \
        -c:v libx264 -preset fast -crf 23 \
        "${output_file}"; then
        print_error "Failed to create test video"
        return 1
    fi

    print_success "Test video created: ${output_file}"

    # Verify the test video properties
    local actual_duration
    if ! actual_duration=$(ffprobe -hide_banner -loglevel quiet -select_streams v:0 -show_entries format=duration -of csv=p=0 "${output_file}" 2>/dev/null); then
        print_error "Could not probe test video duration"
        return 1
    fi

    local actual_fps
    if ! actual_fps=$(ffprobe -hide_banner -loglevel quiet -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "${output_file}" 2>/dev/null); then
        print_error "Could not probe test video framerate"
        return 1
    fi

    # Convert fractional framerate to decimal
    local fps_decimal
    if [[ "${actual_fps}" =~ ^([0-9]+)/([0-9]+)$ ]]; then
        fps_decimal=$(echo "scale=2; ${BASH_REMATCH[1]} / ${BASH_REMATCH[2]}" | bc)
    else
        fps_decimal="${actual_fps}"
    fi

    echo "  ✓ Actual duration: ${actual_duration}s"
    echo "  ✓ Actual framerate: ${fps_decimal}fps"
    echo ""
}

function run_roadlapse_single() {
    local input_file="$1"
    local output_file="$2"
    local roadlapse_program="$3"

    print_step "Running roadlapse program (single input)"
    echo "  Program: ${roadlapse_program}"
    echo "  Input: ${input_file}"
    echo "  Output: ${output_file}"
    echo ""

    # Run the roadlapse program and capture output
    local start_time=$(date +%s)

    # New syntax: no target_fps parameter
    if ! "${roadlapse_program}" "${output_file}" "${input_file}"; then
        print_error "Roadlapse program failed"
        return 1
    fi

    local end_time=$(date +%s)
    local processing_time=$((end_time - start_time))

    print_success "Roadlapse program completed in ${processing_time}s"
    echo ""
}

function run_roadlapse_multi() {
    local output_file="$1"
    local roadlapse_program="$2"
    shift 2
    local input_files=("$@")

    print_step "Running roadlapse program (multi-input)"
    echo "  Program: ${roadlapse_program}"
    echo "  Output: ${output_file}"
    echo "  Input files (${#input_files[@]}):"
    for i in "${!input_files[@]}"; do
        echo "    $((i+1)). ${input_files[i]}"
    done
    echo ""

    # Run the roadlapse program with multiple inputs
    local start_time=$(date +%s)

    if ! "${roadlapse_program}" "${output_file}" "${input_files[@]}"; then
        print_error "Roadlapse program failed"
        return 1
    fi

    local end_time=$(date +%s)
    local processing_time=$((end_time - start_time))

    print_success "Roadlapse program completed in ${processing_time}s"
    echo ""
}

function verify_output() {
    local output_file="$1"
    local expected_fps="$2"
    local expected_duration="${3:-}"

    print_step "Verifying output video"

    # Check if output file exists
    if [[ ! -f "${output_file}" ]]; then
        print_error "Output file does not exist: ${output_file}"
        return 1
    fi
    print_success "Output file exists"

    # Check if output file is readable
    if [[ ! -r "${output_file}" ]]; then
        print_error "Output file is not readable: ${output_file}"
        return 1
    fi
    print_success "Output file is readable"

    # Get output video duration
    local output_duration
    if ! output_duration=$(ffprobe -hide_banner -loglevel quiet -select_streams v:0 -show_entries format=duration -of csv=p=0 "${output_file}" 2>/dev/null); then
        print_error "Could not probe output video duration"
        return 1
    fi

    # Get output video framerate
    local output_fps_raw
    if ! output_fps_raw=$(ffprobe -hide_banner -loglevel quiet -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "${output_file}" 2>/dev/null); then
        print_error "Could not probe output video framerate"
        return 1
    fi

    # Convert fractional framerate to decimal
    local output_fps
    if [[ "${output_fps_raw}" =~ ^([0-9]+)/([0-9]+)$ ]]; then
        output_fps=$(echo "scale=0; ${BASH_REMATCH[1]} / ${BASH_REMATCH[2]} + 0.5" | bc | cut -d. -f1)
    else
        output_fps="${output_fps_raw}"
    fi

    # Get frame count for additional verification
    local frame_count
    if ! frame_count=$(ffprobe -hide_banner -loglevel quiet -select_streams v:0 -count_frames -show_entries stream=nb_read_frames -of csv=p=0 "${output_file}" 2>/dev/null); then
        print_error "Could not count frames in output video"
        return 1
    fi

    echo "  Duration: ${output_duration}s"
    echo "  Framerate: ${output_fps}fps"
    echo "  Frame count: ${frame_count} frames"
    echo ""

    # If expected duration is provided, verify it
    if [[ -n "${expected_duration}" ]]; then
        local min_duration=$(echo "scale=2; ${expected_duration} * 0.8" | bc)  # FIXED: ±20% tolerance
        local max_duration=$(echo "scale=2; ${expected_duration} * 1.2" | bc)

        if (( $(echo "${output_duration} < ${min_duration}" | bc -l) )); then
            print_error "Output duration ${output_duration}s is too short (expected ~${expected_duration}s)"
            return 1
        fi

        if (( $(echo "${output_duration} > ${max_duration}" | bc -l) )); then
            print_error "Output duration ${output_duration}s is too long (expected ~${expected_duration}s)"
            return 1
        fi

        print_success "Duration ${output_duration}s is within expected range"
    else
        # Use default min/max for single file tests
        if (( $(echo "${output_duration} < ${EXPECTED_MIN_DURATION}" | bc -l) )); then
            print_error "Output duration ${output_duration}s is too short (expected >= ${EXPECTED_MIN_DURATION}s)"
            return 1
        fi

        if (( $(echo "${output_duration} > ${EXPECTED_MAX_DURATION}" | bc -l) )); then
            print_error "Output duration ${output_duration}s is too long (expected <= ${EXPECTED_MAX_DURATION}s)"
            return 1
        fi

        print_success "Duration ${output_duration}s is within expected range (${EXPECTED_MIN_DURATION}s - ${EXPECTED_MAX_DURATION}s)"
    fi

    # Verify framerate
    if [[ "${output_fps}" != "${expected_fps}" ]]; then
        print_error "Output framerate ${output_fps}fps does not match expected ${expected_fps}fps"
        return 1
    fi

    print_success "Framerate ${output_fps}fps matches expected ${expected_fps}fps"

    # Calculate expected frame count and verify it's reasonable
    local expected_frames
    expected_frames=$(echo "scale=0; ${output_duration} * ${expected_fps} + 0.5" | bc | cut -d. -f1)

    # Allow some tolerance in frame count (±5%)
    local min_frames
    local max_frames
    min_frames=$(echo "scale=0; ${expected_frames} * 0.95" | bc | cut -d. -f1)
    max_frames=$(echo "scale=0; ${expected_frames} * 1.05" | bc | cut -d. -f1)

    if (( frame_count < min_frames || frame_count > max_frames )); then
        print_error "Frame count ${frame_count} is not within expected range (${min_frames} - ${max_frames})"
        return 1
    fi

    print_success "Frame count ${frame_count} is within expected range (${min_frames} - ${max_frames})"
    echo ""
}

function test_single_file_25fps() {
    local roadlapse_program="$1"
    local test_dir="$2"

    echo ""
    print_step "Test 1: Single file with 25fps input"

    # Define file paths
    local input_video="${test_dir}/test_25fps.mp4"
    local output_video="${test_dir}/test_25fps_output.mp4"

    # Create test video at 25fps
    if ! create_test_video "${input_video}" "${TEST_DURATION}" 25; then
        return 1
    fi

    # Run roadlapse program
    if ! run_roadlapse_single "${input_video}" "${output_video}" "${roadlapse_program}"; then
        return 1
    fi

    # Verify output (25fps -> 50fps)
    if ! verify_output "${output_video}" "${EXPECTED_FPS_25}"; then
        return 1
    fi

    # FIXED: Calculate speedup with correct 8x expectation
    local input_duration="${TEST_DURATION}"
    local output_duration
    output_duration=$(ffprobe -hide_banner -loglevel quiet -select_streams v:0 -show_entries format=duration -of csv=p=0 "${output_video}" 2>/dev/null)
    local actual_speedup
    actual_speedup=$(echo "scale=2; ${input_duration} / ${output_duration}" | bc)

    echo "  Input duration: ${input_duration}s"
    echo "  Output duration: ${output_duration}s"
    echo "  Expected speedup: 8.00x"  # FIXED: Updated from 4.00x
    echo "  Actual speedup: ${actual_speedup}x"

    # FIXED: Verify speedup is close to 8x (allow ±20% tolerance)
    if (( $(echo "${actual_speedup} < 6.4 || ${actual_speedup} > 9.6" | bc -l) )); then
        print_error "Actual speedup ${actual_speedup}x is not close to expected 8.00x"
        return 1
    fi

    print_success "Speedup factor ${actual_speedup}x is within acceptable range (6.4x - 9.6x)"
    print_success "Test 1 passed!"
    echo ""
}

function test_single_file_30fps() {
    local roadlapse_program="$1"
    local test_dir="$2"

    echo ""
    print_step "Test 2: Single file with 30fps input"

    # Define file paths
    local input_video="${test_dir}/test_30fps.mp4"
    local output_video="${test_dir}/test_30fps_output.mp4"

    # Create test video at 30fps
    if ! create_test_video "${input_video}" "${TEST_DURATION}" 30; then
        return 1
    fi

    # Run roadlapse program
    if ! run_roadlapse_single "${input_video}" "${output_video}" "${roadlapse_program}"; then
        return 1
    fi

    # Verify output (30fps -> 60fps)
    if ! verify_output "${output_video}" "${EXPECTED_FPS_30}"; then
        return 1
    fi

    print_success "Test 2 passed!"
    echo ""
}

function test_multi_file() {
    local roadlapse_program="$1"
    local test_dir="$2"

    echo ""
    print_step "Test 3: Multiple file concatenation"

    # Create 3 test videos, each 5 seconds
    local input_files=()
    local total_duration=0

    for i in 1 2 3; do
        local input_file="${test_dir}/multi_input_${i}.mp4"
        if ! create_test_video "${input_file}" 5 25; then
            return 1
        fi
        input_files+=("${input_file}")
        total_duration=$((total_duration + 5))
    done

    local output_video="${test_dir}/multi_output.mp4"

    # Run roadlapse with multiple inputs
    if ! run_roadlapse_multi "${output_video}" "${roadlapse_program}" "${input_files[@]}"; then
        return 1
    fi

    # FIXED: Expected duration: 15s total / 8x speedup = 1.875s
    local expected_multi_duration=$(echo "scale=3; ${total_duration} / 8.0" | bc)

    # Verify output
    if ! verify_output "${output_video}" "${EXPECTED_FPS_25}" "${expected_multi_duration}"; then
        return 1
    fi

    # Calculate speedup
    local output_duration
    output_duration=$(ffprobe -hide_banner -loglevel quiet -select_streams v:0 -show_entries format=duration -of csv=p=0 "${output_video}" 2>/dev/null)
    local actual_speedup
    actual_speedup=$(echo "scale=2; ${total_duration} / ${output_duration}" | bc)

    echo "  Total input duration: ${total_duration}s (3 files × 5s)"
    echo "  Output duration: ${output_duration}s"
    echo "  Expected speedup: 8.00x"  # FIXED: Updated from 4.00x
    echo "  Actual speedup: ${actual_speedup}x"

    print_success "Multi-file concatenation and processing successful!"
    print_success "Test 3 passed!"
    echo ""
}

function run_test() {
    local roadlapse_program="${1:-./roadlapse}"

    print_header

    # Check if roadlapse program exists
    if [[ ! -f "${roadlapse_program}" ]]; then
        print_error "Roadlapse program not found: ${roadlapse_program}"
        echo "Usage: $0 [path_to_roadlapse_program]"
        return 1
    fi

    if [[ ! -x "${roadlapse_program}" ]]; then
        print_error "Roadlapse program is not executable: ${roadlapse_program}"
        return 1
    fi

    print_success "Roadlapse program found: ${roadlapse_program}"
    echo ""

    # Check dependencies
    if ! check_dependencies; then
        return 1
    fi

    # Create temporary directory
    TEST_DIR=$(mktemp -d -t roadlapse_test_XXXXXX)
    trap cleanup EXIT

    print_step "Test setup"
    echo "  Test directory: ${TEST_DIR}"
    echo "  Single file duration: ${TEST_DURATION}s"
    echo "  Expected output duration: ${EXPECTED_MIN_DURATION}s - ${EXPECTED_MAX_DURATION}s (8x speedup)"  # FIXED: Updated comment
    echo "  Expected output framerates:"
    echo "    25fps input → ${EXPECTED_FPS_25}fps output"
    echo "    30fps input → ${EXPECTED_FPS_30}fps output"
    echo ""

    # Run tests
    local test_passed=0
    local test_failed=0

    if test_single_file_25fps "${roadlapse_program}" "${TEST_DIR}"; then
        ((test_passed++))
    else
        ((test_failed++))
    fi

    if test_single_file_30fps "${roadlapse_program}" "${TEST_DIR}"; then
        ((test_passed++))
    else
        ((test_failed++))
    fi

    if test_multi_file "${roadlapse_program}" "${TEST_DIR}"; then
        ((test_passed++))
    else
        ((test_failed++))
    fi

    # Test completed
    echo ""
    print_step "Test Results"
    echo -e "${GREEN}✓ Tests passed: ${test_passed}${NC}"
    if [[ ${test_failed} -gt 0 ]]; then
        echo -e "${RED}✗ Tests failed: ${test_failed}${NC}"
    fi
    echo ""

    if [[ ${test_failed} -eq 0 ]]; then
        print_success "All tests passed!"
        echo ""
        echo -e "${GREEN}✓ Single file (25fps): PASS${NC}"
        echo -e "${GREEN}✓ Single file (30fps): PASS${NC}"
        echo -e "${GREEN}✓ Multi-file concatenation: PASS${NC}"
        echo -e "${GREEN}✓ Duration verification: PASS${NC}"
        echo -e "${GREEN}✓ Framerate verification: PASS${NC}"
        echo -e "${GREEN}✓ Speedup factor verification: PASS (8x speedup)${NC}"  # FIXED: Updated comment
        echo ""
        echo -e "${GREEN}🎉 Regression test completed successfully!${NC}"
        return 0
    else
        print_error "Some tests failed!"
        return 1
    fi
}

# Main execution
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    # Script is being executed directly
    roadlapse_program="${1:-./roadlapse}"

    if ! run_test "${roadlapse_program}"; then
        print_error "Regression test failed!"
        exit 1
    fi
fi
