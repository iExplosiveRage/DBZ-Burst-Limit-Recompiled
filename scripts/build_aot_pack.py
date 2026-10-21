# Packs the XenosRecomp-BL output (C:\rex\_full_native\shaders: manifest.json + out\dxil) into one binary
# the SDK loads at startup (gpu_native_aot_shaders):
#   'XAOT' u32 version(1) u32 count, then per shader: u64 ucode hash, u32 stage (0 VS, 1 PS),
#   u32 flags (bit0 depth export, bit1 uses textures), u32 tex2d_mask, u32 tex3d_mask, u32 texcube_mask,
#   u32 dxil_size, dxil bytes (padded to 4).
# usage: python build_aot_pack.py <shaders_dir> <out_file> [<exclude.txt>]
import json, os, struct, sys
root, out = sys.argv[1], sys.argv[2]
m = json.load(open(os.path.join(root, 'manifest.json')))
exclude = set()
if len(sys.argv) > 3:
    exclude = {l.split('#')[0].strip().upper() for l in open(sys.argv[3]) if l.split('#')[0].strip()}
entries = []
for e in m['shaders']:
    if e.get('status') != 'ok' or e['xxh3'].upper() in exclude:
        continue
    meta = e['meta']
    dxil = open(os.path.join(root, e['dxil']), 'rb').read()
    stage = 0 if e['stage'] == 'vs' else 1
    flags = (1 if (e['stage'] == 'ps' and meta['export_mask'] & 0x10) else 0)
    tex = meta['tex2d_mask'] | meta['tex3d_mask'] | meta['texcube_mask'] | meta.get('tex1d_mask', 0)
    flags |= 2 if tex else 0
    entries.append(struct.pack('<QIIIIII', int(e['xxh3'], 16), stage, flags, meta['tex2d_mask'] | meta.get('tex1d_mask', 0),
                               meta['tex3d_mask'], meta['texcube_mask'], len(dxil)) + dxil + b'\0' * ((4 - len(dxil) % 4) % 4))
with open(out, 'wb') as f:
    f.write(b'XAOT' + struct.pack('<II', 1, len(entries)))
    for b in entries:
        f.write(b)
print(f'{len(entries)} shaders -> {out} ({os.path.getsize(out)} bytes)')
