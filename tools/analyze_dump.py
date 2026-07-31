"""Minidump triage for PalServer crashes.

Usage: python analyze_dump.py <dump path>

Prints the exception record, faulting thread registers, the module that owns
each interesting address, and a stack scan of module return addresses.
"""

import sys

from minidump.minidumpfile import MinidumpFile


def main(path: str) -> None:
    mdf = MinidumpFile.parse(path)

    modules = []
    if mdf.modules:
        for m in mdf.modules.modules:
            modules.append((m.baseaddress, m.baseaddress + m.size, m.name))
    modules.sort()

    def resolve(addr: int) -> str:
        for lo, hi, name in modules:
            if lo <= addr < hi:
                short = name.rsplit("\\", 1)[-1]
                return f"{short}+0x{addr - lo:x}"
        return ""

    exc = mdf.exception
    exc_tid = None
    if exc and exc.exception_records:
        rec = exc.exception_records[0]
        er = rec.ExceptionRecord
        exc_tid = rec.ThreadId
        code = er.ExceptionCode
        code_val = code.value if hasattr(code, "value") else int(code)
        print(f"ExceptionCode : 0x{code_val:08x} ({code})")
        print(f"ExceptionAddr : 0x{er.ExceptionAddress:016x} {resolve(er.ExceptionAddress)}")
        print(f"ThreadId      : {exc_tid}")
        params = list(er.ExceptionInformation or [])
        if code_val == 0xC0000005 and len(params) >= 2:
            kind = {0: "READ", 1: "WRITE", 8: "EXECUTE"}.get(params[0], str(params[0]))
            print(f"AccessViolation: {kind} at 0x{params[1]:016x} {resolve(params[1])}")
        print()

    # Registers + stack for the faulting thread.
    if exc_tid is None or not mdf.threads:
        print("No exception thread context available")
        return

    thread = None
    for t in mdf.threads.threads:
        if t.ThreadId == exc_tid:
            thread = t
            break
    if thread is None:
        print("Faulting thread not found in dump")
        return

    ctx = getattr(thread, "ContextObject", None)
    regs = {}
    if ctx is not None:
        for reg in ("Rax", "Rbx", "Rcx", "Rdx", "Rsi", "Rdi", "Rbp", "Rsp",
                    "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15", "Rip"):
            val = getattr(ctx, reg, None)
            if val is not None:
                regs[reg.lower()] = val

    print("Registers:")
    for name, val in regs.items():
        loc = resolve(val)
        print(f"  {name:<4} = 0x{val:016x} {loc}")
    print()

    reader = mdf.get_reader()

    def read_mem(addr: int, size: int) -> bytes:
        try:
            buf_reader = reader.get_buffered_reader()
            buf_reader.move(addr)
            return buf_reader.read(size)
        except Exception:
            return b""

    # Try to show what registers point at (UTF-16 preview helps spot FString data).
    print("Register memory previews:")
    for name, val in regs.items():
        if val < 0x10000:
            continue
        data = read_mem(val, 64)
        if not data:
            continue
        try:
            text = data.decode("utf-16-le", errors="replace")
            printable = "".join(ch if 32 <= ord(ch) < 0x3000 else "." for ch in text)
        except Exception:
            printable = ""
        print(f"  [{name}] {data[:32].hex()} | u16:'{printable[:24]}'")
    print()

    rsp = regs.get("rsp")
    if not rsp:
        return
    print("Stack scan (module return addresses):")
    stack = read_mem(rsp, 0x3000)
    if not stack:
        print("  <stack memory not in dump>")
        return
    shown = 0
    for off in range(0, len(stack) - 7, 8):
        val = int.from_bytes(stack[off:off + 8], "little")
        loc = resolve(val)
        if loc:
            print(f"  rsp+0x{off:04x}: 0x{val:016x} {loc}")
            shown += 1
            if shown >= 60:
                break


if __name__ == "__main__":
    main(sys.argv[1])
