#!/usr/bin/env python3
"""piccolino end to end, with frontends-common's net_probe as the client.

Runs the piccolino binary on a free UDP port in a fresh data folder, then:
  1. discovery: net_probe asks it and finds it by name;
  2. pairing: net_probe asks to pair, piccolino prints the code, the code
     is typed into net_probe (stdin), and the same run puts a file to a
     mirrored path (roms/gba/Game.gba: the folders are made on the server);
  3. as a paired device: a listing of the shared folders, a get from
     STATES, and a put into a read-only folder (refused);
  4. SIGINT (Ctrl+C on Windows) stops piccolino cleanly, and the paired
     device is in net_devices.cfg.

net_probe gets files into its one shared folder (no subfolders): that's
net_probe's own behaviour, not piccolino's.

Usage: e2e_net_probe.py <piccolino> <net_probe> <scratch folder>
Prints "piccolino e2e: all checks passed" at the end; each failure is a
"FAIL" line.
"""

import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time

failures = 0


def check(condition, what):
    global failures
    if not condition:
        print("FAIL " + what, flush=True)
        failures += 1


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="") as f:
        f.write(text)


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return None


class Server:
    """The piccolino process, with its stdout and stderr read on threads."""

    def __init__(self, binary, data):
        flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
        self.process = subprocess.Popen([binary, "--data", data], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, creationflags=flags)
        self.lines = []
        self.lock = threading.Lock()
        self.readers = [threading.Thread(target=self.read, args=(self.process.stdout, "")),
                        threading.Thread(target=self.read, args=(self.process.stderr, "[err] "))]
        for reader in self.readers:
            reader.daemon = True
            reader.start()

    def read(self, stream, prefix):
        for line in stream:
            line = line.rstrip("\n")
            print("  piccolino| " + prefix + line, flush=True)
            with self.lock:
                self.lines.append(line)

    def wait_for(self, pattern, seconds, start=0):
        """The first match of `pattern` in a line from `start` on, or None."""
        end = time.time() + seconds
        while time.time() < end:
            with self.lock:
                for line in self.lines[start:]:
                    match = re.search(pattern, line)
                    if match:
                        return match
            if self.process.poll() is not None:
                break
            time.sleep(0.02)
        return None

    def line_count(self):
        with self.lock:
            return len(self.lines)

    def stop(self):
        if self.process.poll() is None:
            if os.name == "nt":
                self.process.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                self.process.send_signal(signal.SIGINT)
        try:
            self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        for reader in self.readers:
            reader.join(timeout=5)
        return self.process.returncode


def probe(net_probe, args, stdin_text=None, server=None, timeout=60):
    """Runs net_probe; with `server`, types the code piccolino prints."""
    print("net_probe " + " ".join(args), flush=True)
    process = subprocess.Popen([net_probe] + args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    if server is not None:
        start = server.line_count()
        match = server.wait_for(r"PAIRING CODE (\d{9})", 20, start)
        check(match is not None, "piccolino printed a pairing code")
        stdin_text = (match.group(1) if match else "") + "\n"
    try:
        output, _ = process.communicate(stdin_text, timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _ = process.communicate()
        check(False, "net_probe ended in time")
    for line in output.splitlines():
        print("  net_probe| " + line, flush=True)
    return process.returncode, output


def main():
    if len(sys.argv) < 4:
        print("FAIL usage: e2e_net_probe.py <piccolino> <net_probe> <scratch folder>")
        return 1
    piccolino, net_probe, scratch = sys.argv[1], sys.argv[2], sys.argv[3]
    data = os.path.join(scratch, "data")
    probe_dir = os.path.join(scratch, "probe")
    mine = os.path.join(probe_dir, "mine")
    port = free_udp_port()

    # The owner ticked FILE TRANSFER for unsigned builds (net_probe is one),
    # and keeps an archive folder read only.
    write(os.path.join(data, "piccolino.cfg"),
          "name = e2e piccolino\n"
          "port = %d\n"
          "pair_grants = share\n"
          "pair_grants_unsigned = share\n"
          "[folder:roms]\npath = files/roms\nkind = games\n"
          "[folder:states]\npath = files/states\nkind = states\n"
          "[folder:archive]\npath = archive\nkind = games\nread_only = true\n" % port)
    write(os.path.join(data, "files", "states", "gba", "Game.state1"), "state one")
    write(os.path.join(mine, "gba", "Game.gba"), "a game, mirrored")

    server = Server(piccolino, data)
    started = server.wait_for(r"listening on UDP port (\d+)", 20)
    check(started is not None and int(started.group(1)) == port, "piccolino listens on its port")
    if started is None:
        server.stop()
        print("piccolino e2e: piccolino didn't start")
        return 1
    common = ["--port", str(port), "--dir", probe_dir, "--share-dir", mine]

    # 1. Discovery.
    code, output = probe(net_probe, ["--port", str(port), "--at", "127.0.0.1", "--seconds", "2"])
    check('"e2e piccolino"' in output and "1 found" in output, "discovery finds piccolino by name")

    # 2. Pairing (the code from piccolino's stdout) and a put to a mirrored path.
    code, output = probe(net_probe, ["--pair", "127.0.0.1"] + common +
                         ["--share-put", "gba/Game.gba", "roms/gba/Game.gba"], server=server)
    check(code == 0, "pair + put: net_probe succeeds")
    check("FILE TRANSFER" in output and "put: ok" in output, "pair + put: paired with FILE TRANSFER, put ok")
    check(server.wait_for(r'PAIRED "NET PROBE"', 5) is not None, "piccolino says paired")
    check(read(os.path.join(data, "files", "roms", "gba", "Game.gba")) == "a game, mirrored",
          "put: on the server at the mirrored path")

    # 3. As a paired device: list, get, a refused put.
    code, output = probe(net_probe, ["--connect", "127.0.0.1"] + common + ["--share-list", ""])
    check(code == 0 and "roms" in output and "STATES" in output and "archive" in output,
          "list: the shared folders")
    code, output = probe(net_probe, ["--connect", "127.0.0.1"] + common + ["--share-get", "STATES/gba/Game.state1"])
    check(code == 0 and "get: ok" in output, "get: ok")
    check(read(os.path.join(mine, "Game.state1")) == "state one", "get: the file arrived")
    code, output = probe(net_probe, ["--connect", "127.0.0.1"] + common +
                         ["--share-put", "gba/Game.gba", "archive/gba/Game.gba"])
    check(code != 0 and "put: refused" in output, "read only: put refused")
    check(not os.path.exists(os.path.join(data, "archive", "gba", "Game.gba")), "read only: nothing written")

    # 4. Stop.
    result = server.stop()
    check(result == 0, "piccolino stops cleanly (exit 0)")
    devices = read(os.path.join(data, "net_devices.cfg")) or ""
    check("name = NET PROBE" in devices and "grants = share" in devices, "net_devices.cfg has net_probe")
    if os.name != "nt":
        mode = os.stat(os.path.join(data, "net_identity.cfg")).st_mode & 0o077
        check(mode == 0, "net_identity.cfg is private")

    if failures == 0:
        print("piccolino e2e: all checks passed")
        return 0
    print("piccolino e2e: %d check(s) failed" % failures)
    return 1


if __name__ == "__main__":
    sys.exit(main())
