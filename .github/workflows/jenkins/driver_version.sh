#!/bin/bash

set -e  # Exit on error

# Check for branch argument
if [ -z "$1" ]; then
    echo "ERROR: Branch name required as first argument"
    echo "Usage: $0 <branch_name>"
    exit 1
fi

BRANCH="$1"
REPO_URL="https://github.com/realsenseai/realsense_mipi_platform_driver.git"
CLONE_DIR="realsense_mipi_platform_driver_temp"
VERSION_FILE="kernel/realsense/d4xx.c"

echo "=== Driver Version Tagger ==="
echo "Branch: $BRANCH"
echo ""

# Check for required credentials
if [ -z "$GIT_USER" ] || [ -z "$GIT_PASS" ]; then
    echo "ERROR: GIT_USER and GIT_PASS environment variables must be set"
    exit 1
fi

# Clean up any existing clone
if [ -d "$CLONE_DIR" ]; then
    echo "Removing existing clone directory..."
    rm -rf "$CLONE_DIR"
fi

# Clone the repository with credentials
echo "Cloning repository..."
git clone "https://${GIT_USER}:${GIT_PASS}@github.com/RealSenseAI/realsense_mipi_platform_driver.git" "$CLONE_DIR"
cd "$CLONE_DIR"

# Checkout the specified branch
echo "Checking out branch: $BRANCH"
git checkout "$BRANCH"

# Fetch all tags
echo "Fetching tags..."
git fetch --tags

# Get the last tag on the specified branch
LAST_TAG=$(git describe --tags --abbrev=0 "$BRANCH" 2>/dev/null || echo "")

if [ -z "$LAST_TAG" ]; then
    echo "No tags found on branch $BRANCH. Exiting..."
    cd ..
    rm -rf "$CLONE_DIR"
    exit 0
fi

echo "Last tag: $LAST_TAG"

# Check if there are commits after the last tag
TAG_COMMIT=$(git rev-list -n 1 "$LAST_TAG")
BRANCH_COMMIT=$(git rev-parse "$BRANCH")

echo "Tag commit: $TAG_COMMIT"
echo "Branch commit: $BRANCH_COMMIT"

if [ "$TAG_COMMIT" = "$BRANCH_COMMIT" ]; then
    echo "No new commits after tag $LAST_TAG on branch $BRANCH. Nothing to do."
    cd ..
    rm -rf "$CLONE_DIR"
    exit 0
fi

echo "Found new commits after tag $LAST_TAG"

# Check if version file exists
if [ ! -f "$VERSION_FILE" ]; then
    echo "ERROR: Version file not found: $VERSION_FILE"
    cd ..
    rm -rf "$CLONE_DIR"
    exit 1
fi

echo "Reading current version from $VERSION_FILE..."

FULL_VERSION=$(grep "MODULE_VERSION" $VERSION_FILE | sed 's/MODULE_VERSION("\(.*\)")[;]*$/\1/')
echo "Current version: $FULL_VERSION"

REV=$(echo "$FULL_VERSION" | awk -F. '{print $NF}')
echo "$REV"

#BASE_VERSION=$(echo "$FULL_VERSION" | awk -F. '{NF--; print $0}')
BASE_VERSION=$(echo "$FULL_VERSION" | cut -d'.' -f1-3)
echo "$BASE_VERSION"


NEW_REV=$((REV + 1))  
NEW_VERSION="${BASE_VERSION}_${NEW_REV}"
NEW_TAG_VERSION="${BASE_VERSION}.${NEW_REV}"

echo "New version: $NEW_TAG_VERSION"
TAG_NAME="v${NEW_TAG_VERSION}"
echo "New tag: $TAG_NAME"

# Update the version fields in the file
echo "Updating $VERSION_FILE..."
if [ "$NEW_REV" -ne "$REV" ]; then
    sed -i -E "/MODULE_VERSION/ s/([0-9]+\.[0-9]+\.[0-9]+\.)([0-9]+)/\1$NEW_REV/" "$VERSION_FILE"
else
    echo "Change not required"
fi

# Verify the change
echo "I am before NEW_REV_CHECK"
VERSION_LINE=$(grep MODULE_VERSION "$VERSION_FILE")
#NEW_REV_CHECK=$(echo "$VERSION_LINE" | sed -n 's/VERSION("\([^"]*\)")/\1/p')
#NEW_REV_CHECK=$(sed -i "s/$FULL_VERSION(\"[^\"]*\");/$FULL_VERSION(\"$NEW_VERSION\");/" "$VERSION_FILE"; grep -o '$FULL_VERSION("[^"]*");' "$VERSION_FILE")
#NEW_REV_CHECK=$(grep 'FULL_VERSION(' "$VERSION_FILE" | awk -v ver="$NEW_VERSION" '{gsub(/FULL_VERSION\("[^"]*"\)/,"FULL_VERSION(\"" ver "\")"); print}')
# Extract full version first
FULL_VERSION_FILE=$(echo "$VERSION_LINE" | awk -F'"' '{print $2}')

# Extract last number
NEW_REV_CHECK=$(echo "$FULL_VERSION_FILE" | awk -F'.' '{print $NF}')

echo "NEW_REV_CHECK: $NEW_REV_CHECK"


if [ "$NEW_REV_CHECK" != "$NEW_REV" ]; then
    echo "ERROR: Failed to update REV version in file (expected $NEW_REV, got $NEW_REV_CHECK)"
    cd ..
    rm -rf "$CLONE_DIR"
    exit 1
fi

echo "Successfully updated version to $BASE_VERSION.$NEW_REV"


# Configure git (use environment variables if available)
if [ -n "$GIT_USER_NAME" ]; then
    git config user.name "$GIT_USER_NAME"
else
    git config user.name "Jenkins Auto Version"
fi

if [ -n "$GIT_USER_EMAIL" ]; then
    git config user.email "$GIT_USER_EMAIL"
else
    git config user.email "jenkins@realsenseai.com"
fi

# Commit the change
echo "Committing changes..."
git add "$VERSION_FILE"
git commit -m "Auto-increment version to $NEW_TAG_VERSION"

# Create and push the tag
echo "Creating tag: $TAG_NAME"
git tag -a "$TAG_NAME" -m "Auto-tagged version $TAG_NAME"


# Push changes and tag
echo "Pushing changes and tag to remote..."
git push origin "$BRANCH"
git push origin "$TAG_NAME"

echo ""
echo "=== Success ==="
echo "Branch: $BRANCH"
echo "Version updated to: $NEW_TAG_VERSION"
echo "Tag created and pushed: $TAG_NAME"

# Cleanup
cd ..
rm -rf "$CLONE_DIR"

exit 0
