/*
 * android_main.cc
 *
 * Verdandi on android: SDL's activity calls SDL_main on its SDL thread. this sets up the
 * norns home, starts the sidecar (forked, as norns/main.cpp does), the SuperCollider
 * stand-in and matron, then runs the window (sdl_bridge_android.cc) on the SDL thread.
 *
 * the norns home is the app's external files folder (Android/data/<package>/files), reachable
 * from a computer over USB, so scripts can be copied into dust/code by hand:
 *   <home>/norns/   lua, sc, resources: unpacked from the apk on every new version
 *   <home>/dust/    scripts, audio, data: the scripts in the apk are only added if missing
 */

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include <SDL.h>

#include "matron_main.h"
#include "platform/android/fake_sclang.h"
#include "platform/android/sdl_bridge_android.h"
#include "sidecar.h"

#define LOG_TAG "Verdandi"

namespace {

std::string home;
FILE *log_file = NULL;

//--- stdout/stderr -> logcat and <home>/norns.log

int log_pipe[2];

void *log_thread(void *p) {
    (void)p;
    char buf[1024];
    std::string line;
    ssize_t n;
    while ((n = read(log_pipe[0], buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                __android_log_write(ANDROID_LOG_INFO, LOG_TAG, line.c_str());
                line.clear();
            } else {
                line += buf[i];
            }
        }
        if (log_file != NULL) {
            fwrite(buf, 1, (size_t)n, log_file);
            fflush(log_file);
        }
    }
    return NULL;
}

void redirect_output(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (pipe(log_pipe) != 0) {
        return;
    }
    dup2(log_pipe[1], STDOUT_FILENO);
    dup2(log_pipe[1], STDERR_FILENO);
    pthread_t t;
    pthread_create(&t, NULL, log_thread, NULL);
    pthread_detach(t);
}

//--- files

bool mkdirs(const std::string &path) {
    std::string p;
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            p = path.substr(0, i);
            if (!p.empty()) {
                mkdir(p.c_str(), 0755);
            }
        }
    }
    struct stat sb;
    return stat(path.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
}

bool exists(const std::string &path) {
    struct stat sb;
    return stat(path.c_str(), &sb) == 0;
}

std::string read_asset(const char *name) {
    std::string out;
    SDL_RWops *rw = SDL_RWFromFile(name, "rb");
    if (rw == NULL) {
        return out;
    }
    char buf[16384];
    size_t n;
    while ((n = SDL_RWread(rw, buf, 1, sizeof buf)) > 0) {
        out.append(buf, n);
    }
    SDL_RWclose(rw);
    return out;
}

bool copy_asset(const std::string &asset, const std::string &dest) {
    SDL_RWops *rw = SDL_RWFromFile(asset.c_str(), "rb");
    if (rw == NULL) {
        fprintf(stderr, "verdandi: missing asset %s\n", asset.c_str());
        return false;
    }
    size_t slash = dest.rfind('/');
    if (slash != std::string::npos) {
        mkdirs(dest.substr(0, slash));
    }
    std::string tmp = dest + ".part";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (f == NULL) {
        SDL_RWclose(rw);
        return false;
    }
    char buf[65536];
    size_t n;
    while ((n = SDL_RWread(rw, buf, 1, sizeof buf)) > 0) {
        fwrite(buf, 1, n, f);
    }
    fclose(f);
    SDL_RWclose(rw);
    return rename(tmp.c_str(), dest.c_str()) == 0;
}

// assets/home/manifest.txt (android/app/build.gradle): first line the build id, then one
// file per line: "R path" is replaced on a new build, "S path" (seed) only added if missing
void unpack_home(void) {
    std::string manifest = read_asset("home/manifest.txt");
    if (manifest.empty()) {
        fprintf(stderr, "verdandi: no asset manifest\n");
        return;
    }
    size_t eol = manifest.find('\n');
    std::string build = manifest.substr(0, eol);
    std::string stamp_path = home + "/norns/.verdandi-build";
    std::string stamp;
    FILE *sf = fopen(stamp_path.c_str(), "r");
    if (sf != NULL) {
        char b[256] = {0};
        if (fgets(b, sizeof b, sf) != NULL) {
            stamp = b;
        }
        fclose(sf);
        while (!stamp.empty() && (stamp.back() == '\n' || stamp.back() == '\r')) {
            stamp.pop_back();
        }
    }
    bool fresh = stamp != build;
    fprintf(stderr, "verdandi: build %s, home %s (%s)\n", build.c_str(), home.c_str(),
            fresh ? "unpacking" : "up to date");
    size_t pos = eol == std::string::npos ? manifest.size() : eol + 1;
    int copied = 0;
    while (pos < manifest.size()) {
        size_t e = manifest.find('\n', pos);
        if (e == std::string::npos) {
            e = manifest.size();
        }
        std::string line = manifest.substr(pos, e - pos);
        pos = e + 1;
        if (line.size() < 3) {
            continue;
        }
        char kind = line[0];
        std::string rel = line.substr(2);
        std::string dest = home + "/" + rel;
        if ((kind == 'R' && fresh) || (kind == 'S' && !exists(dest))) {
            if (copy_asset("home/" + rel, dest)) {
                copied++;
            }
        }
    }
    if (fresh) {
        mkdirs(home + "/norns");
        FILE *f = fopen(stamp_path.c_str(), "w");
        if (f != NULL) {
            fprintf(f, "%s\n", build.c_str());
            fclose(f);
        }
    }
    fprintf(stderr, "verdandi: %d files unpacked\n", copied);
}

