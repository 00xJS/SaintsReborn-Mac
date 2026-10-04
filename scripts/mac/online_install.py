# Installs the online pack when its stamp matches the source that was built.
# Usage: online_install.py <zip> <dist folder> <wanted stamp>; exit 3 = other version.
import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); dist = sys.argv[2]; wanted = sys.argv[3]
have = z.read('stamp.txt').decode().strip()
if have != wanted:
    print('online pack is for another version of the source'); sys.exit(3)
for n in z.namelist():
    rel = n.replace('\\', '/')
    if rel == 'stamp.txt' or rel.endswith('/'): continue
    if rel.startswith('/') or '..' in rel.split('/'): sys.exit('bad path in the online pack: ' + rel)
    dest = os.path.join(dist, rel); os.makedirs(os.path.dirname(dest), exist_ok=True)
    open(dest, 'wb').write(z.read(n))
open(os.path.join(dist, 'online_pack.txt'), 'w').write(wanted)
