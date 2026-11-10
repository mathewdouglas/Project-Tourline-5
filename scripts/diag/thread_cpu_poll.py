"""Per-thread CPU of gt5.exe in short steps, while a command (a test route) runs.

    python scripts/diag/thread_cpu_poll.py <out.csv> [--step-ms 50] -- <command...>

Finds the first gt5.exe started after this script, then every step writes one row per thread that used
CPU in that step: unix_ms, host thread id, CPU ms in the step (kernel + user), host priority, and the logical
CPU it last ran on. Thread names come from the run log afterwards ("[THREAD n] ... hosttid=", "[hostthread]
<name> hosttid="). Read-only: it opens threads with THREAD_QUERY_LIMITED_INFORMATION and never suspends them.
It answers "which threads were running while the render thread was runnable but not scheduled", which
PS3_TIMELINE (waits only) cannot.
"""
import argparse
import ctypes
import subprocess
import sys
import threading
import time
from ctypes import wintypes as W

k = ctypes.WinDLL('kernel32', use_last_error=True)
TH32CS_SNAPPROCESS, TH32CS_SNAPTHREAD = 2, 4


class PE(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('pid', W.DWORD), ('heap', ctypes.c_void_p),
                ('module', W.DWORD), ('threads', W.DWORD), ('parent', W.DWORD), ('pri', W.LONG),
                ('flags', W.DWORD), ('exe', W.WCHAR * 260)]


class TE(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('tid', W.DWORD), ('pid', W.DWORD),
                ('basePri', W.LONG), ('deltaPri', W.LONG), ('flags', W.DWORD)]


class PN(ctypes.Structure):
    _fields_ = [('Group', W.WORD), ('Number', ctypes.c_ubyte), ('Reserved', ctypes.c_ubyte)]


k.CreateToolhelp32Snapshot.restype = W.HANDLE
k.CreateToolhelp32Snapshot.argtypes = [W.DWORD, W.DWORD]
k.OpenThread.restype = W.HANDLE
k.OpenThread.argtypes = [W.DWORD, W.BOOL, W.DWORD]
k.GetThreadTimes.argtypes = [W.HANDLE] + [ctypes.POINTER(W.FILETIME)] * 4
k.GetThreadPriority.argtypes = [W.HANDLE]
k.GetThreadIdealProcessorEx.argtypes = [W.HANDLE, ctypes.POINTER(PN)]
k.CloseHandle.argtypes = [W.HANDLE]
for name, struct in (('Process32FirstW', PE), ('Process32NextW', PE), ('Thread32First', TE), ('Thread32Next', TE)):
    getattr(k, name).argtypes = [W.HANDLE, ctypes.POINTER(struct)]


def games():
    snap = k.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PE(); e.dwSize = ctypes.sizeof(PE); out = set()
    ok = k.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.exe.lower() == 'gt5.exe':
            out.add(e.pid)
        ok = k.Process32NextW(snap, ctypes.byref(e))
    k.CloseHandle(snap)
    return out


def threads(pid):
    snap = k.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = TE(); e.dwSize = ctypes.sizeof(TE); out = []
    ok = k.Thread32First(snap, ctypes.byref(e))
    while ok:
        if e.pid == pid:
            out.append(e.tid)
        ok = k.Thread32Next(snap, ctypes.byref(e))
    k.CloseHandle(snap)
    return out


def poll(path, step, before, stop):
    pid = None
    while pid is None and not stop.is_set():
        new = games() - before
        pid = next(iter(new)) if new else None
        if pid is None:
            time.sleep(0.2)
    if pid is None:
        return
    handles, last, refresh = {}, {}, 0.0
    ft = [W.FILETIME() for _ in range(4)]
    with open(path, 'x', encoding='ascii') as out:
        out.write(f'#pid {pid}\nunix_ms,tid,cpu_ms,priority,ideal_cpu\n')
        while not stop.is_set():
            now = time.time()
            if now - refresh > 1.0:
                refresh = now
                current = threads(pid)
                if not current:
                    break
                for tid in current:
                    if tid not in handles:
                        h = k.OpenThread(0x0800, False, tid)   # THREAD_QUERY_LIMITED_INFORMATION
                        if h:
                            handles[tid] = h
            stamp = int(now * 1000)
            for tid, h in list(handles.items()):
                if not k.GetThreadTimes(h, *[ctypes.byref(f) for f in ft]):
                    k.CloseHandle(h); del handles[tid]; last.pop(tid, None)
                    continue
                total = sum((f.dwHighDateTime << 32 | f.dwLowDateTime) for f in ft[2:]) / 10000.0
                previous = last.get(tid)
                last[tid] = total
                if previous is not None and total - previous >= 0.5:
                    n = PN(); k.GetThreadIdealProcessorEx(h, ctypes.byref(n))
                    out.write(f'{stamp},{tid},{total - previous:.1f},{k.GetThreadPriority(h)},{n.Number}\n')
            time.sleep(step)
    for h in handles.values():
        k.CloseHandle(h)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('output')
    ap.add_argument('--step-ms', type=int, default=50)
    ap.add_argument('command', nargs=argparse.REMAINDER)
    a = ap.parse_args()
    command = a.command[1:] if a.command[:1] == ['--'] else a.command
    if not command:
        ap.error('a command is required after --')
    stop = threading.Event()
    worker = threading.Thread(target=poll, args=(a.output, a.step_ms / 1000.0, games(), stop), daemon=True)
    worker.start()
    try:
        code = subprocess.run(command).returncode
    finally:
        stop.set(); worker.join(timeout=5)
    sys.exit(code)


if __name__ == '__main__':
    main()
