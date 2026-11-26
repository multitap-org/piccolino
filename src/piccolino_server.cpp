#include "piccolino_server.h"

#include <cstdio>
#include <cstring>

#include "host_fs.h"
#include "host_log.h"
#include "piccolino_lan.h"

static const char* kind_text(NetAppKind kind)
{
    return net_app_kind_name(kind);
}

// "share,settings" for the log, "nothing" for none.
static std::string caps_text(NetCaps caps)
{
    std::string text = net_caps_name(caps);
    return text.empty() ? std::string("nothing") : text;
}

static void address_text(const NetAddress* address, char* out, size_t capacity)
{
    net_address_to_text(*address, out, capacity);
}

static int32_t find_request(const PiccolinoServer* server, uint32_t peer)
{
    for (size_t i = 0; i < server->requests.size(); i++)
    {
        if (server->requests[i].peer == peer)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

static void drop_request(PiccolinoServer* server, uint32_t peer)
{
    int32_t index = find_request(server, peer);
    if (index >= 0)
    {
        server->requests.erase(server->requests.begin() + index);
    }
}

// A device asks to pair: LAN only, then the code is shown (stdout + log)
// with what it will get.
static void on_pairing_requested(PiccolinoServer* server, const NetAccessEvent* event)
{
    char where[64] = {};
    address_text(&event->address, where, sizeof(where));
    NetTransport transport = net_host_peer_transport(&server->access.session.host, event->peer);
    if (transport != NetTransport::enet || !piccolino_address_is_lan(&event->address))
    {
        // Not refused for the run (net_access_refuse_pairing): the same
        // device may come back on the LAN later.
        host_log(LogLevel::warning, "Pairing: \"%s\" asked from %s, not the LAN: closed (piccolino pairs on the LAN only)",
                 event->info.name, where);
        server->refused_not_lan++;
        drop_request(server, event->peer);
        net_access_close(&server->access, event->peer);
        return;
    }
    NetCaps wanted = event->signed_build ? server->pair_grants : server->pair_grants_unsigned;
    net_access_set_pairing_grants(&server->access, event->peer, wanted);
    PiccolinoRequest request;
    request.peer = event->peer;
    std::memcpy(request.code, event->code, sizeof(request.code));
    request.name = event->info.name;
    request.kind = event->info.kind;
    request.signed_build = event->signed_build;
    request.grants = server->access.peers[event->peer].grants;   // what set_pairing_grants kept
    int32_t index = find_request(server, event->peer);
    if (index >= 0)
    {
        server->requests[static_cast<size_t>(index)] = request;
    }
    else
    {
        server->requests.push_back(request);
    }
    host_log(LogLevel::info, "Pairing: \"%s\" (%s, %s build) asks from %s%s; it gets: %s", request.name.c_str(),
             kind_text(request.kind), request.signed_build ? "signed" : "unsigned", where,
             event->retry ? " (the last code was wrong)" : "", caps_text(request.grants).c_str());
    // The line a person (or the end-to-end test) reads: the code alone, on stdout.
    std::printf("PAIRING CODE %s for \"%s\": type it on that device within %d minutes\n", request.code,
                request.name.c_str(), static_cast<int>(NET_ACCESS_REQUEST_SECONDS / 60.0));
    std::fflush(stdout);
}

// The shared folder a virtual path starts in ("ROMS/gba/x.gba" -> "ROMS").
static bool in_read_only_root(const PiccolinoServer* server, const std::string* path)
{
    size_t slash = path->find('/');
    std::string root = slash == std::string::npos ? *path : path->substr(0, slash);
    for (size_t i = 0; i < server->read_only_roots.size(); i++)
    {
        if (server->read_only_roots[i] == root)
        {
            return true;
        }
    }
    return false;
}

// A put into a read-only folder: refused before net_share_host sees it.
static bool refuse_read_only_put(PiccolinoServer* server, const NetAccessEvent* event)
{
    if (event->channel != NetChannel::control || server->read_only_roots.empty())
    {
        return false;
    }
    uint32_t id = 0;
    uint64_t file_size = 0;
    uint64_t mtime = 0;
    uint8_t flags = 0;
    std::string path;
    if (!net_share_read_put_request(event->data, event->size, &id, &file_size, &mtime, &flags, &path) ||
        !in_read_only_root(server, &path))
    {
        return false;
    }
    host_log(LogLevel::info, "Share: put %s refused: read-only folder", path.c_str());
    server->refused_read_only++;
    size_t size = net_share_write_put_ready(server->out.data(), server->out.size(), id, NetShareStatus::refused, 0);
    if (size > 0)
    {
        net_access_send(&server->access, event->peer, NetChannel::control, server->out.data(), size);
    }
    return true;
}

static void on_event(PiccolinoServer* server, const NetAccessEvent* event)
{
    if (event->type == NetAccessEventType::message && refuse_read_only_put(server, event))
    {
        return;
    }
    if (net_share_host_handle(&server->share, &server->access, event))
    {
        return;
    }
    switch (event->type)
    {
        case NetAccessEventType::pairing_requested:
            on_pairing_requested(server, event);
            break;
        case NetAccessEventType::paired:
            drop_request(server, event->peer);
            host_log(LogLevel::info, "Pairing: \"%s\" (%s) paired; it may use: %s", event->info.name,
                     kind_text(event->info.kind), caps_text(event->caps).c_str());
            std::printf("PAIRED \"%s\"\n", event->info.name);
            std::fflush(stdout);
            break;
        case NetAccessEventType::authorized:
            host_log(LogLevel::info, "\"%s\" (%s) connected; it may use: %s", event->info.name,
                     kind_text(event->info.kind), caps_text(event->caps).c_str());
            break;
        case NetAccessEventType::pairing_failed:
            drop_request(server, event->peer);
            host_log(LogLevel::warning, "Pairing: wrong codes or a broken request: closed");
            break;
        case NetAccessEventType::closed:
            drop_request(server, event->peer);
            net_share_host_peer_closed(&server->share, event->peer);
            if (event->device >= 0)
            {
                host_log(LogLevel::info, "A paired device left");
            }
            break;
        case NetAccessEventType::unpaired:
            host_log(LogLevel::info, "\"%s\" unpaired itself: forgotten", event->info.name);
            break;
        case NetAccessEventType::granted:
            break;   // piccolino uses nothing: what a device grants us doesn't matter
        case NetAccessEventType::message:
            // SETTINGS (the settings client) isn't served yet: piccolo's SETTINGS
            // client waits for the end of the freeze (design 11.3, Q16).
            host_log(LogLevel::debug, "A message piccolino doesn't serve yet (type %u): ignored",
                     event->size > 0 ? static_cast<unsigned>(event->data[0]) : 0u);
            break;
        default:
            break;   // codes, guests: piccolino shows no code of its own and has no guest links
    }
}

static bool copy_running(const PiccolinoServer* server)
{
    for (uint32_t i = 0; i < NET_SHARE_HOST_MAX_COPIES; i++)
    {
        if (server->share.copies[i].peer != NET_NO_PEER)
        {
            return true;
        }
    }
    return false;
}

bool piccolino_server_poll(PiccolinoServer* server)
{
    bool busy = false;
    NetAccessEvent event;
    while (net_access_poll(&server->access, &event))
    {
        on_event(server, &event);
        busy = true;
    }
    net_share_host_pump(&server->share, &server->access, false);
    return busy || copy_running(server);
}

void piccolino_server_set_folders(PiccolinoServer* server, const PiccolinoConfig* config)
{
    NetShareFolderPaths paths;
    std::vector<std::string> read_only_paths;   // canonical
    for (size_t i = 0; i < config->folders.size(); i++)
    {
        const PiccolinoFolder* folder = &config->folders[i];
        std::string path = piccolino_config_folder_path(folder, server->data_dir.c_str());
        char canonical[4096] = {};
        if (!host_fs_is_directory(path.c_str()) || !host_fs_canonical(path.c_str(), canonical, sizeof(canonical)))
        {
            host_log(LogLevel::warning, "Folder %s (%s): %s isn't there: not shared", folder->name.c_str(),
                     piccolino_kind_name(folder->kind), path.c_str());
            continue;
        }
        switch (folder->kind)
        {
            case NetShareKind::saves:
                paths.saves = canonical;
                break;
            case NetShareKind::states:
                paths.states = canonical;
                break;
            case NetShareKind::screenshots:
                paths.screenshots = canonical;
                break;
            case NetShareKind::recordings:
                paths.recordings = canonical;
                break;
            case NetShareKind::config:
                paths.config = canonical;
                break;
            default:
                paths.games.push_back(canonical);
                break;
        }
        if (folder->read_only)
        {
            read_only_paths.push_back(canonical);
        }
    }
    net_share_host_set_folders(&server->share, &paths);
    // The names devices see come from net_share_set_folders ("SAVES", "roms",
    // "roms 2"): matched back by path.
    server->read_only_roots.clear();
    const std::vector<NetShareRoot>* roots = &server->share.folders.roots;
    for (size_t i = 0; i < roots->size(); i++)
    {
        bool read_only = false;
        for (size_t j = 0; j < read_only_paths.size() && !read_only; j++)
        {
            read_only = (*roots)[i].path == read_only_paths[j];
        }
        if (read_only)
        {
            server->read_only_roots.push_back((*roots)[i].name);
        }
        host_log(LogLevel::info, "Sharing %s%s: %s", (*roots)[i].name.c_str(), read_only ? " (read only)" : "",
                 (*roots)[i].path.c_str());
    }
}

// The folders inside the data folder are ours to make; one elsewhere that's
// missing may be a disk that isn't mounted yet: left alone (logged later).
static void make_folders(const PiccolinoConfig* config, const char* data_dir)
{
    std::string base = std::string(data_dir) + "/";
    for (size_t i = 0; i < config->folders.size(); i++)
    {
        std::string path = piccolino_config_folder_path(&config->folders[i], data_dir);
        if (path.compare(0, base.size(), base) == 0 && !host_fs_exists(path.c_str()))
        {
            host_fs_make_directories(path.c_str());
        }
    }
}

bool piccolino_server_create(PiccolinoServer* server, const PiccolinoConfig* config, const char* data_dir)
{
    server->data_dir = data_dir;
    server->pair_grants = config->pair_grants;
    server->pair_grants_unsigned = config->pair_grants_unsigned;
    server->out.resize(NET_LINK_MAX_PAYLOAD);
    std::string identity_path = server->data_dir + "/" + PICCOLINO_IDENTITY_FILE;
    std::string devices_path = server->data_dir + "/" + PICCOLINO_DEVICES_FILE;
    NetKeypair identity;
    if (!net_identity_load_or_create(identity_path.c_str(), &identity))
    {
        host_log(LogLevel::error, "No identity (%s): piccolino can't run without one", identity_path.c_str());
        return false;
    }
    NetDeviceInfo info;
    std::snprintf(info.name, sizeof(info.name), "%s", config->name.c_str());
    info.kind = NetAppKind::piccolino;
    info.offers = NET_CAP_SHARE | NET_CAP_SETTINGS;
    info.uses = 0;
    server->access_created = true;
    if (!net_access_create(&server->access, config->port, &info, &identity, devices_path.c_str()))
    {
        host_log(LogLevel::error, "Can't listen on UDP port %u (another piccolino running?)",
                 static_cast<unsigned>(config->port));
        net_keypair_wipe(&identity);
        return false;
    }
    net_keypair_wipe(&identity);
    // No build proof check yet (net_build_verify is frontends-common's):
    // every device reads as unsigned.
    net_access_set_verify_build(&server->access, nullptr, nullptr);
    server->access.accept_pairing = true;
    make_folders(config, data_dir);
    net_share_host_start(&server->share);
    server->share_started = true;
    piccolino_server_set_folders(server, config);
    return true;
}

void piccolino_server_destroy(PiccolinoServer* server)
{
    if (server->share_started)
    {
        net_share_host_stop(&server->share);
        server->share_started = false;
    }
    if (server->access_created)
    {
        net_access_destroy(&server->access);
        server->access_created = false;
    }
    server->requests.clear();
}

uint16_t piccolino_server_port(const PiccolinoServer* server)
{
    return net_host_port(&server->access.session.host);
}
