#!/usr/bin/env python3
import sys
import zipfile

apk_path, *libraries = sys.argv[1:]
with zipfile.ZipFile(apk_path, "a", compression=zipfile.ZIP_STORED) as apk:
    for library in libraries:
        apk.write(library, f"lib/arm64-v8a/{library.rsplit('/', 1)[-1]}")
