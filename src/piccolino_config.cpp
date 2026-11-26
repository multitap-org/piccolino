#include "piccolino_config.h"

#include <cstdlib>
#include <cstring>

#include "host_fs.h"
#include "host_log.h"
#include "net_host.h"

static const char* CONFIG_HEADER = "piccolino settings: edit with piccolino stopped (it writes this file too).\n"
                                   "Relative folder paths are inside the data folder.";
static const char* FOLDER_PREFIX = "folder:";

struct KindName
{
    NetShareKind kind;
    const char*  name;
};

static const KindName KIND_NAMES[] = {
    {NetShareKind::games, "games"},
    {NetShareKind::saves, "saves"},
    {NetShareKind::states, "states"},
    {NetShareKind::screenshots, "screenshots"},
    {NetShareKind::recordings, "recordings"},
    {NetShareKind::config, "config"},
};

const char* piccolino_kind_name(NetShareKind kind)
{
    for (size_t i = 0; i < sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0]); i++)
    {
        if (KIND_NAMES[i].kind == kind)
        {
            return KIND_NAMES[i].name;
        }
    }
    return "?";
}

bool piccolino_kind_parse(const char* name, NetShareKind* kind)
{
    for (size_t i = 0; i < sizeof(KIND_NAMES) / sizeof(KIND_NAMES[0]); i++)
    {
        if (std::strcmp(KIND_NAMES[i].name, name) == 0)
        {
            *kind = KIND_NAMES[i].kind;
            return true;
        }
    }
    return false;
}

static void add_folder(PiccolinoConfig* config, const char* name, NetShareKind kind)
{
    PiccolinoFolder folder;
    folder.name = name;
    folder.path = std::string(PICCOLINO_FILES_FOLDER) + "/" + name;
    folder.kind = kind;
    config->folders.push_back(folder);
}

void piccolino_config_defaults(PiccolinoConfig* config, const char* host_name)
{
    config->name = host_name != nullptr && host_name[0] != '\0' ? host_name : "piccolino";
    if (config->name.size() > NET_DEVICE_NAME_MAX)
    {
        config->name.resize(NET_DEVICE_NAME_MAX);
    }
    config->port = NET_PICCOLINO_PORT;
    config->pair_grants = NET_CAP_SHARE;
    config->pair_grants_unsigned = 0;
    config->folders.clear();
    add_folder(config, "roms", NetShareKind::games);
    add_folder(config, "saves", NetShareKind::saves);
    add_folder(config, "states", NetShareKind::states);
    add_folder(config, "screenshots", NetShareKind::screenshots);
    add_folder(config, "recordings", NetShareKind::recordings);
    config->file = SettingsFile{};
}

static std::string config_path(const char* data_dir)
{
    return std::string(data_dir) + "/" + PICCOLINO_CONFIG_FILE;
}

static void read_caps(const SettingsFile* file, const char* key, NetCaps* caps)
{
    const char* value = settings_file_get(file, "", key);
    if (value == nullptr)
    {
        return;
    }
    if (!net_caps_parse(value, caps))
    {
        host_log(LogLevel::warning, "piccolino.cfg: %s = %s isn't a list of gamepad, stream, settings, share: "
                 "keeping %s", key, value, net_caps_name(*caps).c_str());
    }
}

static bool read_bool(const char* value, bool* out)
{
    if (std::strcmp(value, "true") == 0)
    {
        *out = true;
        return true;
    }
    if (std::strcmp(value, "false") == 0)
    {
        *out = false;
        return true;
    }
    return false;
}

static bool has_kind(const std::vector<PiccolinoFolder>* folders, NetShareKind kind)
{
    for (size_t i = 0; i < folders->size(); i++)
    {
        if ((*folders)[i].kind == kind)
        {
            return true;
        }
    }
    return false;
}

// The [folder:NAME] sections. With none at all the defaults stay (a file
// written by hand with only the global keys still shares something).
static void read_folders(PiccolinoConfig* config)
{
    std::vector<PiccolinoFolder> folders;
    bool any = false;
    size_t prefix = std::strlen(FOLDER_PREFIX);
    for (size_t s = 0; s < config->file.sections.size(); s++)
    {
        const SettingsFileSection* section = &config->file.sections[s];
        if (section->name.compare(0, prefix, FOLDER_PREFIX) != 0)
        {
            continue;
        }
        any = true;
        PiccolinoFolder folder;
        folder.name = section->name.substr(prefix);
        const char* section_name = section->name.c_str();
        const char* path = settings_file_get(&config->file, section_name, "path");
        const char* kind = settings_file_get(&config->file, section_name, "kind");
        const char* read_only = settings_file_get(&config->file, section_name, "read_only");
        if (folder.name.empty() || path == nullptr || path[0] == '\0')
        {
            host_log(LogLevel::warning, "piccolino.cfg: [%s] needs a name and a path: left out", section_name);
            continue;
        }
        folder.path = path;
        if (kind != nullptr && !piccolino_kind_parse(kind, &folder.kind))
        {
            host_log(LogLevel::warning, "piccolino.cfg: [%s] kind = %s is unknown: left out", section_name, kind);
            continue;
        }
        if (read_only != nullptr && !read_bool(read_only, &folder.read_only))
        {
            host_log(LogLevel::warning, "piccolino.cfg: [%s] read_only = %s isn't true or false: false",
                     section_name, read_only);
        }
        if (folder.kind != NetShareKind::games && has_kind(&folders, folder.kind))
        {
            host_log(LogLevel::warning, "piccolino.cfg: [%s] is a second %s folder (only one is shared): left out",
                     section_name, piccolino_kind_name(folder.kind));
            continue;
        }
        folders.push_back(folder);
    }
    if (any)
    {
        config->folders = folders;
    }
}

