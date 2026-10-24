# hnhttp guidance

- Implementation has started: implement and independently test each capability, and keep
  README availability explicit. Unsupported protocol features must fail before wire effects;
  never add fake-success stubs or claim codec tests prove a complete HTTP/2/3 connection.
- Scope includes HTTP/1.0, HTTP/1.1, HTTP/2 and HTTP/3. Plain TCP uses socketpp. TLS and
  QUIC engines belong in separate hntls/hnquic libraries. Verify their negotiated state
  before adapting a connection. Do not introduce third-party TLS/QUIC engines. Payload compression
  uses hncomp and crypto/certificates libcertpp.
- Keep HTTP/1 connection reuse, HTTP/2 multiplexed byte-stream framing, and HTTP/3 QUIC stream
  mapping distinct. Do not emulate HTTP/3 with a single ordered transport stream.
- All protocol algorithms and state structures will be implemented locally. Payload codecs
  belong in hncomp; certificate objects come from libcertpp without implying validation.
- Public stateful concrete implementations must use private PIMPL. Pure interfaces expose no
  mutable state. Use C/I/S/E names, camelCase methods and libcertpp-style Javadoc comments.
- Own coroutine inputs/results. Never retain caller spans or expose mapped/native pointers.
- Stream bindings share lifetime with shared_ptr, but acquire one logical I/O lease. Reuse
  the same connection between requests; never reconstruct protocol state from shared raw streams.
- Transport and connection executor() identify a stable owning serial asyncpp dispatcher.
  Marshal native completions and worker results back to it; shared ownership is not thread safety.
  Keep per-turn work bounded and join close cleanup before stopping reactors. Optional
  deadline/stop-token support remains distinct from the tested close-driven wakeup/join path.
- Preserve read-ahead, bounded body streaming, duplicate fields, stream-scoped errors and
  backpressure. Do not auto-retry requests or silently drain unlimited bodies.
- Future file-backed bodies must use bounded configurable sliding mappings with runtime setters.
- Tests use doctest and console-only failures. Compile-contract tests do not prove protocol
  conformance. Use independent RFC vectors and interoperability checks when implementation starts.
