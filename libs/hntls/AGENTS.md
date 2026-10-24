# hntls guidance

- Actual TLS implementation is authorized. Implement TLS 1.3 using libcertpp primitives;
  never use a system TLS engine or report authentication without verification.
- Keep factories/public contracts independent from implementation files. Dummies have been
  removed; expose isImplemented() without confusing it with an established trusted session.
- Split public contracts by responsibility into common, options, credentials, context, session and stream
  headers. hntls.hpp only aggregates includes. Every public header must compile independently.
- TLS cryptography, key objects and certificate objects use libcertpp. Preserve any necessary
  upstream additions as refs/libcertpp patches. Never duplicate cryptographic algorithms here.
- Keep the TLS record path distinct from the QUIC record-free CRYPTO/traffic-secret path.
- Use owned coroutine inputs/results, serial caller-owned executors, private state, C/I/S/E
  naming and libcertpp-style Javadoc. No hidden threads, networking or trust success in dummies.
- Tests use doctest and console-only diagnostics, including Windows faults.
