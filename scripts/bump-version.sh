#!/bin/bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2025 Ericky
#
# Version bump script for gst-cuda-dmabuf
# Usage: ./scripts/bump-version.sh [major|minor|patch]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$PROJECT_ROOT"

# Files containing version
MESON_BUILD="meson.build"
SPEC_FILE="gst-cuda-dmabuf.spec"
DEBIAN_CHANGELOG="debian/changelog"

# Get current version from meson.build
CURRENT_VERSION=$(grep -oP "version:\s*'\K[0-9]+\.[0-9]+\.[0-9]+" "$MESON_BUILD")

if [ -z "$CURRENT_VERSION" ]; then
    echo "Error: Could not find version in $MESON_BUILD"
    exit 1
fi

echo "Current version: $CURRENT_VERSION"

# Parse version components
IFS='.' read -r MAJOR MINOR PATCH <<< "$CURRENT_VERSION"

# Determine bump type
BUMP_TYPE="${1:-patch}"

case "$BUMP_TYPE" in
    major)
        MAJOR=$((MAJOR + 1))
        MINOR=0
        PATCH=0
        ;;
    minor)
        MINOR=$((MINOR + 1))
        PATCH=0
        ;;
    patch)
        PATCH=$((PATCH + 1))
        ;;
    *)
        echo "Usage: $0 [major|minor|patch]"
        echo "  major: X.0.0 (breaking changes)"
        echo "  minor: 0.X.0 (new features)"
        echo "  patch: 0.0.X (bug fixes) [default]"
        exit 1
        ;;
esac

NEW_VERSION="$MAJOR.$MINOR.$PATCH"
echo "New version: $NEW_VERSION"

# Confirm
read -p "Bump version from $CURRENT_VERSION to $NEW_VERSION? [y/N] " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

# Update meson.build
sed -i "s/version: '$CURRENT_VERSION'/version: '$NEW_VERSION'/" "$MESON_BUILD"
echo "✓ Updated $MESON_BUILD"

# Update spec file version
sed -i "s/^Version:.*$/Version:        $NEW_VERSION/" "$SPEC_FILE"
echo "✓ Updated $SPEC_FILE"

# Add changelog entry to spec file
TODAY=$(date +"%a %b %d %Y")
CHANGELOG_ENTRY="* $TODAY Ericky <ericky_k_y@hotmail.com> - $NEW_VERSION-1\n- Version bump to $NEW_VERSION"

# Insert changelog entry after %changelog line
sed -i "/%changelog/a\\$CHANGELOG_ENTRY" "$SPEC_FILE"
echo "✓ Added changelog entry"

# Update debian/changelog
if [ -f "$DEBIAN_CHANGELOG" ]; then
    DEB_DATE=$(date -R)
    DEB_ENTRY="gst-cuda-dmabuf ($NEW_VERSION-1) unstable; urgency=medium\n\n  * Version bump to $NEW_VERSION\n\n -- Ericky <ericky_k_y@hotmail.com>  $DEB_DATE\n"
    # Prepend to debian/changelog
    echo -e "$DEB_ENTRY" | cat - "$DEBIAN_CHANGELOG" > /tmp/deb_changelog_tmp && mv /tmp/deb_changelog_tmp "$DEBIAN_CHANGELOG"
    echo "✓ Updated $DEBIAN_CHANGELOG"
fi

# Git operations
echo ""
echo "Staging changes..."
git add "$MESON_BUILD" "$SPEC_FILE" "$DEBIAN_CHANGELOG"

echo "Creating commit..."
git commit -m "chore: bump version to $NEW_VERSION"

echo "Creating tag v$NEW_VERSION..."
git tag -a "v$NEW_VERSION" -m "Release v$NEW_VERSION"

# Push
read -p "Push commit and tag to origin? [y/N] " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    git push origin HEAD
    git push origin "v$NEW_VERSION"
    echo ""
    echo "✓ Pushed commit and tag v$NEW_VERSION to origin"
else
    echo ""
    echo "Changes committed and tagged locally."
    echo "To push manually:"
    echo "  git push origin HEAD"
    echo "  git push origin v$NEW_VERSION"
fi

echo ""
echo "✓ Version bumped to $NEW_VERSION"
