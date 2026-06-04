import struct, sys
p = sys.argv[1] if len(sys.argv) > 1 else "/home/mintxfce/.steam/debian-installation/appcache/appinfo.vdf"
with open(p, "rb") as f:
    d = f.read()
sto = struct.unpack("<q", d[8:16])[0]
nstr = struct.unpack("<I", d[sto:sto+4])[0]
ss = []
pp = sto + 4
for _ in range(nstr):
    e = d.find(b"\x00", pp)
    ss.append(d[pp:e].decode("utf-8", "replace"))
    pp = e + 1

def label(idx):
    if 0 <= idx < len(ss):
        return repr(ss[idx])
    return "BAD-{}".format(idx)

pos = 16
while pos < sto:
    appid = struct.unpack("<I", d[pos:pos+4])[0]
    if appid == 0:
        break
    sz = struct.unpack("<I", d[pos+4:pos+8])[0]
    if appid in (228980, 638510, 480, 17390):
        kv = d[pos+72:pos+8+sz]
        t = kv[0]
        ki = struct.unpack("<I", kv[1:5])[0]
        print("app={} type={} key_idx={} -> {}".format(appid, t, ki, label(ki)))
    pos += 8 + sz
