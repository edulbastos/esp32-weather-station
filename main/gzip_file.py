# Compresses a file with gzip for embedding in the firmware (used by
# main/CMakeLists.txt for web_page.html). mtime=0 keeps the output identical
# for identical input, so the build stays reproducible.
# Usage: python gzip_file.py <input> <output>
import gzip
import sys

with open(sys.argv[1], 'rb') as src:
    data = src.read()
with open(sys.argv[2], 'wb') as dst:
    dst.write(gzip.compress(data, compresslevel=9, mtime=0))
