#pragma once

// The server: multitap-net's host side (NetAccess: connections, pairing,
// the paired devices) and its SHARE side (net_share_host: listings and
// copies), polled together from main's single-threaded loop. No net_service:
// that's for a frontend whose frame loop mustn't wait on the network, and it
// drags in remote play. piccolino has no frames.
//
// piccolino offers SHARE + SETTINGS and uses nothing: it is never a client
// and never grants anything back. Discovery v2 replies (kind piccolino, the
// offers) come from NetAccess's listener on the configured UDP port.
//
// Pairing (design 10, 11.11, 11.12): always started by the other device on
// the LAN. piccolino is the receiver, so it makes the code (NetAccess does,
// on pair_ask) and shows it: here, printed to stdout and the log, until the
// admin page exists. A request is answered only when it came straight over
// ENet from a LAN address (piccolino_lan.h); any other is closed without a
// code. A new device gets the config's pair_grants (pair_grants_unsigned for
// an unsigned build), kept to what it uses.
//
// Signed builds: the build proof check (net_build_verify) is in
// frontends-common, which piccolino doesn't link, so for now every device
// reads as unsigned and gets pair_grants_unsigned. See the notes for the move.
//
// Read-only folders: a put_request into one is answered `refused` here,
// before net_share_host sees it (design 3: no protocol change; the client
// shows today's generic refusal).

#include <cstdint>
#include <string>
#include <vector>

#include "net_access.h"
#include "net_share_host.h"
#include "piccolino_config.h"

// A pairing request waiting for its code to be typed on the other device.
struct PiccolinoRequest
{
    uint32_t    peer = NET_NO_PEER;
    char        code[NET_PAIR_CODE_DIGITS + 1] = {};
    std::string name;
    NetAppKind  kind = NetAppKind::piccolo;
    bool        signed_build = false;
    NetCaps     grants = 0;   // what it gets once paired (kept to what it uses)
};

struct PiccolinoServer
{
    NetAccess    access;
    NetShareHost share;
    bool         net_started = false;     // net_init() (WSAStartup on Windows), undone in destroy
    bool         access_created = false;
    bool         share_started = false;
    std::string  data_dir;
    NetCaps      pair_grants = 0;
    NetCaps      pair_grants_unsigned = 0;
    std::vector<std::string>      read_only_roots;   // shared folders' names (as devices see them) that refuse puts
    std::vector<PiccolinoRequest> requests;
    std::vector<uint8_t>          out;               // our own answers (read-only refusals)
    uint32_t     refused_not_lan = 0;    // pairing requests closed: not from the LAN over ENet
    uint32_t     refused_read_only = 0;  // puts refused: read-only folder
};

constexpr const char* PICCOLINO_IDENTITY_FILE = "net_identity.cfg";
constexpr const char* PICCOLINO_DEVICES_FILE = "net_devices.cfg";

// Loads (or makes, 0600) the identity in `data_dir`, opens the paired
// devices list there, makes the folders inside the data folder that are
// missing, and starts listening on config->port. False (logged) if any of
// that fails; piccolino_server_destroy is still needed.
bool piccolino_server_create(PiccolinoServer* server, const PiccolinoConfig* config, const char* data_dir);
void piccolino_server_destroy(PiccolinoServer* server);

// Everything the network has: pairing, SHARE messages, copies. True if
// anything happened or a copy runs (the loop then polls again soon).
bool piccolino_server_poll(PiccolinoServer* server);

// The shared folders as devices see them (the config's, those that exist).
void piccolino_server_set_folders(PiccolinoServer* server, const PiccolinoConfig* config);

// The UDP port it listens on (the configured one, or the one picked for port 0).
uint16_t piccolino_server_port(const PiccolinoServer* server);
