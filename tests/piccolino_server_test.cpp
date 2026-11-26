// Checks piccolino_server.h in one process, over loopback, with
// multitap-net's own SHARE client (net_share_link, as piccolo's FILE
// TRANSFER runs it) on its thread and the server polled here:
//   - discovery v2: the reply says piccolino, its name and its offers;
//   - pairing: the request shows a code (the receiver makes it), typing it
//     pairs, the device gets the config's grants for an unsigned build and
//     is saved in net_devices.cfg; the identity file is private;
//   - a put to a mirrored path (missing folders made there) and a get back
//     into the same relative path on our side;
//   - a put into a read-only folder is refused;
//   - with the default config an unsigned build pairs but gets no FILE
//     TRANSFER.
// The binary with net_probe as the client is tests/e2e_net_probe.py.
//
// Usage: piccolino_server_test <scratch folder> (emptied by ctest first)
// Exit code 0 = all checks passed. Each failure is printed.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "host_fs.h"
#include "host_log.h"
#include "net_share_link.h"
#include "piccolino_server.h"

constexpr double WAIT_SECONDS = 20.0;

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

static void write_text(const std::string* path, const char* text)
{
    host_fs_write_file(path->c_str(), text, std::strlen(text));
}

static std::string read_text(const std::string* path)
{
    uint8_t* data = nullptr;
    size_t size = 0;
    if (!host_fs_read_file(path->c_str(), &data, &size))
    {
        return std::string();
    }
    std::string text(reinterpret_cast<const char*>(data), size);
    delete[] data;
    return text;
}

static double seconds_since(std::chrono::steady_clock::time_point start)
{
    std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    return elapsed.count();
}

