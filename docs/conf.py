# Sphinx configuration for the wvt documentation (issue #23).
# Build: pip install -r docs/requirements.txt
#        sphinx-build -W -b html docs build/docs

project = "wvt"
author = "Brian Collette"
copyright = "2026, Brian Collette"

extensions = ["myst_parser"]
source_suffix = {".md": "markdown"}
# design/ and docs-archive/ are not tracked (issue #109). Skip them in a local build.
exclude_patterns = ["_build", "requirements.txt", "design", "docs-archive"]

# MyST: ::: fences, definition lists, and anchors for all heading levels.
# MANUAL.md is one file with headings down to #####, and it links to its own
# sections with #anchor.
myst_enable_extensions = ["colon_fence", "deflist"]
myst_heading_anchors = 6

try:
    import furo  # noqa: F401

    html_theme = "furo"
except ImportError:
    html_theme = "alabaster"
html_title = "wvt"
html_static_path = []
