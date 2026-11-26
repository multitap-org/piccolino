// piccolino: the self-hosted Multitap file server (headless).
//
//   piccolino [--data DIR] [--log-level debug|info|warning|error]
//   piccolino --version
//
// Everything else is in <data>/piccolino.cfg (piccolino_config.h). The data
// folder holds piccolino.cfg, net_identity.cfg (this server's key, 0600),
// net_devices.cfg (the paired devices) and files/ (the default folders).
// Default: $XDG_DATA_HOME/piccolino (~/.local/share/piccolino), or
// %LOCALAPPDATA%\piccolino on Windows; the systemd unit and the Docker image
// (later phases) pass /var/lib/piccolino and /data.
//
// One thread, one loop: the server is polled about every millisecond while
// anything happens, and every 10 ms when idle. SIGINT / SIGTERM stop it; a
// put cut short keeps its part file to resume, and nothing else needs saving
// (every change is saved when it happens).

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "host_fs.h"
#include "host_log.h"
#include "piccolino_config.h"
#include "piccolino_server.h"
#include "piccolino_version.h"

constexpr int    POLL_BUSY_MS = 1;
constexpr int    POLL_IDLE_MS = 10;
constexpr double BUSY_LINGER_SECONDS = 1.0;   // keep polling fast this long after the last event

// The one global: a signal handler gets no userdata, so the stop request
// lives here. Only the handler writes it; only the loop reads it.
static volatile std::sig_atomic_t g_stop_requested = 0;

static void on_signal(int signal_number)
{
    (void)signal_number;
    g_stop_requested = 1;
}

static void print_usage()
{
    std::printf("piccolino %s: the self-hosted Multitap file server\n\n"
                "  piccolino [--data DIR] [--log-level debug|info|warning|error]\n"
                "  piccolino --version\n\n"
                "Settings: <data>/piccolino.cfg (written on first run).\n",
                PICCOLINO_VERSION);
}

static bool parse_level(const char* text, LogLevel* level)
{
    static const char* NAMES[] = {"debug", "info", "warning", "error"};
    static const LogLevel LEVELS[] = {LogLevel::debug, LogLevel::info, LogLevel::warning, LogLevel::error};
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
    {
        if (std::strcmp(text, NAMES[i]) == 0)
        {
            *level = LEVELS[i];
            return true;
        }
    }
    return false;
}

static std::string environment(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

static std::string default_data_dir()
{
#if defined(_WIN32)
    std::string base = environment("LOCALAPPDATA");
    if (base.empty())
    {
        return std::string();
    }
#else
    std::string base = environment("XDG_DATA_HOME");
    if (base.empty())
    {
        std::string home = environment("HOME");
        if (home.empty())
        {
            return std::string();
        }
        base = home + "/.local/share";
    }
#endif
    std::string path = base + "/piccolino";
    host_fs_forward_slashes(path.data());
    return path;
}

static std::string host_name()
{
#if defined(_WIN32)
    return environment("COMPUTERNAME");
#else
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0)
    {
        return std::string();
    }
    return std::string(name);
#endif
}

int main(int argc, char** argv)
{
    std::string data_dir;
    LogLevel level = LogLevel::info;
    for (int i = 1; i < argc; i++)
    {
        bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--data") == 0 && has_value)
        {
            data_dir = argv[++i];
        }
        else if (std::strcmp(argv[i], "--log-level") == 0 && has_value)
        {
            if (!parse_level(argv[++i], &level))
            {
                std::fprintf(stderr, "--log-level: debug, info, warning or error\n");
                return 2;
            }
        }
        else if (std::strcmp(argv[i], "--version") == 0)
        {
            std::printf("piccolino %s\n", PICCOLINO_VERSION);
            return 0;
        }
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            print_usage();
            return 0;
        }
        else
        {
            std::fprintf(stderr, "piccolino: unknown option %s (try --help)\n", argv[i]);
            return 2;
        }
    }
    host_log_set_min_level(level);
    if (data_dir.empty())
    {
        data_dir = default_data_dir();
        if (data_dir.empty())
        {
            host_log(LogLevel::error, "No data folder: pass --data DIR");
            return 1;
        }
    }
    host_fs_forward_slashes(data_dir.data());
    while (data_dir.size() > 1 && data_dir.back() == '/')
    {
        data_dir.pop_back();
    }
    if (!host_fs_make_directories(data_dir.c_str()))
    {
        return 1;
    }

    PiccolinoConfig config;
    if (!piccolino_config_load(&config, data_dir.c_str(), host_name().c_str()))
    {
        return 1;
    }
    // Big (NetAccess holds every peer's buffers): on the heap.
    PiccolinoServer* server = new PiccolinoServer();
    int result = 0;
    if (!piccolino_server_create(server, &config, data_dir.c_str()))
    {
        result = 1;
    }
    else
    {
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        std::printf("piccolino %s: \"%s\" listening on UDP port %u, data in %s\n", PICCOLINO_VERSION,
                    config.name.c_str(), static_cast<unsigned>(piccolino_server_port(server)), data_dir.c_str());
        std::fflush(stdout);
        std::chrono::steady_clock::time_point last_busy = std::chrono::steady_clock::now();
        while (g_stop_requested == 0)
        {
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            if (piccolino_server_poll(server))
            {
                last_busy = now;
            }
            std::chrono::duration<double> idle = now - last_busy;
            int wait_ms = idle.count() < BUSY_LINGER_SECONDS ? POLL_BUSY_MS : POLL_IDLE_MS;
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        }
        host_log(LogLevel::info, "Stopping");
    }
    piccolino_server_destroy(server);
    delete server;
    return result;
}
