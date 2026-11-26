// Checks piccolino_config.h: the first run writes the defaults (and reads
// them back the same), hand edits are read (name, port, grants, folders with
// kinds, absolute paths and read_only), bad values keep their defaults, a
// second folder of a fixed kind is left out, unknown keys survive a save, and
// folder paths resolve against the data folder.
//
// Usage: piccolino_config_test <scratch folder> (emptied by ctest first)
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>
#include <string>

#include "host_fs.h"
#include "piccolino_config.h"

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

static const PiccolinoFolder* find_folder(const PiccolinoConfig* config, const char* name)
{
    for (size_t i = 0; i < config->folders.size(); i++)
    {
        if (config->folders[i].name == name)
        {
            return &config->folders[i];
        }
    }
    return nullptr;
}

static void check_defaults(const char* scratch)
{
    std::string dir = std::string(scratch) + "/defaults";
    host_fs_make_directories(dir.c_str());
    PiccolinoConfig config;
    check(piccolino_config_load(&config, dir.c_str(), "nas"), "first run loads");
    std::string path = dir + "/piccolino.cfg";
    check(host_fs_exists(path.c_str()), "first run writes piccolino.cfg");
    check(config.name == "nas", "default name is the host name");
    check(config.port == 55421, "default port is 55421");
    check(config.pair_grants == NET_CAP_SHARE, "default grants: FILE TRANSFER");
    check(config.pair_grants_unsigned == 0, "default grants for unsigned builds: nothing");
    check(config.folders.size() == 5, "five default folders");
    const PiccolinoFolder* roms = find_folder(&config, "roms");
    check(roms != nullptr && roms->kind == NetShareKind::games && roms->path == "files/roms" && !roms->read_only,
          "roms: games, files/roms, writable");
    const PiccolinoFolder* recordings = find_folder(&config, "recordings");
    check(recordings != nullptr && recordings->kind == NetShareKind::recordings, "recordings folder");
    std::string text = read_text(&path);
    check(text.find("pair_grants_unsigned =") != std::string::npos, "the file shows pair_grants_unsigned");
    check(text.find("[folder:screenshots]") != std::string::npos, "the file shows the folders");

    // Read back: the same.
    PiccolinoConfig again;
    check(piccolino_config_load(&again, dir.c_str(), "other"), "second run loads");
    check(again.name == "nas", "the saved name wins over the host name");
    check(again.port == 55421 && again.pair_grants == NET_CAP_SHARE && again.pair_grants_unsigned == 0,
          "values read back");
    check(again.folders.size() == 5, "folders read back");

    PiccolinoConfig empty_host;
    piccolino_config_defaults(&empty_host, "");
    check(empty_host.name == "piccolino", "no host name: piccolino");
}

