from minidump.minidumpfile import MinidumpFile
import struct

path = r"c:\Users\James\Downloads\crash_2026_07_31_20_15_48.4793184.dmp"
mf = MinidumpFile.parse(path)
rec = mf.exception.exception_records[0]
ex = rec.ExceptionRecord
print("ThreadId", rec.ThreadId)
print("ExceptionCode raw", hex(ex.ExceptionCode) if isinstance(ex.ExceptionCode, int) else ex.ExceptionCode)
print("ExceptionCode", ex.ExceptionCode)
print("ExceptionFlags", hex(ex.ExceptionFlags))
print("ExceptionAddress", hex(ex.ExceptionAddress))
print("NumberParameters", ex.NumberParameters)
print("ExceptionInformation", [hex(x) for x in ex.ExceptionInformation[:ex.NumberParameters]])

mods = []
for m in mf.modules.modules:
    name = m.name or "?"
    bn = name.split("\\")[-1]
    size = getattr(m, "size", None) or 0
    mods.append((m.baseaddress, size, bn, name))
mods.sort()

def resolve(addr):
    for base, size, bn, full in mods:
        if size and base <= addr < base + size:
            return f"{bn}+{hex(addr-base)}"
    return None

print("fault:", resolve(ex.ExceptionAddress))

# read context
loc = rec.ThreadContext
with open(path, "rb") as f:
    f.seek(loc.Rva)
    ctx = f.read(loc.DataSize)
rsp = int.from_bytes(ctx[0x98:0xA0], "little")
rip = int.from_bytes(ctx[0xF8:0x100], "little")
print("RIP", hex(rip), resolve(rip))
print("RSP", hex(rsp))

reader = mf.get_reader()
print("\n=== full stack module hits ===")
hits = 0
for i in range(0, 0x1000, 8):
    try:
        data = reader.read(rsp + i, 8)
        if not data or len(data) < 8:
            continue
        val = int.from_bytes(data, "little")
    except Exception:
        continue
    r = resolve(val)
    if not r:
        continue
    print(f"sp+{hex(i)}: {hex(val)}  {r}")
    hits += 1
    if hits >= 80:
        break

# check if main.dll / UE4SS anywhere in wider stack
print("\n=== mod dlls ===")
for base, size, bn, full in mods:
    if any(x in bn.lower() for x in ["main", "ue4ss", "cross", "defender", "palserver"]):
        print(bn, hex(base), hex(size))
