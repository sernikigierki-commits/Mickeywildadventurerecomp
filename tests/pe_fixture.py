"""Small PE headers for import-parser tests; never used as build outputs."""
import struct


def pe_image(imports=(), *, delay=(), machine=0x8664):
    data = bytearray(2048)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HH', data, 0x84, machine, 0)
    struct.pack_into('<H', data, 0x94, 240)
    optional = 0x98
    struct.pack_into('<H', data, optional, 0x20b)
    struct.pack_into('<I', data, optional + 60, len(data))
    struct.pack_into('<I', data, optional + 108, 16)
    cursor = 1024
    for directory, descriptor, stride, values in ((1, 512, 20, imports), (13, 768, 32, delay)):
        if not values:
            continue
        struct.pack_into('<II', data, optional + 112 + directory * 8, descriptor, (len(values) + 1) * stride)
        for i, name in enumerate(values):
            if directory == 13:
                struct.pack_into('<II', data, descriptor + i * stride, 1, cursor)
            else:
                struct.pack_into('<I', data, descriptor + i * stride + 12, cursor)
            encoded = name.encode() + b'\0'
            data[cursor:cursor + len(encoded)] = encoded
            cursor += len(encoded)
    return bytes(data)
