# Sphinx configuration for the wvt documentation (issue #23).
# Build: pip install -r docs/requirements.txt
#        sphinx-build -W -b html docs build/docs

project = "wvt"
author = "Brian Collette"
copyright = "2026, Brian Collette"

extensions = ["myst_parser"]
source_suffix = {".md": "markdown"}
exclude_patterns = ["_build", "requirements.txt"]

# MyST: ::: fences, definition lists, and anchors for headings down to ###,
# so pages can link to a section with page.md#anchor.
myst_enable_extensions = ["colon_fence", "deflist"]
myst_heading_anchors = 3

try:
    import furo  # noqa: F401

    html_theme = "furo"
except ImportError:
    html_theme = "alabaster"
html_title = "wvt"
html_static_path = []
