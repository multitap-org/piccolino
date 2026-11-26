# piccolino

The self-hosted Multitap file server. piccolo and piccoletto pair with it on
your network and copy games, saves, states, screenshots and recordings to
and from its folders, the same way they copy between two piccolos (FILE
TRANSFER).

piccolino is a Multitap host that offers FILE TRANSFER (and SETTINGS); it
is never a client, runs no games and streams nothing.

**Status: phase 1, the headless core.** It runs, pairs and shares files.
The admin web page, the desktop tray, packages and reaching it from outside
your network come next.

## Build

```sh
git submodule update --init --checkout
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

C++20, CMake 3.25+. No other dependencies: everything is in `deps/`.

## Run

```sh
build/piccolino --data /srv/piccolino
```

- `--data DIR`: where it keeps everything (default: `~/.local/share/piccolino`,
  `%LOCALAPPDATA%\piccolino` on Windows).
- `--log-level debug|info|warning|error`, `--version`.

The first run writes `piccolino.cfg` in the data folder with the defaults;
stop piccolino to edit it:

```ini
# Shown in piccolo's lists (default: the host name).
name = Living room NAS
# UDP: connections and LAN discovery.
port = 55421
# What a newly paired device may use; the same for a build not signed by Multitap.
pair_grants = share
pair_grants_unsigned =

# One section per shared folder. Relative paths are inside the data folder.
# kind: games, saves, states, screenshots, recordings or config.
[folder:roms]
path = files/roms
kind = games
read_only = false
```

The default folders are `files/roms`, `files/saves`, `files/states`,
`files/screenshots` and `files/recordings` in the data folder.

## Pairing

Pairing is started from the other device, on the same network: PAIR A
DEVICE in piccolo or piccoletto, then pick piccolino. piccolino prints the
code:

```
PAIRING CODE 123456789 for "Ana's piccolo": type it on that device within 5 minutes
```

Type it on that device. piccolino answers pairing requests only from your
network (private addresses, including Tailscale's 100.64/10); once paired,
a device keeps its access.

A build that isn't signed by Multitap gets `pair_grants_unsigned` (nothing by
default). This build can't check signatures yet, so for now every device
counts as unsigned: set `pair_grants_unsigned = share` to let your devices
copy files.

## Tests

```sh
ctest --test-dir build
```

The end-to-end test runs the binary against frontends-common's `net_probe`
(built from piccolo): pass `-DPICCOLINO_NET_PROBE=/path/to/net_probe`.

## License

MIT (see LICENSE). The libraries in `deps/` have their own licenses.
