#pragma once
// The source tree wvt was built from (issue #61). cmake/build_info.cmake
// writes the definitions at every build.

// The short git commit hash, with "-dirty" if tracked files had changes;
// "unknown" outside a git checkout.
const char* buildGitHash();
// The version tag on that commit (e.g. "v1.1"); empty if it has none.
const char* buildVersionTag();
