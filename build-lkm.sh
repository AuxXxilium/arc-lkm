#!/usr/bin/env bash

set -o pipefail

#
# Copyright (C) 2025 AuxXxilium <https://github.com/AuxXxilium>
# 
# This is free software, licensed under the MIT License.
# See /LICENSE for more information.
#

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() {
  echo -e "${GREEN}[INFO]${NC} $*"
}

log_warn() {
  echo -e "${YELLOW}[WARN]${NC} $*"
}

log_error() {
  echo -e "${RED}[ERROR]${NC} $*"
}

log_debug() {
  echo -e "${BLUE}[DEBUG]${NC} $*"
}

# Directories
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEMP_DIR="${SCRIPT_DIR}/.tmp_build"
RELEASES_DIR="${SCRIPT_DIR}/releases"
BUILD_TEMP="${TEMP_DIR}/build"
STAGING_DIR="${TEMP_DIR}/staging"

# Configuration
VERSION=""
BUILD_ALL=false
INTERACTIVE_MODE=false

# Platform kernel definitions
PLATFORMS=(
  "apollolake:4.4.302"
  "broadwell:4.4.302"
  "broadwellnk:4.4.302"
  "broadwellnkv2:4.4.302"
  "broadwellntbap:4.4.302"
  "denverton:4.4.302"
  "epyc7002:5.10.55"
  "geminilake:4.4.302"
  "geminilakenk:5.10.55"
  "purley:4.4.320"
  "r1000:4.4.302"
  "r1000nk:5.10.55"
  "v1000:4.4.302"
  "v1000nk:5.10.55"
)

# Help function
show_help() {
  cat << EOF
${BLUE}=== LKM Build Script ===${NC}

${GREEN}Usage:${NC} $0 [OPTIONS]

${GREEN}Options:${NC}
  -v, --version VERSION    DSM/Toolkit version (7.1, 7.2, 7.3)
  -a, --all                Build all versions (prod and dev)
  -h, --help               Show this help message

${GREEN}Examples:${NC}
  $0 -v 7.2                  Build 7.2 (prod and dev)
  $0 --version 7.3           Build 7.3 (prod and dev)
  $0 --all                   Build all versions
  $0                         Interactive mode

EOF
}

# Parse arguments
parse_args() {
  while [[ $# -gt 0 ]]; do
    case $1 in
      -v|--version)
        VERSION="$2"
        shift 2
        ;;
      -a|--all)
        BUILD_ALL=true
        shift
        ;;
      -h|--help)
        show_help
        exit 0
        ;;
      *)
        log_error "Unknown option: $1"
        show_help
        exit 1
        ;;
    esac
  done
}

# Interactive input
interactive_mode() {
  echo -e "${BLUE}=== LKM Build Configuration ===${NC}"
  echo ""
  
  if [ -z "$VERSION" ]; then
    echo "Available versions:"
    echo "  1) 7.2"
    echo "  2) 7.3"
    echo "  3) all"
    echo ""
    read -p "Select version (1-3, or enter version number): " VERSION_INPUT
    
    case "$VERSION_INPUT" in
      1) VERSION="7.2" ;;
      2) VERSION="7.3" ;;
      3) BUILD_ALL=true ;;
      7.[0-9]) VERSION="$VERSION_INPUT" ;;
      *) log_error "Invalid selection"; exit 1 ;;
    esac
    
    INTERACTIVE_MODE=true
  fi
  
}

# Validate inputs
validate_inputs() {
  if [[ ! "$VERSION" =~ ^7\.[0-9]$ ]]; then
    log_error "Invalid version: $VERSION"
    exit 1
  fi
}

