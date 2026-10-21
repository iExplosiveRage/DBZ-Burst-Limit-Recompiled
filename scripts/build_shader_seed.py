# Builds the shader / pipeline storage seed (step C, "pipeline storage warm-up"):
# the union of every known 424107DC.xsh (guest shaders) and 424107DC.rtv.d3d12.xpso
# (pipeline descriptions) under the given roots, deduplicated by hash. The SDK
# (gpu_shader_storage_seed) validates every record (XXH3) when it merges the seed
# into a user's cache, so a stale record is skipped, not trusted.
# usage: python build_seed.py <out_dir> <root> [<root> ...]
import os, sys, struct

out_dir = sys.argv[1]
roots = sys.argv[2:]
XSH, XPSO = '424107DC.xsh', '424107DC.rtv.d3d12.xpso'
xsh_header = None; xpso_header = None
shaders = {}; pipelines = {}
REC = 72
def read(path):
    try:
        return open(path, 'rb').read()
    except OSError:
        return None
for root in roots:
    for dirpath, dirnames, filenames in os.walk(root):
        if 'Documents' in dirpath:
            continue
        for name in filenames:
            path = os.path.join(dirpath, name)
            if name == XSH:
                data = read(path)
                if data is None:
                    continue
                if len(data) < 8 or data[:4] != b'XESH':
                    continue
                if xsh_header is None:
                    xsh_header = data[:8]
                elif data[:8] != xsh_header:
                    continue
                pos = 8
                while pos + 12 <= len(data):
                    h, dc = struct.unpack_from('<QI', data, pos)
                    n = (dc & 0x7FFFFFFF) * 4
                    if pos + 12 + n > len(data):
                        break
                    shaders.setdefault(h, data[pos:pos + 12 + n])
                    pos += 12 + n
            elif name == XPSO:
                data = read(path)
                if data is None:
                    continue
                if len(data) < 12 or data[:4] != b'XEPS':
                    continue
                if xpso_header is None:
                    xpso_header = data[:12]
                elif data[:12] != xpso_header:
                    continue
                for pos in range(12, len(data) - REC + 1, REC):
                    h = struct.unpack_from('<Q', data, pos)[0]
                    pipelines.setdefault(h, data[pos:pos + REC])
os.makedirs(out_dir, exist_ok=True)
with open(os.path.join(out_dir, XSH), 'wb') as f:
    f.write(xsh_header)
    for h in sorted(shaders):
        f.write(shaders[h])
with open(os.path.join(out_dir, XPSO), 'wb') as f:
    f.write(xpso_header)
    for h in sorted(pipelines):
        f.write(pipelines[h])
print(f'shaders {len(shaders)} pipelines {len(pipelines)}')
