#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
check_iram.py - prove that the USB interrupt path survives SPI-flash operations.

While the SPI flash is written or erased (NVS commit, OTA, ...) the cache is
disabled: only code in IRAM/ROM may run and only data in internal RAM may be
read. An interrupt allocated with ESP_INTR_FLAG_IRAM keeps firing during that
time, so everything it can reach must live there too.

This script builds a static call graph from the disassembly of the linked ELF
(riscv32-esp-elf-objdump -d) and walks it from the ISR entry points:

  * direct calls/jumps (jal/j/call/tail and auipc+jalr pairs, which objdump
    annotates with "<symbol>") become edges;
  * an address of a function that is merely *loaded* (a function pointer, e.g.
    a callback) is treated as a possible indirect call: conservative;
  * every data address the code materialises (objdump's "# addr <sym>"
    comments for lui/auipc + addi/load/store) is classified by the section it
    falls in.

It fails if a reachable function sits in .flash.text, or reachable code
references .flash.rodata / .flash.appdesc / .flash.tdata (const tables and
strings), or external RAM. ROM (0x4fc00000..) counts as safe.

Indirect calls through tables (TinyUSB's class driver, esp_intr dispatch)
cannot be followed statically: their targets are listed as extra roots
(ROOTS below). Calls that are statically present but provably never taken in
ISR context are cut with ALLOW (each with its reason).

Usage: check_iram.py <elf> [--objdump PATH] [--root SYM ...] [-v]
Exit status 0 = OK, 1 = violations, 2 = usage/tool error.
"""
import argparse
import bisect
import os
import re
import shutil
import subprocess
import sys

# ISR entry points and the targets of the indirect calls made on the way.
ROOTS = [
    # TinyUSB DWC2 interrupt handler (dwc2_esp32.h), allocated with
    # ESP_INTR_FLAG_IRAM by our esp_intr_alloc wrapper (usb/usb_intr_iram.c)
    "dwc2_int_handler_wrap",
    # our class driver callbacks, called through usbd_class_driver_t from
    # dcd_event_handler() in ISR context
    "uac2_sof_isr",
    "uac2_xfer_isr",
    # AoIP TX packetizer wake-up (esp_timer ISR dispatch)
    "timer_cb@libmain.a",
]

# (caller, callee): edge that exists in the code but never runs in ISR
# context. Keep this list short and justified.
ALLOW = {
    # tusb_fifo.c _ff_lock()/_ff_unlock(): only taken when a FIFO has a mutex
    # (tu_fifo_config_mutex). Our class driver moves data through plain
    # buffers, never through endpoint FIFOs, and DWC2 runs in buffer-DMA mode,
    # so the FIFO helpers dcd_dwc2.c links in for slave mode never run with a
    # mutex - and never in ISR context with one.
    ("tu_fifo_read_n_access_mode", "xQueueSemaphoreTake"),
    ("tu_fifo_read_n_access_mode", "xQueueGenericSend"),
    ("tu_fifo_write_n_access_mode", "xQueueSemaphoreTake"),
    ("tu_fifo_write_n_access_mode", "xQueueGenericSend"),
    ("tu_fifo_peek_n_access_mode", "xQueueSemaphoreTake"),
    ("tu_fifo_peek_n_access_mode", "xQueueGenericSend"),
    # usbd.c dcd_event_handler() -> osal_queue_send(q, ev, in_isr): calls
    # xQueueSendToBackFromISR when in_isr (always, from the DWC2 interrupt);
    # the blocking xQueueGenericSend branch is the task-context one.
    ("dcd_event_handler", "xQueueGenericSend"),
    # FreeRTOS queue.c prvCopyDataToQueue(): xTaskPriorityDisinherit() only for
    # mutexes, which are never given from an ISR.
    ("prvCopyDataToQueue", "xTaskPriorityDisinherit"),
}

# Functions whose *address* is taken by ISR code without being called there
# (e.g. passed to a queue as a deferred callback). Not followed.
NOT_CALLED = {
}

# Objects whose data references are checked strictly (ours and TinyUSB). IDF's
# own IRAM functions reference flash strings only for assert/abort messages,
# which IDF accepts (an assert during a flash operation aborts anyway).
DATA_STRICT = ("libmain.a", "libespressif__tinyusb.a")

ROM_LO, ROM_HI = 0x4FC00000, 0x4FC40000          # ESP32-P4 mask ROM
EXTRAM_LO, EXTRAM_HI = 0x48000000, 0x4C000000    # PSRAM window

SAFE_TEXT = (".iram0.text", ".iram0.text_end", ".spm.text", ".rtc.text", ".tcm.text")
FLASH_TEXT = (".flash.text",)
FLASH_DATA = (".flash.rodata", ".flash.appdesc", ".flash.tdata", ".flash.init_array",
              ".flash_rodata_dummy")

FUNC_RE = re.compile(r"^([0-9a-f]{8}) <([^>]+)>:$")
INSN_RE = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
TARGET_RE = re.compile(r"\b([0-9a-f]{8}) <([^>+]+)(\+0x[0-9a-f]+)?>")
COMMENT_RE = re.compile(r"#\s*([0-9a-f]{8}) <([^>+]+)(\+0x[0-9a-f]+)?>")
MAP_RE = re.compile(r"^\s+(?:\S+\s+)?(0x[0-9a-f]{8,16})\s+(0x[0-9a-f]+)\s+(\S+\.(?:obj|o|a\([^)]*\)))\s*$")
CALL_MNEMONICS = {"jal", "j", "call", "tail", "jalr", "jr", "c.j", "c.jal", "c.jr", "c.jalr"}


def find_objdump(arg):
    if arg:
        return arg
    for name in ("riscv32-esp-elf-objdump", "riscv64-unknown-elf-objdump", "objdump"):
        p = shutil.which(name)
        if p:
            return p
    sys.exit("check_iram: no objdump found (source tools/idf-env.sh)")


def sections(objdump, elf):
    try:
        res = subprocess.run([objdump, "-h", elf], check=True, capture_output=True, text=True)
    except FileNotFoundError:
        sys.exit(f"check_iram: objdump '{objdump}' not runnable (exit 2 = tool error)")
    except subprocess.CalledProcessError as e:
        sys.exit(f"check_iram: '{objdump} -h {elf}' failed: {e.stderr.strip()[:200]}")
    secs = []
    for line in res.stdout.splitlines():
        f = line.split()
        if len(f) >= 7 and f[0].isdigit():
            name, size, vma = f[1], int(f[2], 16), int(f[3], 16)
            if size and vma:
                secs.append((vma, vma + size, name))
    return secs


def section_of(secs, addr):
    for lo, hi, name in secs:
        if lo <= addr < hi:
            return name
    if ROM_LO <= addr < ROM_HI:
        return "ROM"
    if EXTRAM_LO <= addr < EXTRAM_HI:
        return "EXTRAM"
    return None


def map_objects(map_path):
    """[(lo, hi, object)] from the linker map's input-section lines."""
    out = []
    if not map_path or not os.path.isfile(map_path):
        return out
    with open(map_path, errors="replace") as f:
        for line in f:
            m = MAP_RE.match(line)
            if m:
                lo, size = int(m.group(1), 16), int(m.group(2), 16)
                if size and lo:
                    out.append((lo, lo + size, m.group(3).split("/")[-1]))
    out.sort()
    return out


def object_of(objs, starts, addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0 and objs[i][0] <= addr < objs[i][1]:
        return objs[i][2]
    return "?"


def disassemble(objdump, elf):
    """Return {addr: [name, [(insn_addr, mnemonic, operands)]]}, keyed by address
    (static functions in different files may share a name)."""
    p = subprocess.Popen([objdump, "-d", "--no-show-raw-insn", elf],
                         stdout=subprocess.PIPE, text=True, bufsize=1 << 20)
    funcs, cur = {}, None
    for line in p.stdout:
        line = line.rstrip("\n")
        m = FUNC_RE.match(line)
        if m:
            cur = int(m.group(1), 16)
            funcs.setdefault(cur, [m.group(2), []])
            continue
        if cur is None:
            continue
        m = INSN_RE.match(line)
        if m:
            funcs[cur][1].append((int(m.group(1), 16), m.group(2), m.group(3)))
    if p.wait() != 0:
        sys.exit("check_iram: objdump failed")
    return funcs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("--map", help="linker map (default: <elf>.map); attributes functions to objects")
    ap.add_argument("--objdump")
    ap.add_argument("--root", action="append", default=[])
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    if not os.path.isfile(a.elf):
        print(f"check_iram: {a.elf} not found", file=sys.stderr)
        return 2
    objdump = find_objdump(a.objdump)
    secs = sections(objdump, a.elf)
    funcs = disassemble(objdump, a.elf)
    starts = sorted(funcs)
    objs = map_objects(a.map or os.path.splitext(a.elf)[0] + ".map")
    ostarts = [o[0] for o in objs]

    def func_at(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i] if i >= 0 else None

    def label(fa):
        return f"{funcs[fa][0]}[{object_of(objs, ostarts, fa)}]" if fa in funcs else f"{fa:#010x}"

    by_name = {}
    for fa, (name, _) in funcs.items():
        by_name.setdefault(name, []).append(fa)

    roots = []
    for r in ROOTS + a.root:
        name, _, obj = r.partition("@")
        cand = [fa for fa in by_name.get(name, []) if not obj or object_of(objs, ostarts, fa).startswith(obj)]
        if len(cand) != 1:
            print(f"check_iram: root '{r}' matches {len(cand)} functions "
                  f"({', '.join(label(c) for c in cand) or 'none: renamed or inlined?'})", file=sys.stderr)
            return 2     # tool-config error, NOT a firmware IRAM violation
        roots.append(cand[0])

    def edges(fa):
        name, insns = funcs[fa]
        out, refs = set(), set()
        for _, mn, ops in insns:
            if mn in CALL_MNEMONICS:
                m = TARGET_RE.search(ops) or COMMENT_RE.search(ops)
                if m:
                    t = int(m.group(1), 16)
                    if section_of(secs, t) == "ROM" and t not in funcs:
                        continue
                    tf = func_at(t)
                    if tf is not None and tf != fa:
                        out.add(tf)
                continue
            m = COMMENT_RE.search(ops)
            if not m:
                continue
            addr, sym = int(m.group(1), 16), m.group(2)
            if m.group(3) and int(m.group(3)[1:], 16) >= 0x10000:
                continue                # a constant that merely looks like an address
            sec = section_of(secs, addr)
            if sec in FLASH_TEXT or sec in SAFE_TEXT:
                if addr in funcs and addr != fa and funcs[addr][0] not in NOT_CALLED:
                    out.add(addr)       # function pointer: may be called
            elif sec in FLASH_DATA or sec == "EXTRAM":
                refs.add((addr, sym, sec))
        return out, refs

    parent = {r: None for r in roots}
    order, stack, data = [], list(roots), {}
    while stack:
        f = stack.pop()
        order.append(f)
        out, refs = edges(f)
        data[f] = refs
        for g in sorted(out):
            if (funcs[f][0], funcs[g][0]) in ALLOW or g in parent:
                continue
            parent[g] = f
            stack.append(g)

    def chain(f):
        out = []
        while f is not None:
            out.append(funcs[f][0])
            f = parent[f]
        return " <- ".join(out)

    def strict(f):
        o = object_of(objs, ostarts, f)
        return any(o.startswith(p) for p in DATA_STRICT)

    bad, soft = [], 0
    for f in order:
        sec = section_of(secs, f)
        if sec not in SAFE_TEXT and sec != "ROM":
            bad.append(f"code  {label(f)} @ {f:#010x} in {sec}: {chain(f)}")
        for daddr, sym, dsec in sorted(data[f]):
            if strict(f):
                bad.append(f"data  {sym} @ {daddr:#010x} in {dsec} used by {label(f)}: {chain(f)}")
            else:
                soft += 1
                if a.verbose:
                    print(f"  note: {label(f)} references {sym} @ {daddr:#010x} ({dsec}); "
                          "IDF IRAM function, assumed assert-path only")

    if a.verbose:
        for f in sorted(order, key=lambda x: funcs[x][0]):
            print(f"  {label(f):60s} {section_of(secs, f)}")
    print(f"check_iram: {len(order)} functions reachable from {len(roots)} ISR roots "
          f"({soft} flash-data references inside IDF IRAM functions not checked)")
    if bad:
        print(f"check_iram: FAIL, {len(bad)} flash-resident items on the ISR path:", file=sys.stderr)
        for b in bad:
            print("  " + b, file=sys.stderr)
        return 1
    print("check_iram: OK, the ISR path is entirely in IRAM/ROM and internal RAM")
    return 0


if __name__ == "__main__":
    sys.exit(main())
