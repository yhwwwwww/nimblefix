#!/usr/bin/env python3
"""Create disposable certificates and run the Layer 0 DirectInitiator TLS/mTLS test."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nimblefix-direct-tls-") as temporary:
        root = Path(temporary)
        certificate, key = root / "cert.pem", root / "key.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-keyout", str(key), "-out", str(certificate), "-subj", "/CN=localhost",
            "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
        ], check=True, capture_output=True, timeout=20)
        subprocess.run([str(args.binary.resolve()), str(certificate), str(certificate), str(key)],
                       check=True, timeout=30)


if __name__ == "__main__":
    main()
