# Version stamp of the online pack: the same hash as scripts/online_stamp.ps1
# and installer/src/online.rs. Usage: online_stamp.py <source folder> <sdk commit>
import hashlib, os, sys
root, sdk = sys.argv[1], sys.argv[2]
files = [('\\patches\\rexglue-sdk.patch', os.path.join(root, 'patches', 'rexglue-sdk.patch'))]
for d, rel, exts in [(os.path.join(root, 'core', 'WhompaysCoop'), '\\core\\whompayscoop\\', ('cpp', 'h')),
                     (os.path.join(root, 'modding', 'include'), '\\modding\\include\\', ('h',))]:
    for n in os.listdir(d):
        p = os.path.join(d, n)
        if os.path.isfile(p) and n.rsplit('.', 1)[-1].lower() in exts:
            files.append((rel + n.lower(), p))
def weights(s):  # PowerShell's Sort-Object order (same as installer/src/online.rs)
    out = []
    for c in s:
        if c == '-': continue
        out.append(1 if c == '.' else 2 if c == '\\' else 3 if c == '_' else
                   100 + ord(c) if c.isdigit() else 1000 + ord(c) if 'a' <= c <= 'z' else 10 + ord(c) % 80)
    return (out, s)
files.sort(key=lambda f: weights(f[0]))
text = ('sdk %s\n' % sdk).encode()
for key, path in files:
    text += ('== %s ==\n' % key.lstrip('\\').replace('\\', '/')).encode()
    text += open(path, 'rb').read().replace(b'\r', b'')
print(hashlib.sha256(text).hexdigest())
