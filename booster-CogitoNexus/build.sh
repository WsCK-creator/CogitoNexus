#!/bin/bash
# build.sh - Build script for CogitoNexus Server on Booster K1
#
# Usage:
#   ./build.sh
#
# This script will:
# 1. Check that the MMS-TTS Python environment (tts_venv) and the local
#    model directory (mms-model/pl_PL) are already set up -- it does NOT
#    create them itself (unlike the old piper-tts auto-build step, which
#    this replaced), since that setup needs internet access and is a
#    one-time thing; see README.md's "Setting up the MMS-TTS Python
#    environment" / "Downloading the model locally" sections.
# 2. Build the cogito_server binary

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR"

# Configuration
SDK_DIR="/home/booster/Workspace/sdk_release-main"
TTS_VENV_DIR="$PROJECT_DIR/tts_venv"
MMS_MODEL_DIR="$PROJECT_DIR/mms-model/pl_PL"
BUILD_DIR="$PROJECT_DIR/build"

# Biblioteki natywne CUDA (m.in. libcudss.so.0) używane przez torch w
# tts_venv leżą tutaj -- bez tego na ścieżce loadera "import torch" (a
# więc i test w Kroku 1 poniżej) kończy się błędem
# "libcudss.so.0: cannot open shared object file". Usługa systemd ma to
# ustawione w swoim pliku .service, ale sam build.sh (uruchamiany ręcznie
# w terminalu) tego nie dziedziczy, więc ustawiamy to też tutaj.
NVIDIA_CU12_LIB="$TTS_VENV_DIR/lib/python3.10/site-packages/nvidia/cu12/lib"
export LD_LIBRARY_PATH="$NVIDIA_CU12_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${GREEN}=== CogitoNexus Build Script ===${NC}"
echo "Project: $PROJECT_DIR"
echo "SDK: $SDK_DIR"

# Check if cmake is available (use pip version if needed)
CMAKE_BIN="cmake"
if command -v cmake &> /dev/null; then
    CMAKE_VERSION=$($CMAKE_BIN --version | head -1)
    echo "Found cmake: $CMAKE_VERSION"
fi

# Use pip cmake if system cmake is too old. pip's install location varies
# depending on how/as-whom it was installed (--user puts it under
# ~/.local, but a prior `sudo pip install` or a package manager that
# invokes pip as root lands it under /usr/local instead) -- so try the
# common candidate paths, then fall back to asking pip itself where its
# cmake package lives (`pip show -f cmake`), rather than hardcoding one
# path and failing when it doesn't match this machine's setup.
if ! $CMAKE_BIN --version 2>/dev/null | grep -q "3.2[6-9]\|3.[3-9]\|4\."; then
    PIP_CMAKE=""
    for CANDIDATE in \
        "$HOME/.local/lib/python3.10/site-packages/cmake/data/bin/cmake" \
        "/usr/local/lib/python3.10/dist-packages/cmake/data/bin/cmake" \
        "/usr/lib/python3.10/dist-packages/cmake/data/bin/cmake"
    do
        if [ -x "$CANDIDATE" ]; then
            PIP_CMAKE="$CANDIDATE"
            break
        fi
    done

    if [ -z "$PIP_CMAKE" ]; then
        # Last resort: ask pip directly where its cmake package's files went.
        PIP_SHOW_LOCATION=$(python3 -m pip show -f cmake 2>/dev/null | awk -F': ' '/^Location:/{print $2}')
        if [ -n "$PIP_SHOW_LOCATION" ]; then
            DISCOVERED="$PIP_SHOW_LOCATION/cmake/data/bin/cmake"
            [ -x "$DISCOVERED" ] && PIP_CMAKE="$DISCOVERED"
        fi
    fi

    if [ -n "$PIP_CMAKE" ]; then
        CMAKE_BIN="$PIP_CMAKE"
        echo "Using pip cmake: $($CMAKE_BIN --version | head -1) ($CMAKE_BIN)"
    else
        echo -e "${RED}Error: cmake >= 3.26 required${NC}"
        echo "Install with: pip install --break-system-packages --user cmake"
        echo "If it's already installed but not found here, locate it with:"
        echo "  python3 -m pip show -f cmake"
        exit 1
    fi
fi

# ============================================================
# Step 1: Check the MMS-TTS Python environment is set up
# ============================================================

echo ""
echo -e "${YELLOW}Step 1: Checking MMS-TTS environment...${NC}"

if [ -x "$TTS_VENV_DIR/bin/python3" ] && "$TTS_VENV_DIR/bin/python3" -c "import transformers, torch" 2>/dev/null; then
    echo -e "${GREEN}tts_venv OK (transformers/torch importable)${NC}"
else
    echo -e "${RED}tts_venv missing or incomplete at $TTS_VENV_DIR${NC}"
    echo "This is a one-time manual setup (needs internet access), not done by this script:"
    echo "  cd $PROJECT_DIR"
    echo "  python3 -m venv tts_venv"
    echo "  source tts_venv/bin/activate"
    echo "  pip install --upgrade pip"
    echo "  pip install transformers torch scipy numpy"
    echo "See README.md's \"Setting up the MMS-TTS Python environment\" section for details."
    exit 1
fi

if [ -f "$MMS_MODEL_DIR/config.json" ]; then
    echo -e "${GREEN}MMS-TTS model found at $MMS_MODEL_DIR${NC}"
else
    echo -e "${RED}MMS-TTS model not found at $MMS_MODEL_DIR${NC}"
    echo "This is a one-time manual download (needs internet access), not done by this script:"
    echo "  $TTS_VENV_DIR/bin/python3 -c \""
    echo "from huggingface_hub import snapshot_download"
    echo "snapshot_download('facebook/mms-tts-pol', local_dir='$MMS_MODEL_DIR')"
    echo "\""
    echo "See README.md's \"Downloading the model locally\" section for details."
    exit 1
fi

# ============================================================
# Step 2: Build cogito_server
# ============================================================

echo ""
echo -e "${YELLOW}Step 2: Building cogito_server...${NC}"

cd "$PROJECT_DIR"

# Clean build directory
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "Configuring cogito_server..."
$CMAKE_BIN .. \
    -DSDK_DIR="$SDK_DIR" \
    -DCMAKE_BUILD_TYPE=Release

echo "Building..."
$CMAKE_BIN --build . --config Release

# ============================================================
# Done
# ============================================================

echo ""
echo -e "${GREEN}=== Build Complete ===${NC}"
echo "Binary: $BUILD_DIR/cogito_server"
echo ""
echo "Run manually:"
echo "  ./build/cogito_server 9000 \\"
echo "    $MMS_MODEL_DIR \\"
echo "    \"\" \\"
echo "    $PROJECT_DIR/mms_worker.py \\"
echo "    1.0 \\"
echo "    1.0"
echo ""
echo "Or install as a systemd service -- see README.md section 4."
echo ""
echo "Run Python test client:"
echo "  python3 test_client.py 127.0.0.1 9000"
