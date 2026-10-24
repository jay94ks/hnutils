"""Independent TLS/HTTP wire peer; Python/OpenSSL is used only as a test tool."""
import socket
import ssl
import subprocess
import sys


def exact(peer, n):
    data = bytearray()
    while len(data) < n:
        chunk = peer.recv(n - len(data))
        if not chunk:
            raise RuntimeError("truncated HTTP frame/message")
        data.extend(chunk)
    return bytes(data)


def frame(kind, flags, stream, payload=b""):
    return len(payload).to_bytes(3, "big") + bytes([kind, flags]) + stream.to_bytes(4, "big") + payload


def check(executable, version):
    process = subprocess.Popen([executable, str(version)], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
    try:
        port = int(process.stdout.readline().strip().split("=", 1)[1])
        der = bytes.fromhex(process.stdout.readline().strip().split("=", 1)[1])
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_3
        context.load_verify_locations(cadata=ssl.DER_cert_to_PEM_cert(der))
        alpn = "h2" if version == 2 else "http/1.1"
        context.set_alpn_protocols([alpn])
        payload = b"independent authenticated HTTP peer"
        with socket.create_connection(("127.0.0.1", port), timeout=10) as raw:
            with context.wrap_socket(raw, server_hostname="localhost") as peer:
                if peer.selected_alpn_protocol() != alpn:
                    raise RuntimeError("HTTP ALPN mismatch")
                if version == 1:
                    peer.sendall(b"POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                                 + str(len(payload)).encode() + b"\r\n\r\n" + payload)
                    head = bytearray()
                    while not head.endswith(b"\r\n\r\n"):
                        head.extend(exact(peer, 1))
                        if len(head) > 65536:
                            raise RuntimeError("oversized HTTP head")
                    if not head.startswith(b"HTTP/1.1 200 "):
                        raise RuntimeError(f"unexpected HTTP status: {head!r}")
                    fields = dict(line.split(b":", 1) for line in bytes(head).split(b"\r\n")[1:] if b":" in line)
                    length = int(next(value for key, value in fields.items() if key.lower() == b"content-length"))
                    received = exact(peer, length)
                else:
                    # Static HPACK POST, https, /; raw literal indexed-name :authority=localhost.
                    headers = b"\x83\x87\x84\x01\x09localhost"
                    peer.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" + frame(4, 0, 0)
                                 + frame(1, 4, 1, headers) + frame(0, 1, 1, payload))
                    received = bytearray()
                    status = False
                    for _ in range(100):
                        header = exact(peer, 9)
                        length = int.from_bytes(header[:3], "big")
                        kind, flags = header[3], header[4]
                        stream = int.from_bytes(header[5:], "big") & 0x7fffffff
                        if length > 1024 * 1024:
                            raise RuntimeError("oversized HTTP/2 frame")
                        body = exact(peer, length)
                        if kind == 4 and not flags & 1:
                            peer.sendall(frame(4, 1, 0))
                        if kind == 1 and stream == 1:
                            status = body.startswith(b"\x88")  # HPACK static :status=200.
                        if kind == 0 and stream == 1:
                            received.extend(body)
                        if stream == 1 and kind in (0, 1) and flags & 1:
                            break
                    else:
                        raise RuntimeError("response did not finish")
                    if not status:
                        raise RuntimeError("missing HTTP/2 status 200")
                if received != payload:
                    raise RuntimeError("HTTP body mismatch")
        stdout, stderr = process.communicate(timeout=10)
        if process.returncode:
            raise RuntimeError(f"server exit {process.returncode}: {stderr} {stdout}")
        print(f"PASS: TLS 1.3 verified localhost, ALPN {alpn}, HTTP {version}, exact independent POST/200 body")
    except Exception as failure:
        if process.poll() is None:
            process.kill()
        stdout, stderr = process.communicate()
        raise RuntimeError(f"HTTP {version} peer failed: {failure}; server: {stderr} {stdout}") from failure
    finally:
        if process.poll() is None:
            process.kill()
        process.communicate()


if __name__ == "__main__":
    for protocol in (1, 2):
        check(sys.argv[1], protocol)
