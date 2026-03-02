#!/usr/bin/env bash
set -euo pipefail

file="src/shadow/shadow_ui.js"
helper="src/shared/filepath_browser.mjs"

if [ ! -f "$helper" ]; then
  echo "FAIL: missing shared helper $helper" >&2
  exit 1
fi

if ! rg -n "filepath_browser\.mjs" "$file" >/dev/null; then
  echo "FAIL: shadow UI is not importing filepath_browser.mjs" >&2
  exit 1
fi

if ! rg -n "FILEPATH_BROWSER" "$file" >/dev/null; then
  echo "FAIL: FILEPATH_BROWSER view is missing" >&2
  exit 1
fi

if ! perl -0ne 'exit((/meta\s*&&\s*meta\.type\s*===\s*"filepath"\s*\)\s*\{[^}]*openHierarchyFilepathBrowser/s) ? 0 : 1)' "$file"; then
  echo "FAIL: hierarchy select path does not open filepath browser for filepath params" >&2
  exit 1
fi

if ! perl -0ne 'exit((/case\s+VIEWS\.FILEPATH_BROWSER\s*:\s*\n\s*drawFilepathBrowser\(\)/s) ? 0 : 1)' "$file"; then
  echo "FAIL: render switch does not draw filepath browser view" >&2
  exit 1
fi

if ! rg -n 'label:\s*`\[\$\{name\}\]`' "$helper" >/dev/null; then
  echo "FAIL: folder labels are not rendered as [folderName]" >&2
  exit 1
fi

if ! rg -n "selectedPath" "$helper" >/dev/null; then
  echo "FAIL: helper missing selectedPath support for restoring parent+highlight" >&2
  exit 1
fi

echo "PASS: shadow filepath browser integration present"
