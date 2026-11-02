# Vendored dependencies

## nlohmann/json

- Version: **3.12.0**
- License: MIT; see `nlohmann/LICENSE.MIT` and the header's embedded notices.
- Upstream: https://github.com/nlohmann/json
- Release and official checksum: https://github.com/nlohmann/json/releases/tag/v3.12.0
- Header source: https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp
- License source: https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT
- `nlohmann/json.hpp` SHA-256:
  `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`

The unmodified single header is included so configuring and building the parser
does not require network access or a system JSON package. Its include path is
private to the implementation; public Litewin types do not expose JSON types.

Verify from the repository root:

```sh
sha256sum third_party/nlohmann/json.hpp
```
