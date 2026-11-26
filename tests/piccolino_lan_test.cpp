// Checks piccolino_lan.h: which source addresses count as the owner's
// network (design 11.7), at the edges of each range, IPv4 as it comes from
// ENet's dual-stack socket (::ffff:a.b.c.d) and IPv6.
//
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>

#include "piccolino_lan.h"

static int g_failures = 0;

static void check_address(const char* text, bool lan)
{
    NetAddress address;
    if (!net_address_from_text(text, 55421, &address))
    {
        std::printf("FAIL %s doesn't parse\n", text);
        g_failures++;
        return;
    }
    if (piccolino_address_is_lan(&address) != lan)
    {
        std::printf("FAIL %s should %sbe LAN\n", text, lan ? "" : "not ");
        g_failures++;
    }
}

int main()
{
    // Address parsing needs the network set up (WSAStartup on Windows).
    if (!net_init())
    {
        std::printf("FAIL net_init\n");
        return 1;
    }
    check_address("10.0.0.1", true);
    check_address("10.255.255.255", true);
    check_address("11.0.0.1", false);
    check_address("172.16.0.1", true);
    check_address("172.31.255.254", true);
    check_address("172.15.0.1", false);
    check_address("172.32.0.1", false);
    check_address("192.168.1.20", true);
    check_address("192.169.0.1", false);
    check_address("100.64.0.1", true);       // CGNAT / Tailscale
    check_address("100.127.255.254", true);
    check_address("100.63.255.255", false);
    check_address("100.128.0.1", false);
    check_address("127.0.0.1", true);
    check_address("169.254.10.1", true);
    check_address("169.253.0.1", false);
    check_address("8.8.8.8", false);
    check_address("203.0.113.5", false);
    check_address("::1", true);
    check_address("fd12:3456::1", true);     // ULA
    check_address("fc00::1", true);
    check_address("fe80::1", true);          // link-local
    check_address("febf::1", true);
    check_address("fec0::1", false);
    check_address("2001:db8::5", false);
    check_address("::", false);
    check_address("::ffff:8.8.8.8", false);
    check_address("::ffff:192.168.0.2", true);
    net_shutdown();
    if (g_failures == 0)
    {
        std::printf("piccolino lan: all checks passed\n");
        return 0;
    }
    std::printf("piccolino lan: %d check(s) failed\n", g_failures);
    return 1;
}
