# hnquic guidance

- Actual QUIC v1 implementation is authorized. Validate encrypted wire behavior and keep
  incomplete endpoint capabilities explicitly unavailable, never fake successful streams.
- Dummies have been removed. Keep public factories and isImplemented(); distinguish
  implementation availability from authenticated handshake confirmation.
- Split public contracts into common, options, stream, connection and server headers. hnquic.hpp
  only aggregates includes. Keep every public header independently compilable.
- No third-party QUIC/TLS engines. UDP uses socketpp, coroutines asyncpp, handshake policy
  hntls and cryptographic primitives libcertpp. Do not move TLS logic into hnquic or hnhttp.
- Keep 62-bit stream IDs, uni/bidirectional semantics, FIN, RESET_STREAM and STOP_SENDING
  distinct. Never emulate QUIC using one TCP byte stream or label test channels real QUIC.
- Use private state and owned coroutine buffers, caller-owned serial executors and bounded
  resources. Doctest failures must go to stdout/stderr with no Windows dialogs.
