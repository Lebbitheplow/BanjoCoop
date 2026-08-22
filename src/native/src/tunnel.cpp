/* cloudflared subprocess driver. See tunnel.hpp. The platform-specific spawning is split by
 * #ifdef; the output parsing and the public surface are shared. */

#include "banjocoop/tunnel.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace bcnet {

namespace {

/* Pull the first https://<host>.trycloudflare.com out of a line of cloudflared output, without the
 * scheme. Empty if the line carries no such URL. Requiring the trycloudflare.com suffix keeps the
 * prose lines in the banner ("Visit it at ...") from being mistaken for the tunnel address. */
std::string extract_host(const std::string& line) {
    const std::string scheme = "https://";
    size_t s = line.find(scheme);
    if (s == std::string::npos) {
        return {};
    }
    size_t start = s + scheme.size();
    size_t end = start;
    while (end < line.size()) {
        char c = line[end];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '|' || c == ')' || c == '"') {
            break;
        }
        end++;
    }
    std::string host = line.substr(start, end - start);
    while (!host.empty() && host.back() == '/') {
        host.pop_back();
    }
    if (host.find(".trycloudflare.com") == std::string::npos) {
        return {};
    }
    return host;
}

/* Prefer a cloudflared shipped next to this library (the release bundles one); otherwise fall back
 * to the name alone and let the OS search PATH. */
std::string cloudflared_path() {
#if defined(_WIN32)
    const char* exe = "cloudflared.exe";
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&extract_host), &mod)) {
        char buf[MAX_PATH];
        DWORD n = GetModuleFileNameA(mod, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::string p(buf, n);
            size_t slash = p.find_last_of("\\/");
            if (slash != std::string::npos) {
                std::string cand = p.substr(0, slash + 1) + exe;
                if (GetFileAttributesA(cand.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    return cand;
                }
            }
        }
    }
    return exe;
#else
    const char* exe = "cloudflared";
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&extract_host), &info) != 0 && info.dli_fname != nullptr) {
        std::string p = info.dli_fname;
        size_t slash = p.find_last_of('/');
        if (slash != std::string::npos) {
            std::string cand = p.substr(0, slash + 1) + exe;
            if (access(cand.c_str(), X_OK) == 0) {
                return cand;
            }
        }
    }
    return exe;
#endif
}

} // namespace

struct TunnelProcess::Impl {
    std::thread reader;
    std::atomic<bool> stop_flag{false};
    mutable std::mutex mu;
    std::string host;

#if defined(_WIN32)
    HANDLE proc = nullptr;
    HANDLE read_pipe = nullptr;
#else
    pid_t pid = -1;
    int read_fd = -1;
#endif

    void set_host(const std::string& h) {
        std::lock_guard<std::mutex> lock(mu);
        if (host.empty()) {
            host = h;
        }
    }

    /* Consume the child's output, line by line, until it closes or we are told to stop. */
    void pump_output(int /*unused on windows*/) {
        std::string buf;
        char chunk[512];
        for (;;) {
            if (stop_flag.load()) {
                break;
            }
#if defined(_WIN32)
            DWORD n = 0;
            if (!ReadFile(read_pipe, chunk, sizeof(chunk), &n, nullptr) || n == 0) {
                break;
            }
#else
            ssize_t n = ::read(read_fd, chunk, sizeof(chunk));
            if (n <= 0) {
                break;
            }
#endif
            buf.append(chunk, static_cast<size_t>(n));
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                std::string h = extract_host(line);
                if (!h.empty()) {
                    set_host(h);
                    std::printf("[banjocoop] tunnel ready: %s\n", h.c_str());
                    std::fflush(stdout);
                }
            }
        }
    }
};

TunnelProcess::TunnelProcess() : impl_(new Impl()) {}

TunnelProcess::~TunnelProcess() {
    stop();
}

std::string TunnelProcess::join_host() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->host;
}

bool TunnelProcess::running() const {
#if defined(_WIN32)
    return impl_->proc != nullptr;
#else
    return impl_->pid > 0;
#endif
}

#if defined(_WIN32)

bool TunnelProcess::start(uint16_t local_port, std::string& error) {
    stop();
    std::string exe = cloudflared_path();
    std::string cmd = "\"" + exe + "\" tunnel --no-autoupdate --url http://localhost:" +
                      std::to_string(local_port);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        error = "CreatePipe failed";
        return false;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<char> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back('\0');
    BOOL ok = CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        error = "could not launch cloudflared (is it next to the mod, or on PATH?)";
        return false;
    }
    CloseHandle(pi.hThread);
    impl_->proc = pi.hProcess;
    impl_->read_pipe = rd;
    impl_->stop_flag.store(false);
    impl_->reader = std::thread([this] { impl_->pump_output(0); });
    return true;
}

void TunnelProcess::stop() {
    impl_->stop_flag.store(true);
    if (impl_->proc != nullptr) {
        TerminateProcess(impl_->proc, 0);
    }
    if (impl_->read_pipe != nullptr) {
        CloseHandle(impl_->read_pipe);
        impl_->read_pipe = nullptr;
    }
    if (impl_->reader.joinable()) {
        impl_->reader.join();
    }
    if (impl_->proc != nullptr) {
        WaitForSingleObject(impl_->proc, 2000);
        CloseHandle(impl_->proc);
        impl_->proc = nullptr;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->host.clear();
}

#else // POSIX

bool TunnelProcess::start(uint16_t local_port, std::string& error) {
    stop();
    std::string exe = cloudflared_path();
    std::string url = "http://localhost:" + std::to_string(local_port);

    int fds[2];
    if (pipe(fds) != 0) {
        error = "pipe() failed";
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        error = "fork() failed";
        return false;
    }
    if (pid == 0) {
        /* Child: send stdout and stderr down the pipe (the URL is on stderr) and detach stdin. */
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        execlp(exe.c_str(), "cloudflared", "tunnel", "--no-autoupdate", "--url", url.c_str(),
               static_cast<char*>(nullptr));
        _exit(127); /* only reached if exec failed */
    }

    close(fds[1]);
    impl_->pid = pid;
    impl_->read_fd = fds[0];
    impl_->stop_flag.store(false);
    impl_->reader = std::thread([this] { impl_->pump_output(0); });
    return true;
}

void TunnelProcess::stop() {
    impl_->stop_flag.store(true);
    if (impl_->pid > 0) {
        /* SIGTERM closes the child's write end, so the reader sees EOF and exits. */
        kill(impl_->pid, SIGTERM);
    }
    if (impl_->reader.joinable()) {
        impl_->reader.join();
    }
    if (impl_->read_fd >= 0) {
        close(impl_->read_fd);
        impl_->read_fd = -1;
    }
    if (impl_->pid > 0) {
        int status = 0;
        waitpid(impl_->pid, &status, 0);
        impl_->pid = -1;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->host.clear();
}

#endif

} // namespace bcnet
