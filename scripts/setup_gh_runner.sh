#!/bin/bash
#
# Setup a GitHub Actions self-hosted runner on a Jetson device.
#
# Usage (run ON the Jetson, or via SSH):
#   ./scripts/setup_gh_runner.sh <GITHUB_RUNNER_TOKEN>
#
# The token is a one-time registration token generated from:
#   GitHub repo -> Settings -> Actions -> Runners -> New self-hosted runner
#   Or via CLI:
#     gh api -X POST repos/OWNER/REPO/actions/runners/registration-token --jq .token
#
# Prerequisites:
#   - Jetson running Ubuntu (aarch64)
#   - Internet access
#   - sudo privileges
#   - python3 + pytest installed (for running tests)
#
set -euo pipefail

# ---- Configuration ----
RUNNER_VERSION="2.322.0"
RUNNER_ARCH="arm64"
REPO_URL="https://github.com/realsenseai/realsense_mipi_platform_driver"
RUNNER_DIR="${HOME}/actions-runner"
RUNNER_USER="$(whoami)"

# Labels identify this runner to workflow `runs-on:` selectors
RUNNER_NAME="${HOSTNAME:-jetson-runner}"
RUNNER_LABELS="jetson,xavier,self-hosted"

# ---- Parse args ----
if [ $# -lt 1 ]; then
    echo "Usage: $0 <GITHUB_RUNNER_TOKEN>"
    echo ""
    echo "Generate a token at:"
    echo "  ${REPO_URL}/settings/actions/runners/new"
    echo ""
    echo "Or via gh CLI:"
    echo "  gh api -X POST repos/realsenseai/realsense_mipi_platform_driver/actions/runners/registration-token --jq .token"
    exit 1
fi

TOKEN="$1"

echo "============================================================"
echo " GitHub Actions Self-Hosted Runner Setup"
echo "============================================================"
echo " Runner name:   ${RUNNER_NAME}"
echo " Labels:        ${RUNNER_LABELS}"
echo " Repo:          ${REPO_URL}"
echo " Install dir:   ${RUNNER_DIR}"
echo " Architecture:  ${RUNNER_ARCH}"
echo "============================================================"
echo ""

# ---- Install system dependencies ----
echo "[1/6] Installing system dependencies..."
sudo apt-get update -qq
sudo apt-get install -y -qq \
    curl jq python3 python3-pip v4l-utils \
    libicu-dev libkrb5-dev zlib1g-dev 2>/dev/null

# Ensure pytest is available
if ! python3 -m pytest --version >/dev/null 2>&1; then
    echo "Installing pytest..."
    pip3 install pytest --break-system-packages 2>/dev/null || pip3 install pytest
fi

# ---- Configure passwordless sudo for runner operations ----
echo ""
echo "[2/6] Configuring passwordless sudo for driver operations..."
SUDOERS_FILE="/etc/sudoers.d/github-runner"
if [ ! -f "$SUDOERS_FILE" ]; then
    sudo tee "$SUDOERS_FILE" > /dev/null <<SUDOERS
# Allow GitHub Actions runner to reload D4XX driver and read dmesg
${RUNNER_USER} ALL=(ALL) NOPASSWD: /sbin/rmmod d4xx
${RUNNER_USER} ALL=(ALL) NOPASSWD: /sbin/modprobe d4xx
${RUNNER_USER} ALL=(ALL) NOPASSWD: /usr/bin/dmesg
${RUNNER_USER} ALL=(ALL) NOPASSWD: /bin/dmesg
SUDOERS
    sudo chmod 0440 "$SUDOERS_FILE"
    echo "Created ${SUDOERS_FILE}"
else
    echo "Sudoers file already exists, skipping"
fi

# ---- Download runner ----
echo ""
echo "[3/6] Downloading GitHub Actions runner v${RUNNER_VERSION}..."
mkdir -p "${RUNNER_DIR}"
cd "${RUNNER_DIR}"

TARBALL="actions-runner-linux-${RUNNER_ARCH}-${RUNNER_VERSION}.tar.gz"
if [ ! -f "${TARBALL}" ]; then
    curl -sL -o "${TARBALL}" \
        "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/${TARBALL}"
    echo "Downloaded ${TARBALL}"
else
    echo "Runner tarball already exists, skipping download"
fi

echo "Extracting..."
tar xzf "${TARBALL}" --overwrite

# ---- Configure runner ----
echo ""
echo "[4/6] Configuring runner..."
./config.sh \
    --url "${REPO_URL}" \
    --token "${TOKEN}" \
    --name "${RUNNER_NAME}" \
    --labels "${RUNNER_LABELS}" \
    --work "_work" \
    --replace \
    --unattended

# ---- Install as systemd service ----
echo ""
echo "[5/6] Installing as systemd service..."
sudo ./svc.sh install "${RUNNER_USER}"
sudo ./svc.sh start

echo ""
echo "[6/6] Verifying runner status..."
sudo ./svc.sh status

echo ""
echo "============================================================"
echo " Setup complete!"
echo "============================================================"
echo ""
echo " Runner '${RUNNER_NAME}' is registered and running."
echo ""
echo " The GitHub Actions workflow will match this runner with:"
echo "   runs-on: [self-hosted, jetson, xavier]"
echo ""
echo " Management commands:"
echo "   cd ${RUNNER_DIR}"
echo "   sudo ./svc.sh status     # Check status"
echo "   sudo ./svc.sh stop       # Stop runner"
echo "   sudo ./svc.sh start      # Start runner"
echo "   sudo ./svc.sh uninstall  # Remove service"
echo "   ./config.sh remove       # Unregister from GitHub"
echo ""
echo " Trigger a test run from GitHub:"
echo "   - Push to master/dev (changes to kernel/realsense/ or test/v4l2_test/)"
echo "   - Manual: Actions tab -> 'V4L2 On-Device Tests' -> Run workflow"
echo "   - CLI:  gh workflow run v4l2-test.yml"
echo "============================================================"
