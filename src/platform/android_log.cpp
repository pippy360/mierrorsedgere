#include "android_log.hpp"

#if defined(__ANDROID__)

#include <android/log.h>
#include <pthread.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace me {

namespace {

constexpr const char* kTag = "mirrorsedge";

struct PipeReader {
    int fd = -1;
    int priority = ANDROID_LOG_INFO;
};

void* reader_thread(void* arg) {
    PipeReader* reader = static_cast<PipeReader*>(arg);
    std::string pending;
    char buf[1024];
    for (;;) {
        const ssize_t n = read(reader->fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        pending.append(buf, static_cast<size_t>(n));
        size_t start = 0;
        for (;;) {
            const size_t nl = pending.find('\n', start);
            if (nl == std::string::npos) break;
            std::string line = pending.substr(start, nl - start);
            if (!line.empty()) __android_log_write(reader->priority, kTag, line.c_str());
            start = nl + 1;
        }
        pending.erase(0, start);
        // A very long line without a newline: flush it rather than grow without bound.
        if (pending.size() > 8192) {
            __android_log_write(reader->priority, kTag, pending.c_str());
            pending.clear();
        }
    }
    if (!pending.empty()) __android_log_write(reader->priority, kTag, pending.c_str());
    close(reader->fd);
    delete reader;
    return nullptr;
}

bool redirect_fd(int target_fd, int priority) {
    int fds[2];
    if (pipe(fds) != 0) return false;
    if (dup2(fds[1], target_fd) < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    close(fds[1]);
    PipeReader* reader = new PipeReader{fds[0], priority};
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, reader_thread, reader) != 0) {
        pthread_attr_destroy(&attr);
        close(fds[0]);
        delete reader;
        return false;
    }
    pthread_attr_destroy(&attr);
    return true;
}

}  // namespace

void install_android_log_redirect() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    // Line buffered: a std::cout line reaches logcat as soon as it ends, not when the buffer fills.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    redirect_fd(STDOUT_FILENO, ANDROID_LOG_INFO);
    redirect_fd(STDERR_FILENO, ANDROID_LOG_WARN);
    __android_log_write(ANDROID_LOG_INFO, kTag, "stdout/stderr -> logcat");
}

}  // namespace me

#else

namespace me {
void install_android_log_redirect() {}
}  // namespace me

#endif
