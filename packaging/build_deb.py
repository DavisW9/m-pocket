#!/usr/bin/env python3
"""Build a standard Debian 2.0 archive without requiring root or dpkg on the host."""
import gzip
import hashlib
import io
import math
from pathlib import Path
import struct
import sys
import tarfile

VERSION='0.2.0~beta1'

def archive(files):
    output=io.BytesIO()
    with tarfile.open(fileobj=output,mode='w',format=tarfile.USTAR_FORMAT) as tar:
        directories=set()
        for name,data,mode in sorted(files):
            for parent in reversed(Path(name).parents):
                if str(parent)=='.' or str(parent) in directories:continue
                directories.add(str(parent));info=tarfile.TarInfo('./'+str(parent));info.type=tarfile.DIRTYPE;info.mode=0o755;info.uid=info.gid=0;info.uname=info.gname='root';info.mtime=0;tar.addfile(info)
            info=tarfile.TarInfo('./'+name);info.size=len(data);info.mode=mode;info.uid=info.gid=0;info.uname=info.gname='root';info.mtime=0
            tar.addfile(info,io.BytesIO(data))
    return gzip.compress(output.getvalue(),compresslevel=9,mtime=0)

def build(stage,destination):
    executable=stage/'usr/bin/m-pocket'
    data=executable.read_bytes()
    if data[:5]!=b'\x7fELF\x02' or data[5]!=1 or struct.unpack_from('<H',data,18)[0]!=183:
        raise SystemExit('Refusing package: executable must be Linux ELF64 AArch64, not a host binary.')
    files=[]
    for path in sorted(stage.rglob('*')):
        if path.is_symlink():raise SystemExit('Unexpected symlink in staging tree')
        if path.is_file():
            name=path.relative_to(stage).as_posix()
            if name.startswith('DEBIAN/') or not name.startswith('usr/'):raise SystemExit('Unexpected package path: '+name)
            files.append((name,path.read_bytes(),0o755 if name=='usr/bin/m-pocket' else 0o644))
    required={'usr/bin/m-pocket','usr/lib/m-pocket/service.py','usr/share/APPLaunch/applications/m-pocket.desktop','usr/share/APPLaunch/share/images/m-pocket.png','usr/share/doc/m-pocket/LICENSE'}
    if not required.issubset({name for name,_,_ in files}):raise SystemExit('Incomplete staging tree')
    size=sum(len(data) for _,data,_ in files)
    control=f'''Package: m-pocket
Version: {VERSION}
Section: net
Priority: optional
Architecture: arm64
Maintainer: m pocket local build <local@localhost>
Installed-Size: {math.ceil(size/1024)}
Depends: libc6 (>= 2.38), libstdc++6 (>= 11), libgcc-s1, python3 (>= 3.9), ca-certificates
Description: Mastodon client for Cardputer Zero (hardware-test beta)
 Purple light and dark themes, paged feeds, settings, and phone QR sign-in.
 Native 320x170 Linux framebuffer application. Physical hardware validation
 is still required before public release.
'''.encode()
    checksums=''.join(hashlib.md5(data).hexdigest()+'  '+name+'\n' for name,data,_ in files).encode()
    members=[('debian-binary',b'2.0\n'),('control.tar.gz',archive([('control',control,0o644),('md5sums',checksums,0o644)])),('data.tar.gz',archive(files))]
    with destination.open('wb') as output:
        output.write(b'!<arch>\n')
        for name,content in members:
            header=f'{name:<16}{0:<12}{0:<6}{0:<6}{"100644":<8}{len(content):<10}`\n'.encode('ascii')
            assert len(header)==60
            output.write(header);output.write(content)
            if len(content)%2:output.write(b'\n')
    print(f'Built {destination.name}: {destination.stat().st_size:,} bytes, {len(files)} files, ARM64')

if __name__=='__main__':
    if len(sys.argv)!=3:raise SystemExit('Usage: build_deb.py STAGE OUTPUT.deb')
    build(Path(sys.argv[1]),Path(sys.argv[2]))
