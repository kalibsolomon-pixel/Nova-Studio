// SPDX-License-Identifier: GPL-3.0-or-later
// Test double for "java -classpath gradle-wrapper.jar GradleWrapperMain <tasks>".
// Plays a script chosen by the task: NOVA_FAKE_GRADLE_RUN for runClient,
// otherwise NOVA_FAKE_GRADLE_PREPARE. Each invocation appends a line to
// NOVA_FAKE_GRADLE_COUNTER. Script commands, one per line:
//   print <text> | eprint <text> | printenv <name> | sleep <ms> | spawn | wait | window | exit <code>
//   companion <game dir>: plays Nova's companion mod (protocol 1) on a background thread, for the
//   launch id given as -PnovaLaunch=<id>: announces itself, answers reload requests.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <qt_windows.h>
#include <string>
#include <vector>

static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_CLOSE) {
        std::puts("[fake] [Render thread/INFO] (Minecraft) Stopping!");
        std::fflush(stdout);
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}

static std::string field(const std::string &json, const std::string &key) {
    const auto at = json.find("\"" + key + "\":\"");
    if (at == std::string::npos)
        return {};
    const auto start = at + key.size() + 4;
    return json.substr(start, json.find('"', start) - start);
}
static void writeAtomically(const std::filesystem::path &path, const std::string &text) {
    auto partial = path;
    partial += ".partial";
    std::ofstream(partial, std::ios::binary) << text;
    std::filesystem::rename(partial, path);
}
static void companion(std::filesystem::path folder, std::string launch) {
    std::error_code ignored;
    std::filesystem::create_directories(folder, ignored);
    writeAtomically(folder / "companion.json", "{\"protocol\":1,\"launch\":\"" + launch + "\"}");
    for (;;) {
        for (const auto &entry : std::filesystem::directory_iterator(folder, ignored)) {
            const auto name = entry.path().filename().string();
            if (name.rfind("request-", 0) != 0 || entry.path().extension() != ".json")
                continue;
            std::stringstream text;
            text << std::ifstream(entry.path(), std::ios::binary).rdbuf();
            std::filesystem::remove(entry.path(), ignored);
            const auto request = text.str();
            const auto id = field(request, "id");
            if (field(request, "launch") != launch || id.empty())
                continue; // another launch's request: ignored, as the mod does
            const bool reload = field(request, "type") == "reload";
            if (reload) {
                std::puts("[fake] [Render thread/INFO] (Minecraft) Reloading ResourceManager: vanilla");
                std::fflush(stdout);
            }
            writeAtomically(folder / ("response-" + id + ".json"),
                            "{\"protocol\":1,\"launch\":\"" + launch + "\",\"id\":\"" + id + "\",\"status\":\"" +
                                (reload ? "finished\",\"durationMs\":7}" : "error\",\"error\":\"Unsupported request type\"}"));
        }
        Sleep(50);
    }
}

static std::string env(const char *name) {
    const char *value = std::getenv(name);
    return value ? value : "";
}

int main(int argc, char **argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    for (const auto &arg : args)
        if (arg == "--child") {
            Sleep(INFINITE);
            return 0;
        }
    bool run = false;
    std::string joined;
    for (const auto &arg : args) {
        run = run || arg == "runClient";
        joined += arg + " ";
    }
    if (auto counter = env("NOVA_FAKE_GRADLE_COUNTER"); !counter.empty()) {
        std::ofstream out(counter, std::ios::app);
        out << (run ? "run " : "prepare ") << joined << "\n";
    }
    std::ifstream script(env(run ? "NOVA_FAKE_GRADLE_RUN" : "NOVA_FAKE_GRADLE_PREPARE"));
    std::string line;
    while (std::getline(script, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto space = line.find(' ');
        const auto command = line.substr(0, space);
        const auto rest = space == std::string::npos ? "" : line.substr(space + 1);
        if (command == "companion") {
            std::string launch;
            for (const auto &arg : args)
                if (arg.rfind("-PnovaLaunch=", 0) == 0)
                    launch = arg.substr(13);
            if (!launch.empty())
                std::thread(companion, std::filesystem::path(rest) / "nova-companion", launch).detach();
        } else if (command == "print") {
            std::puts(rest.c_str());
            std::fflush(stdout);
        } else if (command == "eprint") {
            std::fputs((rest + "\n").c_str(), stderr);
            std::fflush(stderr);
        } else if (command == "printenv") {
            std::puts((rest + "=" + env(rest.c_str())).c_str());
            std::fflush(stdout);
        } else if (command == "sleep") {
            Sleep(DWORD(std::stoul(rest)));
        } else if (command == "spawn") {
            wchar_t self[MAX_PATH];
            GetModuleFileNameW(nullptr, self, MAX_PATH);
            std::wstring commandLine = L"\"" + std::wstring(self) + L"\" --child";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION info{};
            if (CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
                CloseHandle(info.hThread);
                CloseHandle(info.hProcess);
            }
        } else if (command == "wait") {
            Sleep(INFINITE);
        } else if (command == "window") {
            WNDCLASSW type{};
            type.lpfnWndProc = windowProc;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = L"NovaFakeMinecraft";
            RegisterClassW(&type);
            // Visible but placed off-screen so tests do not disturb the desktop.
            HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, type.lpszClassName, L"Fake Minecraft",
                                          WS_POPUP | WS_VISIBLE, -32000, -32000, 64, 64, nullptr,
                                          nullptr, type.hInstance, nullptr);
            if (!window)
                return 3;
            MSG message;
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            return 0;
        } else if (command == "exit") {
            return std::atoi(rest.c_str());
        }
    }
    return 0;
}