# Build LKMs
build_lkms() {
  local version=$1
  
  log_info "Starting LKM Build"
  log_info "Version: $version"
  echo ""
  
  # Create directories
  mkdir -p "$RELEASES_DIR"
  mkdir -p "$BUILD_TEMP"
  
  # Track results
  local SUCCESSFUL=0
  local FAILED=0
  local FAILED_PLATFORMS=()
  
  # Get platforms for version
  local -a platforms=("${PLATFORMS[@]}")
  
  log_info "Building for ${#platforms[@]} platforms (dev + prod)"
  echo ""
  
  # Build for each platform and target (dev/prod)
  for ENTRY in "${platforms[@]}"; do
    local PLATFORM="${ENTRY%%:*}"
    local KERNEL_VER="${ENTRY##*:}"
    
    # Build both dev and prod
    for TARGET in dev prod; do
      log_info "Compiling: ${PLATFORM} (kernel ${KERNEL_VER}) - ${TARGET}"
      
      local PLATFORM_BUILD_DIR="${BUILD_TEMP}/${PLATFORM}-${version}-${TARGET}"
      mkdir -p "$PLATFORM_BUILD_DIR"
      chmod 777 "$PLATFORM_BUILD_DIR"
      
      # Run Docker compilation
      local docker_output
      local docker_exit_code=0
      docker_output=$(docker run --privileged --rm -t \
        -v "${SCRIPT_DIR}":/input \
        -v "${PLATFORM_BUILD_DIR}":/output \
        "auxxxilium/syno-compiler:${version}" \
        compile-lkm "${PLATFORM}" "${TARGET}" 2>&1) || docker_exit_code=$?
      
      if [ $docker_exit_code -eq 0 ]; then
        echo "$docker_output" | sed 's/^/  /'
        
        local FILE_KO="${PLATFORM_BUILD_DIR}/redpill.ko"
        local FOUND=0
        
        # Copy module to releases directory
        if [ -f "${FILE_KO}" ]; then
          mkdir -p "$RELEASES_DIR"
          local OUTPUT_FILE="${RELEASES_DIR}/rp-${PLATFORM}-${version}-${KERNEL_VER}-${TARGET}.ko.gz"
          
          # Fix permissions if needed (for Docker container files)
          chmod 644 "${FILE_KO}" 2>/dev/null || sudo chmod 644 "${FILE_KO}" 2>/dev/null || true
          
          # Compress and copy
          log_debug "Compressing ${FILE_KO} to ${OUTPUT_FILE}"
          if gzip -9 -c "${FILE_KO}" > "${OUTPUT_FILE}"; then
            local SIZE=$(du -h "${OUTPUT_FILE}" | awk '{print $1}')
            log_info "✓ Created: ${PLATFORM}-${version}-${KERNEL_VER}-${TARGET} (${SIZE})"
            ((SUCCESSFUL++))
            FOUND=1
          else
            log_error "✗ Failed to gzip redpill.ko for ${PLATFORM}-${TARGET}"
          fi
        else
          log_warn "⚠ ${PLATFORM}/redpill.ko not found at ${FILE_KO}"
        fi
        
        if [ $FOUND -eq 0 ]; then
          log_error "✗ No redpill module found for ${PLATFORM}-${TARGET}"
          ((FAILED++))
          FAILED_PLATFORMS+=("$PLATFORM-$TARGET")
        fi
      else
        echo "$docker_output" | sed 's/^/  /'
        log_error "✗ Docker compilation failed for ${TARGET}"
        ((FAILED++))
        FAILED_PLATFORMS+=("$PLATFORM-$TARGET")
      fi
      
      rm -rf "$PLATFORM_BUILD_DIR"
    done
  done
  
  echo ""
  local TOTAL_BUILDS=$((${#platforms[@]} * 2))  # 2 targets (dev + prod) per platform
  log_info "=== Build Summary ==="
  log_info "Successful: $SUCCESSFUL/$TOTAL_BUILDS"
  log_info "Failed: $FAILED/$TOTAL_BUILDS"
  
  if [ $FAILED -gt 0 ]; then
    log_warn "Failed builds: ${FAILED_PLATFORMS[*]}"
  fi
  
  echo ""
  log_info "Output stored in: $RELEASES_DIR"
  echo ""
}

# Cleanup
cleanup() {
  rm -rf "$TEMP_DIR"
}

# Create version file and zip releases
finalize_release() {
  log_info "Finalizing release..."
  
  # Create VERSION file with date
  echo "$(date '+%y.%m.%d')" > "${RELEASES_DIR}/VERSION"
  
  # Zip the releases directory
  local ZIP_NAME="rp-lkms.zip"
  local ZIP_PATH="${SCRIPT_DIR}/${ZIP_NAME}"
  
  log_info "Creating release archive: ${ZIP_NAME}"
  (cd "$RELEASES_DIR" && cd .. && zip -9 "$ZIP_PATH" releases/* >/dev/null 2>&1)
  
  if [ -f "$ZIP_PATH" ]; then
    local SIZE=$(du -h "$ZIP_PATH" | awk '{print $1}')
    log_info "✓ Successfully created: $ZIP_PATH"
    log_info "  Size: ${SIZE}"
  else
    log_error "Failed to create zip file"
    return 1
  fi
}

# Build all combinations
build_all() {
  log_info "Building all versions..."
  echo ""
  
  for ver in 7.2 7.3; do
    log_info "Starting: Version $ver"
    VERSION="$ver"
    validate_inputs
    build_lkms "$ver"
    log_info "Completed: Version $ver"
    echo ""
    sleep 2
  done
  
  log_info "All builds completed!"
}

# Main function
main() {
  echo ""
  log_info "=== LKM Docker Build System ==="
  echo ""
  
  # Parse arguments
  parse_args "$@"
  
  # Interactive mode if needed
  if [ "$BUILD_ALL" = false ] && [ -z "$VERSION" ]; then
    interactive_mode
  fi
  
  # Build all
  if [ "$BUILD_ALL" = true ]; then
    build_all
    if [ "$INTERACTIVE_MODE" = true ]; then
      finalize_release
    fi
  else
    validate_inputs
    build_lkms "$VERSION"
  fi
  
  # Cleanup
  cleanup
  
  echo ""
  log_info "=== Done ==="
  log_info "Output stored in: $RELEASES_DIR"
  echo ""
}

# Trap errors
trap cleanup EXIT

# Run main
main "$@"
