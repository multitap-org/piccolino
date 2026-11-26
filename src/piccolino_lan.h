#pragma once

// Which addresses count as the owner's network (Andrés, 2026-11-25: pairing
// with piccolino is LAN only, and Tailscale counts as LAN, design 11.7).
// Pairing requests are answered only from these; the admin page (phase 2)
// takes the same list for its source filter.
//
//   IPv4  10/8, 172.16/12, 192.168/16, 100.64/10 (CGNAT, Tailscale), 127/8, 169.254/16
//   IPv6  fc00::/7 (ULA), fe80::/10 (link-local), ::1
//
// IPv4 also when it comes as ::ffff:a.b.c.d (ENet's one dual-stack socket).

#include "net_host.h"

bool piccolino_address_is_lan(const NetAddress* address);
