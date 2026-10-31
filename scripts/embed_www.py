# =============================================================
#  Pre-build script PlatformIO: tanam dashboard (web/) ke firmware.
#
#  Setiap file di web/ di-gzip lalu ditulis sebagai array byte ke
#  include/www_assets.h, dan disajikan web_server.cpp langsung dari
#  flash program. Dashboard ikut ter-update lewat OTA, tanpa uploadfs.
#  File hasil generate hanya ditulis ulang jika isinya berubah,
#  supaya tidak memicu kompilasi ulang yang tidak perlu.
# =============================================================
Import("env")

import gzip
import hashlib
import io
import os
import sys

PROJECT_DIR = env.subst("$PROJECT_DIR")
WEB_DIR = os.path.join(PROJECT_DIR, "web")
OUTPUT = os.path.join(PROJECT_DIR, "include", "www_assets.h")

MIME_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".json": "application/json",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
}


def gzip_bytes(raw):
    # mtime=0: isi yang sama selalu menghasilkan byte yang sama
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", compresslevel=9, mtime=0) as gz:
        gz.write(raw)
    return buf.getvalue()


def c_identifier(name):
    return "WWW_" + "".join(c if c.isalnum() else "_" for c in name)


def build_header():
    lines = [
        "// AUTO-GENERATED oleh scripts/embed_www.py dari web/ - jangan diedit",
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "struct WebAsset {",
        "  const char*    path;",
        "  const char*    mime;",
        "  const uint8_t* data;   // isi file yang sudah di-gzip",
        "  size_t         len;",
        "  const char*    etag;   // md5 isi file, berubah setiap file diubah",
        "};",
        "",
    ]
    entries = []
    raw_total = gz_total = 0

    for name in sorted(os.listdir(WEB_DIR)):
        path = os.path.join(WEB_DIR, name)
        ext = os.path.splitext(name)[1].lower()
        if name.startswith(".") or not os.path.isfile(path):
            continue
        if ext not in MIME_TYPES:
            print("embed_www: %s dilewati (tipe file tidak dikenal)" % name)
            continue

        with open(path, "rb") as f:
            raw = f.read()
        gz = gzip_bytes(raw)
        raw_total += len(raw)
        gz_total += len(gz)

        ident = c_identifier(name)
        etag = hashlib.md5(raw).hexdigest()[:16]
        lines.append("static const uint8_t %s[] = {" % ident)
        for i in range(0, len(gz), 16):
            lines.append("  " + ", ".join("0x%02x" % b for b in gz[i:i + 16]) + ",")
        lines.append("};")
        lines.append("")
        entries.append('  {"/%s", "%s", %s, sizeof(%s), "\\"%s\\""},' % (name, MIME_TYPES[ext], ident, ident, etag))

    lines.append("static const WebAsset WEB_ASSETS[] = {")
    lines.extend(entries)
    lines.append("};")
    return "\n".join(lines) + "\n", len(entries), raw_total, gz_total


if not os.path.isdir(WEB_DIR):
    sys.stderr.write("embed_www: folder %s tidak ditemukan\n" % WEB_DIR)
    env.Exit(1)

content, count, raw_total, gz_total = build_header()

old = None
if os.path.isfile(OUTPUT):
    with open(OUTPUT, "r", encoding="utf-8") as f:
        old = f.read()

if content != old:
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print("embed_www: %d file dashboard ditanam (%.1f KB -> %.1f KB gzip)"
          % (count, raw_total / 1024.0, gz_total / 1024.0))