void write_if_missing(const std::string &path, const char *text) {
    if (exists(path)) {
        return;
    }
    FILE *f = fopen(path.c_str(), "w");
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

void setup_home(void) {
    const char *ext = SDL_AndroidGetExternalStoragePath();
    const char *internal = SDL_AndroidGetInternalStoragePath();
    if (ext != NULL && mkdirs(ext)) {
        home = ext;
    } else {
        home = internal != NULL ? internal : "/data/local/tmp";
    }
    mkdirs(home + "/dust/code");
    mkdirs(home + "/dust/audio");
    mkdirs(home + "/dust/data");

    std::string log_path = home + "/norns.log";
    log_file = fopen(log_path.c_str(), "w");

    unpack_home();

    // the norns release this is: scripts check it (norns.version.update, lua/core/norns.lua)
    if (exists(home + "/norns/update/version.txt")) {
        std::string v = home + "/version.txt";
        unlink(v.c_str());
        std::string src = home + "/norns/update/version.txt";
        FILE *in = fopen(src.c_str(), "r");
        FILE *out = fopen(v.c_str(), "w");
        if (in != NULL && out != NULL) {
            char b[256];
            size_t n;
            while ((n = fread(b, 1, sizeof b, in)) > 0) {
                fwrite(b, 1, n, out);
            }
        }
        if (in != NULL) {
            fclose(in);
        }
        if (out != NULL) {
            fclose(out);
        }
    }

    write_if_missing(home + "/matronrc.lua", "-- android: the norns window (screen:sdl), touch the keys and knobs\n"
                                             "_boot.add_io('screen:sdl', {})\n");

    // the sidecar's socket and the shell's temporary files
    std::string tmp = std::string(internal != NULL ? internal : home.c_str()) + "/tmp";
    mkdirs(tmp);

    char pid[32];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    setenv("HOME", home.c_str(), 1);
    setenv("TMPDIR", tmp.c_str(), 1);
    setenv("NORNS_PLATFORM", "android", 1);
    setenv("NORNS_APP_PID", pid, 1);
    // no SuperCollider yet: engine commands the .sc parser missed do nothing (lua/core/engine.lua)
    setenv("NORNS_ENGINE_STUB", "1", 1);
    // a hardware keyboard drives K1-3 / E1-3 (lua/core/keyboard.lua)
    setenv("NORNS_HARDWARE_KEYS", "1", 1);
    chdir(home.c_str());
}

//--- sidecar

bool start_sidecar(void) {
    int sync_pipe[2];
    if (pipe(sync_pipe) == -1) {
        perror("pipe");
        return false;
    }
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return false;
    }
    if (child == 0) {
        close(sync_pipe[0]);
        prctl(PR_SET_NAME, (unsigned long)"sidecar");
        // the SDL thread forked us and lives as long as the app
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        int fd = open("/dev/null", O_RDONLY);
        dup2(fd, 0);
        sidecar_server_main(sync_pipe[1]);
        _exit(0);
    }
    close(sync_pipe[1]);
    char b;
    read(sync_pipe[0], &b, 1);
    close(sync_pipe[0]);
    return sidecar_client_init() == 0;
}

//--- matron

void *matron_thread(void *p) {
    (void)p;
    char arg0[] = "norns";
    char *argv[] = {arg0, NULL};
    matron_main(1, argv);
    matron_cleanup();
    sidecar_client_cleanup();
    // matron_cleanup exits; in case it returns, end the app
    exit(0);
    return NULL;
}

} // namespace

extern "C" int SDL_main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    // a closed socket must not kill norns (liblo, nng)
    signal(SIGPIPE, SIG_IGN);

    redirect_output();
    setup_home();
    fprintf(stderr, "verdandi: starting, home %s\n", home.c_str());

    if (!start_sidecar()) {
        fprintf(stderr, "verdandi: sidecar failed; system commands will not work\n");
    }

    if (sdl_bridge_init() != 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Verdandi", SDL_GetError(), NULL);
        return 1;
    }

    fake_sclang_start();

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // lua scripts can recurse deeply
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    pthread_create(&t, &attr, matron_thread, NULL);
    pthread_attr_destroy(&attr);

    sdl_bridge_run();
    // the window is gone: end matron the way the menu's SLEEP does
    exit(0);
    return 0;
}
