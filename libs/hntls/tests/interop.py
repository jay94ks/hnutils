"""Independent TLS peer test. Python/OpenSSL is a test tool, never a runtime dependency."""
import socket
import ssl
import subprocess
import sys


def main():
    server = subprocess.Popen([sys.argv[1], "0"], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)
    try:
        port_line = server.stdout.readline().strip()
        certificate_line = server.stdout.readline().strip()
        if not port_line.startswith("port=") or not certificate_line.startswith("certificate="):
            raise RuntimeError("server did not publish its test endpoint and public certificate")
        port = int(port_line.partition("=")[2])
        der = bytes.fromhex(certificate_line.partition("=")[2])
        policy = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        policy.minimum_version = ssl.TLSVersion.TLSv1_3
        policy.maximum_version = ssl.TLSVersion.TLSv1_3
        policy.load_verify_locations(cadata=ssl.DER_cert_to_PEM_cert(der))
        policy.set_alpn_protocols(["echo/1"])
        with socket.create_connection(("127.0.0.1", port), timeout=10) as raw:
            with policy.wrap_socket(raw, server_hostname="localhost") as peer:
                if peer.version() != "TLSv1.3" or peer.selected_alpn_protocol() != "echo/1":
                    raise RuntimeError("TLS version or ALPN mismatch")
                peer.sendall(b"independent TLS peer")
                received = bytearray()
                while len(received) < len(b"independent TLS peer"):
                    chunk = peer.recv(16384)
                    if not chunk:
                        raise RuntimeError("premature encrypted EOF")
                    received.extend(chunk)
                if received != b"independent TLS peer":
                    raise RuntimeError("echo mismatch")
                peer.unwrap().close()
        stdout, stderr = server.communicate(timeout=10)
        if server.returncode:
            raise RuntimeError(f"server exit {server.returncode}: {stderr} {stdout}")
        print(f"PASS: independent {ssl.OPENSSL_VERSION}, TLSv1.3, verified localhost, ALPN echo/1, exact echo and close_notify")
    finally:
        if server.poll() is None:
            server.kill()
        server.communicate()


if __name__ == "__main__":
    main()
