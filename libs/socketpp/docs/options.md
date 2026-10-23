# Typed socket options

Socket options live in `sockpp::opts`. An option stores its caller-facing
value and provides templated `apply()` and `read()` methods, so the same option
works with `CSocket` and `CSocketAsync` without exposing a native handle.

```cpp
socket.setOption(sockpp::opts::SNoDelay{true});
const bool enabled = sockpp::opts::SNoDelay::read(socket).how;
```

Both socket classes provide an inline `setOption(option)` template that
forwards the option to `option.apply(*this)`. It accepts mutable and const
lvalues as well as temporary options without copying them. Calling
`option.apply(socket)` directly remains supported. Unsupported-platform no-op
behavior and compiler warnings are preserved by the forwarding method.

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