static void poll_a_while(PiccolinoServer* server, int milliseconds)
{
    for (int i = 0; i < milliseconds; i++)
    {
        piccolino_server_poll(server);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// The client side: a device with its own identity, paired devices and one
// shared folder ("mine").
struct TestClient
{
    NetShareLink* link = nullptr;
    std::string   dir;
    std::string   mine;
};

static bool client_start(TestClient* client, const std::string* dir, const char* name)
{
    client->dir = *dir;
    client->mine = *dir + "/mine";
    host_fs_make_directories(client->mine.c_str());
    std::string identity_path = *dir + "/net_identity.cfg";
    std::string devices_path = *dir + "/net_devices.cfg";
    NetKeypair identity;
    if (!net_identity_load_or_create(identity_path.c_str(), &identity))
    {
        return false;
    }
    NetDeviceInfo info;
    std::snprintf(info.name, sizeof(info.name), "%s", name);
    info.kind = NetAppKind::piccolo;
    info.offers = NET_CAPS_ALL;
    info.uses = NET_CAP_SHARE | NET_CAP_SETTINGS;
    client->link = new NetShareLink();
    bool started = net_share_link_start(client->link, &identity, &info, devices_path.c_str());
    net_keypair_wipe(&identity);
    if (!started)
    {
        delete client->link;
        client->link = nullptr;
        return false;
    }
    char canonical[4096] = {};
    host_fs_canonical(client->mine.c_str(), canonical, sizeof(canonical));
    NetShareFolderPaths paths;
    paths.games.push_back(canonical);
    net_share_link_set_folders(client->link, &paths);
    return true;
}

static void client_stop(TestClient* client)
{
    if (client->link != nullptr)
    {
        net_share_link_disconnect(client->link);
        net_share_link_stop(client->link);
        delete client->link;
        client->link = nullptr;
    }
}

// Connects asking to pair, types the code the server shows, and waits until
// the link is ready. Returns the request the server showed (peer
// NET_NO_PEER if there was none).
static PiccolinoRequest client_pair(TestClient* client, PiccolinoServer* server, NetShareLinkStatus* status)
{
    PiccolinoRequest shown;
    NetAddress address;
    net_address_from_text("127.0.0.1", piccolino_server_port(server), &address);
    net_share_link_connect(client->link, address, true);
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    bool typed = false;
    while (seconds_since(start) < WAIT_SECONDS)
    {
        piccolino_server_poll(server);
        net_share_link_status(client->link, status);
        if (!typed && status->code_needed && server->requests.size() == 1)
        {
            shown = server->requests[0];
            check(net_share_link_enter_code(client->link, shown.code), "the shown code is taken");
            typed = true;
        }
        if (status->state == NetShareLinkState::ready)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // What the server grants is told right after the welcome.
    poll_a_while(server, 200);
    net_share_link_status(client->link, status);
    return shown;
}

// One copy, to its end. Returns its status (failed if it didn't end in time).
static NetShareStatus client_copy(TestClient* client, PiccolinoServer* server, bool to_remote, const char* local,
                                  const char* remote, uint64_t size, uint64_t mtime)
{
    net_share_link_copy(client->link, to_remote, local, remote, size, mtime, 0);
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::vector<NetShareClientDone> done;
    while (seconds_since(start) < WAIT_SECONDS)
    {
        piccolino_server_poll(server);
        net_share_link_take_done(client->link, &done);
        if (!done.empty())
        {
            return done[0].status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::printf("copy %s: out of time\n", remote);
    return NetShareStatus::failed;
}

static void check_discovery(PiccolinoServer* server)
{
    NetHost* host = new NetHost();
    bool found = false;
    if (net_host_create_client(host))
    {
        NetAddress address;
        net_address_from_text("127.0.0.1", piccolino_server_port(server), &address);
        net_host_discover_at(host, address);
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        while (!found && seconds_since(start) < 5.0)
        {
            piccolino_server_poll(server);
            NetEvent event;
            while (net_host_poll(host, &event))
            {
                if (event.type != NetEventType::discovered)
                {
                    continue;
                }
                found = true;
                check(event.discovered.version == 2, "discovery: version 2");
                check(event.discovered.kind == static_cast<uint8_t>(NetAppKind::piccolino), "discovery: piccolino");
                check(event.discovered.offers == (NET_CAP_SHARE | NET_CAP_SETTINGS),
                      "discovery: offers SHARE + SETTINGS");
                check(std::strcmp(event.discovered.name, "test server") == 0, "discovery: its name");
                check(event.discovered.port == piccolino_server_port(server), "discovery: its port");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    net_host_destroy(host);
    delete host;
    check(found, "discovery: a reply");
}

static void check_pair_and_copy(const char* scratch)
{
    std::string base = std::string(scratch) + "/main";
    std::string data = base + "/data";
    host_fs_make_directories(data.c_str());
    PiccolinoConfig config;
    piccolino_config_defaults(&config, "test server");
    config.port = 0;   // any free port
    config.pair_grants_unsigned = NET_CAP_SHARE;   // as an owner who ticked FILE TRANSFER for it
    PiccolinoFolder archive;
    archive.name = "archive";
    archive.path = "archive";
    archive.kind = NetShareKind::games;
    archive.read_only = true;
    config.folders.push_back(archive);

    PiccolinoServer* server = new PiccolinoServer();
    check(piccolino_server_create(server, &config, data.c_str()), "server starts");
    check(host_fs_is_directory((data + "/files/roms").c_str()), "default folders made");
    check(host_fs_is_directory((data + "/archive").c_str()), "a folder inside the data folder is made");
    check(server->read_only_roots.size() == 1 && server->read_only_roots[0] == "archive", "archive is read only");
#if !defined(_WIN32)
    struct stat identity_stat = {};
    check(stat((data + "/net_identity.cfg").c_str(), &identity_stat) == 0 && (identity_stat.st_mode & 0077) == 0,
          "net_identity.cfg is readable by the user only");
#endif
    std::string save = data + "/files/saves/gba/Game.srm";
    write_text(&save, "battery save");

    check_discovery(server);

    TestClient client;
    std::string client_dir = base + "/client";
    check(client_start(&client, &client_dir, "TEST CLIENT"), "client starts");
    if (client.link != nullptr)
    {
        NetShareLinkStatus status;
        PiccolinoRequest shown = client_pair(&client, server, &status);
        check(shown.peer != NET_NO_PEER, "pairing: the server showed a code");
        check(net_pair_code_valid(shown.code), "pairing: a 9-digit code");
        check(shown.name == "TEST CLIENT" && !shown.signed_build, "pairing: the request names an unsigned client");
        check(shown.grants == NET_CAP_SHARE, "pairing: it would get FILE TRANSFER");
        check(status.state == NetShareLinkState::ready && status.newly_paired, "pairing: paired");
        check((status.caps & NET_CAP_SHARE) != 0 && (status.caps & NET_CAP_SETTINGS) == 0,
              "pairing: FILE TRANSFER granted, SETTINGS not");
        check(status.host_kind == NetAppKind::piccolino, "pairing: the host says piccolino");
        check(server->requests.empty(), "pairing: no request left");
        NetTrust trust;
        net_trust_load(&trust, (data + "/net_devices.cfg").c_str());
        check(trust.devices.size() == 1 && trust.devices[0].name == "TEST CLIENT" &&
                  trust.devices[0].grants == NET_CAP_SHARE,
              "pairing: saved in net_devices.cfg with its grants");

        // Put to a mirrored path: missing folders made on the server.
        std::string game = client.mine + "/gba/Game.gba";
        write_text(&game, "a game, mirrored");
        NetShareStatus put = client_copy(&client, server, true, "mine/gba/Game.gba", "roms/gba/Game.gba", 0, 0);
        check(put == NetShareStatus::ok, "put: ok");
        std::string landed = data + "/files/roms/gba/Game.gba";
        check(read_text(&landed) == "a game, mirrored", "put: the file is on the server at the same path");

        // Get into the same relative path on our side.
        uint64_t size = 0;
        uint64_t mtime = 0;
        host_fs_file_info(save.c_str(), &size, &mtime);
        NetShareStatus get = client_copy(&client, server, false, "mine/saves/gba/Game.srm", "SAVES/gba/Game.srm",
                                         size, mtime);
        check(get == NetShareStatus::ok, "get: ok");
        std::string got = client.mine + "/saves/gba/Game.srm";
        check(read_text(&got) == "battery save", "get: the file is here at the same path");

        // A read-only folder refuses puts.
        NetShareStatus refused = client_copy(&client, server, true, "mine/gba/Game.gba", "archive/Game.gba", 0, 0);
        check(refused == NetShareStatus::refused, "read only: put refused");
        check(server->refused_read_only == 1, "read only: counted");
        check(!host_fs_exists((data + "/archive/Game.gba").c_str()), "read only: nothing written");
        check(server->refused_not_lan == 0, "loopback is the LAN");
    }
    client_stop(&client);
    poll_a_while(server, 100);
    piccolino_server_destroy(server);
    delete server;
}

static void check_unsigned_default(const char* scratch)
{
    std::string base = std::string(scratch) + "/defaults";
    std::string data = base + "/data";
    host_fs_make_directories(data.c_str());
    PiccolinoConfig config;
    piccolino_config_defaults(&config, "test server");
    config.port = 0;
    PiccolinoServer* server = new PiccolinoServer();
    check(piccolino_server_create(server, &config, data.c_str()), "default server starts");
    TestClient client;
    std::string client_dir = base + "/client";
    check(client_start(&client, &client_dir, "UNSIGNED"), "second client starts");
    if (client.link != nullptr)
    {
        NetShareLinkStatus status;
        PiccolinoRequest shown = client_pair(&client, server, &status);
        check(shown.peer != NET_NO_PEER && shown.grants == 0, "default: an unsigned build would get nothing");
        check(status.state == NetShareLinkState::ready && (status.caps & NET_CAP_SHARE) == 0,
              "default: paired without FILE TRANSFER");
    }
    client_stop(&client);
    poll_a_while(server, 100);
    piccolino_server_destroy(server);
    delete server;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("FAIL no scratch folder given\n");
        return 1;
    }
    host_log_set_min_level(LogLevel::warning);
    check_pair_and_copy(argv[1]);
    check_unsigned_default(argv[1]);
    if (g_failures == 0)
    {
        std::printf("piccolino server: all checks passed\n");
        return 0;
    }
    std::printf("piccolino server: %d check(s) failed\n", g_failures);
    return 1;
}
