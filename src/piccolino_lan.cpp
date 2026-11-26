#include "piccolino_lan.h"

static bool ipv4_is_lan(const uint8_t* ip)
{
    if (ip[0] == 10 || ip[0] == 127)
    {
        return true;
    }
    if (ip[0] == 172 && (ip[1] & 0xF0) == 16)
    {
        return true;
    }
    if (ip[0] == 192 && ip[1] == 168)
    {
        return true;
    }
    if (ip[0] == 100 && (ip[1] & 0xC0) == 64)
    {
        return true;
    }
    return ip[0] == 169 && ip[1] == 254;
}

bool piccolino_address_is_lan(const NetAddress* address)
{
    const uint8_t* ip = address->ip;
    // ::ffff:a.b.c.d
    bool mapped = ip[10] == 0xFF && ip[11] == 0xFF;
    for (int i = 0; i < 10 && mapped; i++)
    {
        mapped = ip[i] == 0;
    }
    if (mapped)
    {
        return ipv4_is_lan(ip + 12);
    }
    // ::1
    bool loopback = ip[15] == 1;
    for (int i = 0; i < 15 && loopback; i++)
    {
        loopback = ip[i] == 0;
    }
    if (loopback)
    {
        return true;
    }
    if ((ip[0] & 0xFE) == 0xFC)
    {
        return true;   // fc00::/7
    }
    return ip[0] == 0xFE && (ip[1] & 0xC0) == 0x80;   // fe80::/10
}
