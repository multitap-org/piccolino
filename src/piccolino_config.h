#pragma once

// piccolino.cfg: the server's settings, in the data folder (hosts-common's
// settings_file format, so a NAS user can edit it by hand with piccolino
// stopped). Made on first run with the defaults below, so every setting is
// there to be seen.
//
//   name = Living room NAS          shown in piccolo's lists (default: the hostname)
//   port = 55421                    UDP: ENet + LAN discovery (NET_PICCOLINO_PORT)
//   pair_grants = share             what a newly paired device gets (FILE TRANSFER)
//   pair_grants_unsigned =          the same for a device whose build isn't signed
//
//   [folder:roms]                   one section per shared folder
//   path = files/roms               relative paths are inside the data folder
//   kind = games                    games | saves | states | screenshots | recordings | config
//   read_only = false               puts into it are refused (the same for every device)
//
// Only one folder of each kind but games (net_share_set_folders' shape);
// a second one is logged and left out. Keys this build doesn't know are kept
// and written back (settings_file), so later phases' keys survive.
//
// The grants are what the pairing pop-up's PERMISSIONS ticks start with
// (design 11.13: FILE TRANSFER on, SETTINGS off; both off for an unsigned
// build). Until the admin page exists (phase 2) they are what a device gets.

#include <cstdint>
#include <string>
#include <vector>

#include "net_share.h"
#include "net_trust.h"
#include "settings_file.h"

constexpr const char* PICCOLINO_CONFIG_FILE = "piccolino.cfg";
constexpr const char* PICCOLINO_FILES_FOLDER = "files";   // the default folders live under it

struct PiccolinoFolder
{
    std::string  name;   // the section's name after "folder:" (only for the config file and logs)
    std::string  path;   // as written: relative to the data folder or absolute
    NetShareKind kind = NetShareKind::games;
    bool         read_only = false;
};

struct PiccolinoConfig
{
    std::string name;
    uint16_t    port = 0;
    NetCaps     pair_grants = 0;
    NetCaps     pair_grants_unsigned = 0;
    std::vector<PiccolinoFolder> folders;
    SettingsFile file;   // everything read, unknown keys included (written back on save)
};

// The defaults: `host_name` as the name, NET_PICCOLINO_PORT, FILE TRANSFER
// for signed builds and nothing for unsigned ones, and the default folders
// files/{roms,saves,states,screenshots,recordings}.
void piccolino_config_defaults(PiccolinoConfig* config, const char* host_name);

// Reads `<data_dir>/piccolino.cfg`. A missing file is the first run: the
// defaults are written there. Bad values are logged and keep their default;
// false only if the file can't be read or (first run) written.
bool piccolino_config_load(PiccolinoConfig* config, const char* data_dir, const char* host_name);

// Writes every setting back to `<data_dir>/piccolino.cfg` (atomically).
bool piccolino_config_save(PiccolinoConfig* config, const char* data_dir);

// A folder's path on disk: absolute as is, relative inside `data_dir`.
std::string piccolino_config_folder_path(const PiccolinoFolder* folder, const char* data_dir);

// "games", "saves", ... and back. False for an unknown name.
const char* piccolino_kind_name(NetShareKind kind);
bool piccolino_kind_parse(const char* name, NetShareKind* kind);
