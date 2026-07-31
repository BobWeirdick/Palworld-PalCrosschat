from minidump.minidumpfile import MinidumpFile
import sys

path = sys.argv[1] if len(sys.argv) > 1 else r"c:\Users\James\Downloads\crash_2026_07_25_16_32_47.4964131.dmp"
mf = MinidumpFile.parse(path)

exlist = mf.exception
rec = exlist.exception_records[0]
ex = rec.ExceptionRecord
tid = rec.ThreadId
fault = ex.ExceptionAddress
code = ex.ExceptionCode
info = list(ex.ExceptionInformation) if ex.ExceptionInformation is not None else []

print("=== EXCEPTION ===")
print(f"ThreadId={tid}")
print(f"Code={code}")
print(f"Address={hex(fault)}")
print(f"Info={info} ({[hex(x) for x in info]})")

mods = []
for m in mf.modules.modules:
    name = m.name or "?"
    bn = name.split("\\")[-1]
    size = getattr(m, "size", None) or getattr(m, "sizeofimage", None) or 0
    mods.append((m.baseaddress, size, bn, name))
mods.sort(key=lambda x: x[0])

print("\n=== ALL MOD / GAME DLLS (filtered) ===")
for base, size, bn, full in mods:
    low = (bn + " " + full).lower()
    if any(x in low for x in ["ue4ss", "mods\\", "paldefender", "palserver", "shipping.exe", "crosschat", "serverevents"]):
        print(f"{bn}: {hex(base)} size={hex(size)} | {full}")

def resolve(addr):
    for base, size, bn, full in mods:
        if size and base <= addr < base + size:
            return bn, addr - base, full
    return None

r = resolve(fault)
print("\n=== FAULT MODULE ===")
if r:
    print(f"{r[0]}+{hex(r[1])} ({r[2]})")
else:
    print("unresolved")

# Find exception thread context
print("\n=== EXCEPTION THREAD CONTEXT ===")
reader = mf.get_reader()
exc_thread = None
for t in mf.threads.threads:
    if t.ThreadId == tid:
        exc_thread = t
        break

rip = rsp = None
if exc_thread is not None:
    # Context is a location descriptor; read CONTEXT from dump
    loc = rec.ThreadContext
    print(f"ThreadContext rva={getattr(loc, 'Rva', None)} data_size={getattr(loc, 'DataSize', None)}")
    try:
        # Minidump AMD64 CONTEXT: Rip at offset 0xF8, Rsp at 0x98 (Windows x64 CONTEXT)
        ctx_bytes = mf.file_handle.read_bytes_at(loc.Rva, loc.DataSize) if hasattr(mf, "file_handle") else None
    except Exception as e:
        ctx_bytes = None
        print("read ctx via file_handle failed", e)

    # Use reader via absolute? ThreadContext RVA is file offset in minidump
    try:
        mf.file_handle.seek(0)
    except Exception:
        pass

    # minidumpfile stores filename; reopen
    with open(path, "rb") as f:
        f.seek(loc.Rva)
        ctx = f.read(loc.DataSize)
    print(f"context bytes={len(ctx)}")
    if len(ctx) >= 0x100:
        rsp = int.from_bytes(ctx[0x98:0xA0], "little")
        rip = int.from_bytes(ctx[0xF8:0x100], "little")
        print(f"RIP={hex(rip)} RSP={hex(rsp)}")
        rr = resolve(rip)
        if rr:
            print(f"RIP in {rr[0]}+{hex(rr[1])}")

print("\n=== STACK POINTERS (module hits) ===")
if rsp and reader:
    hits = 0
    for i in range(0, 0x800, 8):
        try:
            data = reader.read(rsp + i, 8)
            if not data or len(data) < 8:
                continue
            val = int.from_bytes(data, "little")
        except Exception:
            continue
        rr = resolve(val)
        if not rr:
            continue
        bn, off, full = rr
        low = bn.lower()
        if any(x in low for x in ["ue4ss", "serverevent", "crosschat", "paldefender", "palserver", "main"]):
            print(f"sp+{hex(i)}: {hex(val)}  {bn}+{hex(off)}")
            hits += 1
            if hits >= 60:
                break

# Explicitly check if PalCrosschat main.dll present
print("\n=== PalCrosschat loaded? ===")
found = False
for base, size, bn, full in mods:
    if "crosschat" in full.lower() or (bn.lower() == "main.dll" and "mods" in full.lower()):
        print(f"YES: {bn} @ {hex(base)} {full}")
        found = True
if not found:
    print("NO — PalCrosschat DLL not in module list")
