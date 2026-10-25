import zlib, struct
def undcx(b):
    assert b[:4]==b'DCX\0', b[:4]
    i=b.find(b'\x78\xda')
    if i<0: i=b.find(b'\x78\x9c')
    return zlib.decompress(b[i:])
def bnd3(b):
    assert b[:4]==b'BND3', b[:4]
    fmt=b[0xC]; big=b[0xD]==1
    count=struct.unpack_from('<i',b,0x10)[0]
    # entry size depends on format flags
    # BND3 stores the format byte bit-reversed when bit 0 is clear and bit 7 set.
    if not (fmt & 1) and (fmt & 0x80) or fmt == 0x74:
        fmt = int('{:08b}'.format(fmt)[::-1], 2)
    ent = 0x0C + (4 if fmt & 0x02 else 0) + (4 if fmt & 0x0C else 0) + (4 if fmt & 0x20 else 0)
    if fmt & 0x10: ent += 4
    files=[]
    off=0x20
    for k in range(count):
        flags,=struct.unpack_from('<B',b,off)
        csize,=struct.unpack_from('<i',b,off+4)
        doff,=struct.unpack_from('<i',b,off+8)
        fid,=struct.unpack_from('<i',b,off+12)
        noff,=struct.unpack_from('<i',b,off+16)
        usize=struct.unpack_from('<i',b,off+20)[0] if ent>=0x18 else csize
        end=b.index(b'\0',noff); name=b[noff:end].decode('shift_jis','replace')
        files.append((fid,name,b[doff:doff+csize]))
        off+=ent
    return files

def flver0(b):
    assert b[:6]==b'FLVER\0'
    ver,doff,dlen,ndum,nmat,nbone,nmesh,nvb=struct.unpack_from('<8i',b,8)
    unicode_=b[0x49]
    hdr=dict(version=hex(ver),dummies=ndum,materials=nmat,bones=nbone,meshes=nmesh,unicode=unicode_)
    def rstr(off):
        if unicode_:
            end=off
            while b[end:end+2]!=b'\0\0': end+=2
            return b[off:end].decode('utf-16-le','replace')
        end=b.index(b'\0',off); return b[off:end].decode('shift_jis','replace')
    # find the bone block: after dummies (0x40 each) and materials of unknown size
    base=0x80+ndum*0x40
    for msize in (0x20,0x24,0x28,0x30,0x40):
        boff=base+nmat*msize
        try:
            names=[rstr(struct.unpack_from('<i',b,boff+i*0x80+12)[0]) for i in range(nbone)]
            if all(n and n.isprintable() for n in names): break
        except Exception: pass
    else:
        raise RuntimeError('bone block not found')
    bones=[]
    for i in range(nbone):
        o=boff+i*0x80
        t=struct.unpack_from('<3f',b,o); r=struct.unpack_from('<3f',b,o+16)
        parent,child=struct.unpack_from('<2h',b,o+28); s=struct.unpack_from('<3f',b,o+32)
        nxt,prv=struct.unpack_from('<2h',b,o+44)
        bones.append(dict(i=i,name=names[i],parent=parent,t=t,r=r,s=s))
    moff=boff+nbone*0x80
    meshes=[]
    for k in range(nmesh):
        o=moff+k*0x64
        dyn,mat=b[o],b[o+1]
        nidx,nvert=struct.unpack_from('<2i',b,o+4)
        dflt,=struct.unpack_from('<h',b,o+12)
        bi=list(struct.unpack_from('<28h',b,o+14))
        meshes.append(dict(k=k,dynamic=dyn,material=mat,verts=nvert,default=dflt,bones=bi))
    hdr['material_size']=hex(msize)
    return hdr,bones,meshes

def flver2(b):
    """DS1/DSR FLVER2 (version 0x2000C): bones and each mesh's bone list."""
    assert b[:6]==b'FLVER\0'
    ver,doff,dlen,ndum,nmat,nbone,nmesh,nvb=struct.unpack_from('<8i',b,8)
    unicode_=b[0x49]
    def rstr(off):
        if unicode_:
            e=off
            while b[e:e+2]!=b'\0\0': e+=2
            return b[off:e].decode('utf-16-le','replace')
        e=b.index(b'\0',off); return b[off:e].decode('shift_jis','replace')
    boff=0x80+ndum*0x40+nmat*0x20
    bones=[]
    for i in range(nbone):
        o=boff+i*0x80
        t=struct.unpack_from('<3f',b,o); name=rstr(struct.unpack_from('<i',b,o+12)[0])
        r=struct.unpack_from('<3f',b,o+16); parent,child=struct.unpack_from('<2h',b,o+28)
        s=struct.unpack_from('<3f',b,o+32)
        bones.append(dict(i=i,name=name,parent=parent,t=t,r=r,s=s))
    moff=boff+nbone*0x80
    meshes=[]
    for k in range(nmesh):
        o=moff+k*0x30
        dyn=b[o]; mat,=struct.unpack_from('<i',b,o+4); dflt,nb,bbo,bo,nfs,fso,nvbm,vbo=struct.unpack_from('<8i',b,o+16)
        bl=list(struct.unpack_from('<%di'%nb,b,bo)) if nb else []
        meshes.append(dict(k=k,dynamic=dyn,material=mat,default=dflt,bones=bl))
    return dict(version=hex(ver),bones=nbone,meshes=nmesh),bones,meshes
