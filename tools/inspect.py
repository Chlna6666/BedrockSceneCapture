#!/usr/bin/env python3
"""Inspect binary BSC1 chunk output, verifying counts and payload boundaries."""
import argparse
import io
import pathlib
import struct

class Reader:
    def __init__(self,data):self.s=io.BytesIO(data)
    def read(self,n):
        b=self.s.read(n)
        if len(b)!=n:raise ValueError("truncated BSC1 file")
        return b
    def val(self,fmt):
        return struct.unpack("<"+fmt,self.read(struct.calcsize("<"+fmt)))[0]
    def blob(self):
        n=self.val("I")
        if n>32*1024*1024:raise ValueError("oversized blob")
        return self.read(n)

def parse(data):
    r=Reader(data)
    if r.read(4)!=b"BSC1" or r.val("I")!=1:raise ValueError("bad BSC1 header")
    x,z,miny,maxy=[r.val("i") for _ in range(4)]
    palette,cells,meshes,missing=[r.val("I") for _ in range(4)]
    if not(0<maxy-miny<=2048) or cells!=(maxy-miny)*256:raise ValueError("bad cell range")
    if palette>65535 or meshes>cells:raise ValueError("bad section counts")
    for _ in range(palette):
        r.blob().decode("utf-8")
        r.blob() # binary little-endian compound-tag block state
        r.read(4+2+16)
    for _ in range(cells):
        if r.val("H")>=palette:raise ValueError("bad palette index")
    verts=0
    for _ in range(meshes):
        cell,layer,primitive,v,n=[r.val("I") for _ in range(5)]
        if cell>=cells or v>10_000_000 or n>30_000_000:raise ValueError("bad mesh")
        r.read(v*36+n*4)
        verts+=v
    if r.s.read(1):raise ValueError("trailing bytes")
    return dict(chunk=[x,z],height=[miny,maxy],palette=palette,cells=cells,
                meshes=meshes,vertices=verts,missing_meshes=missing)

def test():
    p=lambda fmt,*n:struct.pack("<"+fmt,*n)
    data=b"BSC1"+p("I",1)+p("iiii",0,-1,0,1)+p("IIII",1,256,1,0)
    data+=p("I",15)+b"minecraft:stone"+p("I",3)+b"nbt"
    data+=p("IH",12,0)+bytes(16)+bytes(512)
    data+=p("IIIII",0,0,0,1,0)+bytes(36)
    assert parse(data)["vertices"]==1
    try:parse(data[:-4])
    except ValueError:pass
    else:raise AssertionError("truncation accepted")
    print("BSC1 format self-test passed")

if __name__=="__main__":
    ap=argparse.ArgumentParser()
    ap.add_argument("paths",nargs="*",type=pathlib.Path)
    ap.add_argument("--self-test",action="store_true")
    a=ap.parse_args()
    if a.self_test:test()
    for path in a.paths:print(f"{path}: {parse(path.read_bytes())}")
