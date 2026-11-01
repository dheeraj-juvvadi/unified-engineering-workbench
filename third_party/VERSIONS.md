# Third-party dependency lock

All dependencies are vendored (copied into this repository) so the build and the
packaged app need no network access. Versions are pinned; SHA-256 recorded below.

| Library | Version | Licence | File(s) | SHA-256 | Source |
|---|---|---|---|---|---|
| cpp-httplib | v0.18.3 | MIT | third_party/httplib.h | a0a0c13dc086663863dbe6730a19716f7d3744904b6205496e8301750814dbd5 | https://github.com/yhirose/cpp-httplib/tree/v0.18.3 |
| nlohmann/json | 3.11.3 | MIT | third_party/json.hpp | 9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6 | https://github.com/nlohmann/json/releases/tag/v3.11.3 |
| three.js | r160 (0.160.0) | MIT | src/frontend/vendor/three.module.js | 76dea8151bc9352aef3528b4262e249b2604f62543828328db978d060d61a495 | https://www.npmjs.com/package/three/v/0.160.0 |
| three.js OrbitControls | r160 | MIT | src/frontend/vendor/OrbitControls.js | 5a44a9e86a2a0fb11933eed69bc2cd33c76a496854c1aed6ed776efa87d7b064 | three/examples/jsm/controls |

Licence texts: `third_party/LICENSES/`.

Toolchain used for the release build (not bundled):
- Windows exe: MinGW-w64 GCC 13 (posix threads), `-O2 -static -mwindows`, cross-compiled on Ubuntu 24.04.
- Native test build: GCC 13.3 (Ubuntu 24.04), CMake ≥ 3.16.

Runtime dependencies of Launch.exe: only Windows system DLLs (checked with `objdump -p`).