// Puts the struct's values into the file (keeping the keys it doesn't know).
static bool store(PiccolinoConfig* config)
{
    SettingsFile* file = &config->file;
    std::string port = std::to_string(config->port);
    bool ok = settings_file_set(file, "", "name", config->name.c_str());
    ok = settings_file_set(file, "", "port", port.c_str()) && ok;
    ok = settings_file_set(file, "", "pair_grants", net_caps_name(config->pair_grants).c_str()) && ok;
    ok = settings_file_set(file, "", "pair_grants_unsigned", net_caps_name(config->pair_grants_unsigned).c_str()) && ok;
    // Folder sections are written fresh: one that was dropped goes.
    size_t prefix = std::strlen(FOLDER_PREFIX);
    for (size_t s = file->sections.size(); s > 0; s--)
    {
        if (file->sections[s - 1].name.compare(0, prefix, FOLDER_PREFIX) == 0)
        {
            file->sections.erase(file->sections.begin() + static_cast<std::ptrdiff_t>(s - 1));
        }
    }
    for (size_t i = 0; i < config->folders.size(); i++)
    {
        const PiccolinoFolder* folder = &config->folders[i];
        std::string section = std::string(FOLDER_PREFIX) + folder->name;
        ok = settings_file_set(file, section.c_str(), "path", folder->path.c_str()) && ok;
        ok = settings_file_set(file, section.c_str(), "kind", piccolino_kind_name(folder->kind)) && ok;
        ok = settings_file_set(file, section.c_str(), "read_only", folder->read_only ? "true" : "false") && ok;
    }
    return ok;
}

bool piccolino_config_load(PiccolinoConfig* config, const char* data_dir, const char* host_name)
{
    piccolino_config_defaults(config, host_name);
    std::string path = config_path(data_dir);
    bool first_run = !host_fs_exists(path.c_str());
    if (first_run)
    {
        host_log(LogLevel::info, "piccolino.cfg: first run, writing the defaults to %s", path.c_str());
        return piccolino_config_save(config, data_dir);
    }
    if (!settings_file_load(&config->file, path.c_str()))
    {
        return false;
    }
    const SettingsFile* file = &config->file;
    const char* name = settings_file_get(file, "", "name");
    if (name != nullptr && name[0] != '\0')
    {
        config->name = name;
        if (config->name.size() > NET_DEVICE_NAME_MAX)
        {
            host_log(LogLevel::warning, "piccolino.cfg: name is longer than %u bytes: cut",
                     static_cast<unsigned>(NET_DEVICE_NAME_MAX));
            config->name.resize(NET_DEVICE_NAME_MAX);
        }
    }
    const char* port = settings_file_get(file, "", "port");
    if (port != nullptr)
    {
        char* end = nullptr;
        long value = std::strtol(port, &end, 10);
        if (end == port || *end != '\0' || value < 1 || value > 65535)
        {
            host_log(LogLevel::warning, "piccolino.cfg: port = %s isn't 1-65535: keeping %u", port,
                     static_cast<unsigned>(config->port));
        }
        else
        {
            config->port = static_cast<uint16_t>(value);
        }
    }
    read_caps(file, "pair_grants", &config->pair_grants);
    read_caps(file, "pair_grants_unsigned", &config->pair_grants_unsigned);
    read_folders(config);
    return true;
}

bool piccolino_config_save(PiccolinoConfig* config, const char* data_dir)
{
    if (!store(config))
    {
        host_log(LogLevel::error, "piccolino.cfg: a value can't be written (a line break or spaces at its ends?)");
        return false;
    }
    std::string path = config_path(data_dir);
    return settings_file_save(&config->file, path.c_str(), CONFIG_HEADER);
}

static bool is_absolute(const std::string* path)
{
    if (!path->empty() && ((*path)[0] == '/' || (*path)[0] == '\\'))
    {
        return true;
    }
    // "C:/..." or "C:\..." (Windows; harmless elsewhere: no folder is named "C:").
    return path->size() >= 3 && (*path)[1] == ':' && ((*path)[2] == '/' || (*path)[2] == '\\');
}

std::string piccolino_config_folder_path(const PiccolinoFolder* folder, const char* data_dir)
{
    if (is_absolute(&folder->path))
    {
        return folder->path;
    }
    return std::string(data_dir) + "/" + folder->path;
}
