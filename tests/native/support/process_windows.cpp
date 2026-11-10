// Each case owns its process and capture files; descendants share its deadline.
#include "lh_internal.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
namespace lh::internal {
std::string to_windows_path(const std::string &path) { return path; }
namespace {
std::string quote(const std::string &value) {
    std::string out = "\"";
    unsigned slashes = 0;
    for (char c : value) {
        if (c == '\\') { ++slashes; continue; }
        out.append(slashes * (c == '"' ? 2 : 1), '\\');
        slashes = 0;
        if (c == '"') out += '\\';
        out += c;
    }
    out.append(slashes * 2, '\\');
    return out + '"';
}
}
Result run_process(const Command &command) {
    Result result;
    result.timeout = command.timeout();
    result.command = quote(command.program());
    for (const auto &arg : command.arguments()) result.command += " " + quote(arg.text());
    const auto directory = mkdtemp_under(fs::temp_directory_path(), "lh-process-");
    write_file(directory / "in", command.has_input() ? command.input_bytes() : std::string());
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    auto file = [&](const fs::path &path, DWORD access) {
        HANDLE h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) LH_FAIL("cannot open process capture file");
        return h;
    };
    HANDLE in = file(command.input_file().empty() ? directory / "in" : command.input_file(), GENERIC_READ);
    HANDLE out = file(directory / "out", GENERIC_WRITE);
    HANDLE err = file(directory / "err", GENERIC_WRITE);
    std::map<std::string, std::string> environment;
    char *block = GetEnvironmentStringsA();
    for (char *at = block; *at; at += std::strlen(at) + 1) {
        std::string item(at);
        auto split = item.find('=', 1);
        if (split != std::string::npos) environment[item.substr(0, split)] = item.substr(split + 1);
    }
    FreeEnvironmentStringsA(block);
    for (const auto &name : command.unset_names()) environment.erase(name);
    for (const auto &entry : command.environment()) environment[entry.first] = entry.second.text();
    std::string env;
    for (const auto &entry : environment) env += entry.first + "=" + entry.second + '\0';
    env += '\0';
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = in; startup.hStdOutput = out; startup.hStdError = err;
    PROCESS_INFORMATION process{};
    const auto cwd = command.working_directory().string();
    std::string line = result.command;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    BOOL started = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED,
                                  env.data(), cwd.empty() ? nullptr : cwd.c_str(), &startup, &process);
    CloseHandle(in); CloseHandle(out); CloseHandle(err);
    if (!started) { CloseHandle(job); fs::remove_all(directory); LH_FAIL("cannot start " + result.command); }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(job);
        LH_FAIL("cannot assign child process deadline");
    }
    ResumeThread(process.hThread);
    result.timed_out = WaitForSingleObject(process.hProcess, DWORD(command.timeout() * 1000)) == WAIT_TIMEOUT;
    if (result.timed_out) { TerminateJobObject(job, 1); WaitForSingleObject(process.hProcess, INFINITE); }
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    result.rc = static_cast<int>(code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(job);
    result.out = read_file(directory / "out"); result.err = read_file(directory / "err");
    fs::remove_all(directory);
    return result;
}
}
