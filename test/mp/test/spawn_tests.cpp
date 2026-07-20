// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <mp/util.h>

#include <kj/test.h>

#include <atomic>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
//! State for the fork() interposition below, used by the FD_CLOEXEC leak test.
std::atomic<bool> g_simulate_concurrent_spawn{false};
std::atomic<pid_t> g_concurrent_child_pid{-1};
int g_concurrent_stdin_pipe[2]{-1, -1};
} // namespace

//! Interpose libc fork() for this test binary (this strong definition wins
//! over libc when mputil's undefined reference is resolved at link time).
//! A passthrough unless armed. This makes the FD_CLOEXEC leak deterministic:
//! instead of hoping another thread's fork+exec lands inside the vulnerable
//! window, we run one exactly at SpawnProcess's own fork() call, which lies
//! inside that window, snapshotting the caller's file descriptor table at the
//! most adversarial instant.
extern "C" pid_t fork()
{
    using ForkFn = pid_t (*)();
    static const ForkFn real_fork{reinterpret_cast<ForkFn>(dlsym(RTLD_NEXT, "fork"))};
    if (g_simulate_concurrent_spawn.exchange(false)) {
        const pid_t pid{real_fork()};
        if (pid == 0) {
            // Exec a process that lives until its stdin closes, holding any
            // descriptors it inherited across exec. Do not close anything
            // else: an unrelated spawn knows nothing about the caller's fds,
            // so only close-on-exec flags decide what leaks through exec.
            dup2(g_concurrent_stdin_pipe[0], STDIN_FILENO);
            char arg0[]{"cat"};
            char* argv[]{arg0, nullptr};
            execv("/bin/cat", argv);
            _exit(127);
        }
        g_concurrent_child_pid = pid;
    }
    return real_fork();
}

namespace mp {
namespace test {
namespace {

constexpr auto FAILURE_TIMEOUT = std::chrono::seconds{30};

// Poll for child process exit using waitpid(..., WNOHANG) until the child exits
// or timeout expires. Returns true if the child exited and status_out was set.
// Returns false on timeout or error.
static bool WaitPidWithTimeout(ProcessId pid, std::chrono::milliseconds timeout, int& status_out)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const int r = ::waitpid(pid, &status_out, WNOHANG);
        if (r == pid) return true;
        if (r == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            continue;
        }
        // waitpid error
        return false;
    }
    return false;
}

} // namespace

KJ_TEST("SpawnProcess does not run callback in child")
{
    // This test is designed to fail deterministically if fd_to_args is invoked
    // in the post-fork child: a mutex held by another parent thread at fork
    // time appears locked forever in the child.
    std::mutex target_mutex;
    std::mutex control_mutex;
    std::condition_variable control_cv;
    bool locked{false};
    bool release{false};

    // Holds target_mutex until the releaser thread updates release
    std::thread locker([&] {
        std::unique_lock<std::mutex> target_lock(target_mutex);
        {
            std::lock_guard<std::mutex> g(control_mutex);
            locked = true;
        }
        control_cv.notify_one();

        std::unique_lock<std::mutex> control_lock(control_mutex);
        control_cv.wait(control_lock, [&] { return release; });
    });

    // Wait for target_mutex to be held by the locker thread.
    {
        std::unique_lock<std::mutex> l(control_mutex);
        control_cv.wait(l, [&] { return locked; });
    }

    // Release the lock shortly after SpawnProcess starts.
    std::thread releaser([&] {
        // In the unlikely event a CI machine overshoots this delay, a
        // regression could be missed. This is preferable to spurious
        // test failures.
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        {
            std::lock_guard<std::mutex> g(control_mutex);
            release = true;
        }
        control_cv.notify_one();
    });

    const auto [pid, socket]{SpawnProcess([&](SpawnConnectInfo connect_info) -> std::vector<std::string> {
        // If this callback runs in the post-fork child, target_mutex appears
        // locked forever (the owning thread does not exist), so this deadlocks.
        std::lock_guard<std::mutex> g(target_mutex);
        return {"true", std::move(connect_info)};
    })};
    ::close(socket);

    int status{0};
    // Give the child some time to exit. If it does not, terminate it and
    // reap it to avoid leaving a zombie behind.
    const bool exited{WaitPidWithTimeout(pid, FAILURE_TIMEOUT, status)};
    if (!exited) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, /*options=*/0);
    }

    releaser.join();
    locker.join();

    KJ_EXPECT(exited, "Timeout waiting for child process to exit");
    KJ_EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

KJ_TEST("SpawnProcess socket does not leak into a concurrent fork+exec")
{
    // Reproduces the close-on-exec race: if SpawnProcess ever leaves the
    // child's socket descriptor without FD_CLOEXEC in the parent, a
    // fork+exec performed concurrently by an unrelated thread inherits a
    // duplicate that survives its exec. The duplicate then keeps the socket
    // open after the spawned process exits, so the parent never sees EOF.
    // The fork() interposition above simulates that concurrent fork+exec at
    // the worst possible instant, making the failure deterministic.

    // Pipe keeping the simulated process alive; close-on-exec so the ends we
    // hold are not themselves inherited (the child dup2s the read end onto
    // its stdin, which clears the flag on the copy).
    KJ_SYSCALL(pipe(g_concurrent_stdin_pipe));
    KJ_SYSCALL(fcntl(g_concurrent_stdin_pipe[0], F_SETFD, FD_CLOEXEC));
    KJ_SYSCALL(fcntl(g_concurrent_stdin_pipe[1], F_SETFD, FD_CLOEXEC));

    g_simulate_concurrent_spawn = true;
    const auto [pid, socket]{SpawnProcess([&](SpawnConnectInfo connect_info) -> std::vector<std::string> {
        return {"true", std::move(connect_info)};
    })};
    KJ_EXPECT(!g_simulate_concurrent_spawn, "fork() interposition did not engage");

    // Reap the spawned process.
    int status{0};
    const bool exited{WaitPidWithTimeout(pid, FAILURE_TIMEOUT, status)};
    if (!exited) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, /*options=*/0);
    }
    KJ_EXPECT(exited, "Timeout waiting for child process to exit");
    KJ_EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    // With the spawned process gone, all descriptors for its end of the
    // socket must be closed, so reading the parent's end must give EOF. If a
    // duplicate leaked into the simulated concurrent process, the socket
    // stays open and poll() times out.
    struct pollfd pfd{socket, POLLIN, 0};
    int poll_result{-1};
    KJ_SYSCALL(poll_result = poll(&pfd, 1, /*timeout=*/5000));
    KJ_EXPECT(poll_result == 1, "No EOF on socket: child descriptor leaked into concurrent process");
    if (poll_result == 1) {
        char buf;
        ssize_t nread{-1};
        KJ_SYSCALL(nread = read(socket, &buf, 1));
        KJ_EXPECT(nread == 0, "Expected EOF on socket");
    }
    ::close(socket);

    // Cleanup: closing the pipe ends the simulated process (a leaked socket
    // duplicate dies with it).
    ::close(g_concurrent_stdin_pipe[1]);
    ::close(g_concurrent_stdin_pipe[0]);
    const pid_t concurrent_pid{g_concurrent_child_pid.exchange(-1)};
    KJ_EXPECT(concurrent_pid > 0);
    if (concurrent_pid > 0) {
        int concurrent_status{0};
        if (!WaitPidWithTimeout(concurrent_pid, FAILURE_TIMEOUT, concurrent_status)) {
            ::kill(concurrent_pid, SIGKILL);
            ::waitpid(concurrent_pid, &concurrent_status, /*options=*/0);
        }
    }
}
} // namespace test
} // namespace mp