static void check_edits(const char* scratch)
{
    std::string dir = std::string(scratch) + "/edited";
    host_fs_make_directories(dir.c_str());
    std::string path = dir + "/piccolino.cfg";
    write_text(&path,
               "name = Living room NAS\n"
               "port = 55491\n"
               "pair_grants = share, settings\n"
               "pair_grants_unsigned = share\n"
               "future_key = kept\n"
               "[folder:games]\n"
               "path = /srv/games\n"
               "kind = games\n"
               "read_only = true\n"
               "[folder:more]\n"
               "path = more\n"
               "[folder:saves]\n"
               "path = s\n"
               "kind = saves\n"
               "[folder:saves2]\n"
               "path = s2\n"
               "kind = saves\n"
               "[folder:odd]\n"
               "path = odd\n"
               "kind = movies\n"
               "[folder:nopath]\n"
               "kind = states\n"
               "[folder:conf]\n"
               "path = C:/piccolo/config\n"
               "kind = config\n"
               "read_only = maybe\n"
               "[other]\n"
               "x = 1\n");
    PiccolinoConfig config;
    check(piccolino_config_load(&config, dir.c_str(), "nas"), "edited file loads");
    check(config.name == "Living room NAS", "name read");
    check(config.port == 55491, "port read");
    check(config.pair_grants == (NET_CAP_SHARE | NET_CAP_SETTINGS), "grants read");
    check(config.pair_grants_unsigned == NET_CAP_SHARE, "unsigned grants read");
    check(config.folders.size() == 4, "four folders kept (second saves, unknown kind, no path left out)");
    const PiccolinoFolder* games = find_folder(&config, "games");
    check(games != nullptr && games->read_only && games->path == "/srv/games", "games: read only, absolute");
    const PiccolinoFolder* more = find_folder(&config, "more");
    check(more != nullptr && more->kind == NetShareKind::games && !more->read_only, "no kind: games");
    check(find_folder(&config, "saves2") == nullptr, "a second saves folder is left out");
    check(find_folder(&config, "odd") == nullptr, "an unknown kind is left out");
    check(find_folder(&config, "nopath") == nullptr, "a folder without a path is left out");
    const PiccolinoFolder* conf = find_folder(&config, "conf");
    check(conf != nullptr && conf->kind == NetShareKind::config && !conf->read_only, "bad read_only: false");

    check(piccolino_config_folder_path(games, dir.c_str()) == "/srv/games", "absolute path as is");
    check(piccolino_config_folder_path(more, dir.c_str()) == dir + "/more", "relative path inside the data folder");
    check(piccolino_config_folder_path(conf, dir.c_str()) == "C:/piccolo/config", "drive path as is");

    // Saving keeps unknown keys and sections, and drops the folders left out.
    check(piccolino_config_save(&config, dir.c_str()), "save");
    std::string text = read_text(&path);
    check(text.find("future_key = kept") != std::string::npos, "unknown key kept");
    check(text.find("[other]") != std::string::npos, "unknown section kept");
    check(text.find("[folder:saves2]") == std::string::npos, "dropped folder not written back");
    check(text.find("pair_grants = settings,share") != std::string::npos, "grants written in bit order");
}

static void check_bad_values(const char* scratch)
{
    std::string dir = std::string(scratch) + "/bad";
    host_fs_make_directories(dir.c_str());
    std::string path = dir + "/piccolino.cfg";
    write_text(&path, "port = 70000\npair_grants = everything\nname =\n");
    PiccolinoConfig config;
    check(piccolino_config_load(&config, dir.c_str(), "nas"), "bad values still load");
    check(config.port == 55421, "bad port keeps the default");
    check(config.pair_grants == NET_CAP_SHARE, "bad grants keep the default");
    check(config.name == "nas", "empty name keeps the host name");
    check(config.folders.size() == 5, "no folder sections: the defaults");

    write_text(&path, "port = 12ab\n");
    check(piccolino_config_load(&config, dir.c_str(), "nas") && config.port == 55421, "port with letters refused");

    std::string long_name(100, 'x');
    std::string line = "name = " + long_name + "\n";
    write_text(&path, line.c_str());
    check(piccolino_config_load(&config, dir.c_str(), "nas") && config.name.size() == NET_DEVICE_NAME_MAX,
          "long name cut");
}

static void check_kinds()
{
    NetShareKind kind = NetShareKind::folder;
    check(piccolino_kind_parse("screenshots", &kind) && kind == NetShareKind::screenshots, "kind parse");
    check(!piccolino_kind_parse("folder", &kind), "folder isn't a folder kind");
    check(std::strcmp(piccolino_kind_name(NetShareKind::states), "states") == 0, "kind name");
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("FAIL no scratch folder given\n");
        return 1;
    }
    check_defaults(argv[1]);
    check_edits(argv[1]);
    check_bad_values(argv[1]);
    check_kinds();
    if (g_failures == 0)
    {
        std::printf("piccolino config: all checks passed\n");
        return 0;
    }
    std::printf("piccolino config: %d check(s) failed\n", g_failures);
    return 1;
}
