"""Pack ordinary Python data into bounded, read-only VDAT values for vcpe_data.
Keys are UTF-8 strings. bytes are opaque blobs. All offsets are little-endian.
Converted data is an artifact, never source code or executable bytecode."""
import struct

def pack(value):
    out=bytearray(16);strings={}
    def alloc(data,align=4):
        out.extend(b'\0'*(-len(out)%align));p=len(out);out.extend(data);return p
    def node(kind,count,data):return alloc(struct.pack('<III',kind,count,data&0xffffffff))
    def emit(v):
        if v is None:return node(0,0,0)
        if isinstance(v,(int,bool)):
            if not -(1<<31)<=int(v)<1<<31:raise ValueError('Integer exceeds VDAT int32')
            return node(1,0,int(v))
        if isinstance(v,float):return node(2,1,alloc(struct.pack('<d',v),8))
        if isinstance(v,str):
            if v in strings:return strings[v]
            raw=v.encode('utf8');n=node(3,len(raw),alloc(raw+b'\0',1));strings[v]=n;return n
        if isinstance(v,bytes):return node(6,len(v),alloc(v))
        if isinstance(v,(list,tuple)):
            values=[emit(x) for x in v];return node(4,len(values),alloc(struct.pack('<%dI'%len(values),*values)))
        if isinstance(v,dict):
            pairs=[(emit(str(k)),emit(v[k])) for k in sorted(v,key=lambda k:str(k).encode('utf8'))]
            return node(5,len(pairs),alloc(b''.join(struct.pack('<II',*p) for p in pairs)))
        raise TypeError(type(v))
    root=emit(value);struct.pack_into('<4sIII',out,0,b'VDAT',1,len(out),root);return bytes(out)
