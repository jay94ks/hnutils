# Typed socket options

Socket options live in `sockpp::opts`. An option stores its caller-facing
value and provides templated `apply()` and `read()` methods, so the same option
works with `CSocket` and `CSocketAsync` without exposing a native handle.

```cpp
sockpp::opts::SNoDelay{true}.apply(socket);
const bool enabled = sockpp::opts::SNoDelay::read(socket).how;
```

## Support matrix

| Option | Windows | Linux | macOS/BSD |
| --- | --- | --- | --- |
| `SNoDelay` | yes | yes | yes |
| `SReuseAddress` | yes | yes | yes |
| `SKeepAlive` | yes | yes | yes |
| `SBroadcast` | yes | yes | yes |
| `SSendBuffer` | yes | yes | yes |
| `SReceiveBuffer` | yes | yes | yes |
| `SReusePort` | no-op warning | yes | yes |
| `SExclusiveAddressUse` | yes | no-op warning | no-op warning |

An unsupported option deliberately has an empty template branch. Instantiating
that branch calls a deprecated helper, producing a compiler warning that names
the unavailable option and platform. It does not reference an unavailable OS
constant and does not fail compilation.

The raw `setOption()` and `getOption()` methods remain as an extension point
for platform or application options that do not yet have a typed wrapper. New
reusable options should be added here as structures instead of duplicated as
ad-hoc calls.
