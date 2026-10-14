#!/usr/bin/env bash
# Publish only after uploading assets. GitHub creates the tag at RELEASE_SHA
# when the draft becomes public, so an interrupted upload can be retried.
set -euo pipefail
: "${RELEASE_TAG:?release tag required}"
: "${RELEASE_SHA:?release commit required}"

if ! gh release view "$RELEASE_TAG" >/dev/null 2>&1; then
  gh release create "$RELEASE_TAG" --draft --target "$RELEASE_SHA" \
    --generate-notes --title "k-vm $RELEASE_TAG"
fi
# A retried draft may have been prepared by an older run of the same version.
gh release edit "$RELEASE_TAG" --target "$RELEASE_SHA"
gh release upload "$RELEASE_TAG" dist/* --clobber
gh release edit "$RELEASE_TAG" --draft=false
if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  echo "### Published $RELEASE_TAG" >> "$GITHUB_STEP_SUMMARY"
  gh release view "$RELEASE_TAG" --json url --jq '.url' >> "$GITHUB_STEP_SUMMARY"
fi
