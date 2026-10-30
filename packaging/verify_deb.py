#!/usr/bin/env python3
"""Independently inspect a .deb via the host ar reader, then validate its tar payload."""
import hashlib
import io
from pathlib import Path
import struct
import subprocess
import sys
import tarfile

path=Path(sys.argv[1])
names=subprocess.check_output(['ar','-t',str(path)],text=True).splitlines()
assert names==['debian-binary','control.tar.gz','data.tar.gz'],names
read=lambda name:subprocess.check_output(['ar','-p',str(path),name])
assert read('debian-binary')==b'2.0\n'
with tarfile.open(fileobj=io.BytesIO(read('control.tar.gz')),mode='r:gz') as tar:
    control=tar.extractfile('./control').read().decode()
    sums=tar.extractfile('./md5sums').read().decode().splitlines()
assert 'Architecture: arm64\n' in control
assert 'Depends: libc6 (>= 2.38)' in control
assert 'Version: 0.2.0~beta1\n' in control
payload={}
with tarfile.open(fileobj=io.BytesIO(read('data.tar.gz')),mode='r:gz') as tar:
    for entry in tar:
        assert entry.uid==entry.gid==0
        assert not entry.name.startswith('/') and '..' not in Path(entry.name).parts
        assert entry.isdir() or entry.isfile()
        assert not entry.mode & 0o6022
        if entry.isfile():payload[entry.name.removeprefix('./')]=tar.extractfile(entry).read()
        if entry.name=='./usr/bin/m-pocket':assert entry.mode==0o755
for line in sums:
    digest,name=line.split('  ',1)
    assert hashlib.md5(payload[name]).hexdigest()==digest,name
assert len(sums)==len(payload)
elf=payload['usr/bin/m-pocket']
assert elf[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<H',elf,18)[0]==183
assert struct.unpack_from('<H',elf,16)[0]==3 # PIE
assert b'/opt/homebrew/' not in elf and b'/Users/' not in elf
assert 'Exec=/usr/bin/m-pocket\n' in payload['usr/share/APPLaunch/applications/m-pocket.desktop'].decode()
assert payload['usr/share/APPLaunch/share/images/m-pocket.png'][:8]==b'\x89PNG\r\n\x1a\n'
assert not any(part in name for name in payload for part in ['.env','preferences.json','registrations.json','__pycache__','.plist'])
print(control)
print(f'PASS: ar structure, ARM64 PIE, {len(payload)} root-owned files, permissions, checksums, launcher paths, no bundled user credentials or host paths.')
print('SHA256: '+hashlib.sha256(path.read_bytes()).hexdigest())
