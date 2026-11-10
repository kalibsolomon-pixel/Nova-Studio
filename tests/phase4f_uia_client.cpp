// SPDX-License-Identifier: GPL-3.0-or-later
// Phase 4-F UI Automation client. Queries another process's window the way screen
// readers and automation tools do, with its own deadline, and reports timing per call.
//
//   nova-phase4f-uia-client <hwnd-decimal> <walk|findall|focus> <budget-ms> <output.json>
//
// walk     depth-first RawView traversal with a cache request per element
// findall  one FindAll(TreeScope_Descendants) from the window, the query pattern that
//          left a Phase 4-D.2 test window unresponsive
// focus    reads the focused element and its ancestors repeatedly (screen-reader polling)
// invoke   expands or invokes controls by accessible name, in order: the fourth argument is
//          a '|' separated list of names; each is searched under the window, then under the
//          desktop's top-level windows of the same process (menus and dialogs)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <UIAutomation.h>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
std::string utf8(BSTR s) {
    if (!s)
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(std::max(n - 1, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}
std::string escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (unsigned(c) < 0x20)
            continue;
        out += c;
    }
    return out.substr(0, 120);
}
struct Slow {
    double ms;
    std::string name;
    int type;
    int depth;
};
struct Report {
    int elements = 0, failures = 0, maxDepth = 0;
    bool deadline = false;
    std::vector<double> calls;
    std::vector<Slow> slowest;
    void call(double t, IUIAutomationElement *e, int depth) {
        calls.push_back(t);
        if (slowest.size() < 12 || t > slowest.back().ms) {
            Slow s{t, {}, 0, depth};
            if (e) {
                BSTR name = nullptr;
                if (SUCCEEDED(e->get_CachedName(&name)) && name) {
                    s.name = utf8(name);
                    SysFreeString(name);
                }
                CONTROLTYPEID type = 0;
                if (SUCCEEDED(e->get_CachedControlType(&type)))
                    s.type = type;
            }
            slowest.push_back(s);
            std::sort(slowest.begin(), slowest.end(), [](auto &a, auto &b) { return a.ms > b.ms; });
            if (slowest.size() > 12)
                slowest.pop_back();
        }
    }
};
} // namespace

int main(int argc, char **argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: nova-phase4f-uia-client <hwnd> <walk|findall|focus> <budget-ms> <out.json>\n");
        return 2;
    }
    const HWND hwnd = reinterpret_cast<HWND>(static_cast<intptr_t>(std::strtoll(argv[1], nullptr, 10)));
    const std::string mode = argv[2];
    const double budget = std::atof(argv[3]);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation *uia = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                reinterpret_cast<void **>(&uia)))) {
        std::fprintf(stderr, "UI Automation unavailable\n");
        return 3;
    }
    // Bound every cross-process call so a busy target cannot stall this client forever.
    if (IUIAutomation2 *uia2 = nullptr; SUCCEEDED(uia->QueryInterface(__uuidof(IUIAutomation2), reinterpret_cast<void **>(&uia2)))) {
        uia2->put_ConnectionTimeout(5000);
        uia2->put_TransactionTimeout(10000);
        uia2->Release();
    }
    IUIAutomationCacheRequest *cache = nullptr;
    uia->CreateCacheRequest(&cache);
    cache->AddProperty(UIA_NamePropertyId);
    cache->AddProperty(UIA_ControlTypePropertyId);
    cache->AddProperty(UIA_BoundingRectanglePropertyId);
    cache->AddProperty(UIA_IsKeyboardFocusablePropertyId);
    cache->AddProperty(UIA_HasKeyboardFocusPropertyId);
    // "deep" asks for everything an inspector tool shows: every standard property and
    // pattern-availability flag, and the full text of text controls.
    const bool deep = mode == "deep";
    int deepProperties = 0;
    if (deep)
        for (PROPERTYID id = 30000; id <= 30180; ++id)
            deepProperties += SUCCEEDED(cache->AddProperty(id)) ? 1 : 0;
    Report report;
    const auto start = Clock::now();
    auto expired = [&] { return ms(start, Clock::now()) > budget; };
    IUIAutomationElement *root = nullptr;
    auto t0 = Clock::now();
    HRESULT hr = uia->ElementFromHandleBuildCache(hwnd, cache, &root);
    report.call(ms(t0, Clock::now()), root, 0);
    if (FAILED(hr) || !root) {
        std::fprintf(stderr, "ElementFromHandle failed 0x%08lx\n", hr);
        return 4;
    }
    if (mode == "walk" || mode == "deep") {
        IUIAutomationTreeWalker *walker = nullptr;
        uia->get_RawViewWalker(&walker);
        struct Item {
            IUIAutomationElement *e;
            int depth;
        };
        std::vector<Item> stack{{root, 0}};
        root->AddRef();
        while (!stack.empty()) {
            if (expired()) {
                report.deadline = true;
                break;
            }
            auto [e, depth] = stack.back();
            stack.pop_back();
            ++report.elements;
            if (const char *dump = std::getenv("NOVA_UIA_DUMP")) {
                static FILE *list = std::fopen(dump, "wb");
                BSTR name = nullptr;
                CONTROLTYPEID type = 0;
                e->get_CachedName(&name);
                e->get_CachedControlType(&type);
                std::fprintf(list, "%d %d %s\n", depth, type, utf8(name).c_str());
                SysFreeString(name);
            }
            report.maxDepth = std::max(report.maxDepth, depth);
            if (deep) {
                IUIAutomationTextPattern *text = nullptr;
                t0 = Clock::now();
                if (SUCCEEDED(e->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
                                                     reinterpret_cast<void **>(&text))) && text) {
                    IUIAutomationTextRange *range = nullptr;
                    if (SUCCEEDED(text->get_DocumentRange(&range)) && range) {
                        BSTR content = nullptr;
                        range->GetText(-1, &content);
                        SysFreeString(content);
                        SAFEARRAY *rects = nullptr;
                        range->GetBoundingRectangles(&rects);
                        if (rects)
                            SafeArrayDestroy(rects);
                        range->Release();
                    }
                    text->Release();
                }
                report.call(ms(t0, Clock::now()), e, depth);
            }
            IUIAutomationElement *child = nullptr;
            t0 = Clock::now();
            hr = walker->GetFirstChildElementBuildCache(e, cache, &child);
            report.call(ms(t0, Clock::now()), child, depth + 1);
            if (FAILED(hr))
                ++report.failures;
            std::vector<IUIAutomationElement *> children;
            while (child && children.size() < 20000) {
                children.push_back(child);
                IUIAutomationElement *next = nullptr;
                t0 = Clock::now();
                hr = walker->GetNextSiblingElementBuildCache(child, cache, &next);
                report.call(ms(t0, Clock::now()), next, depth + 1);
                if (FAILED(hr))
                    ++report.failures;
                child = next;
                if (expired())
                    break;
            }
            for (auto it = children.rbegin(); it != children.rend(); ++it)
                stack.push_back({*it, depth + 1});
            e->Release();
        }
        for (auto &item : stack)
            item.e->Release();
        walker->Release();
    } else if (mode == "findall") {
        IUIAutomationCondition *all = nullptr;
        uia->CreateTrueCondition(&all);
        IUIAutomationElementArray *found = nullptr;
        t0 = Clock::now();
        hr = root->FindAllBuildCache(TreeScope_Descendants, all, cache, &found);
        report.call(ms(t0, Clock::now()), nullptr, 0);
        if (FAILED(hr))
            ++report.failures;
        int length = 0;
        if (found) {
            found->get_Length(&length);
            found->Release();
        }
        report.elements = length;
        all->Release();
    } else if (mode == "invoke") {
        // argv[4] holds the names; the report goes to argv[5].
        // argv is in the ANSI code page; names such as "MCP Access…" need the wide command line.
        std::wstring names;
        {
            int count = 0;
            LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &count);
            if (wide && count > 4)
                names = wide[4];
            LocalFree(wide);
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        IUIAutomationElement *desktop = nullptr;
        uia->GetRootElement(&desktop);
        size_t start = 0;
        while (start <= names.size() && !expired()) {
            const auto end = names.find(L'|', start);
            auto name = names.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            // A trailing '*' matches by prefix (tab titles carry status marks such as " ●").
            const bool prefix = !name.empty() && name.back() == L'*';
            if (prefix)
                name.pop_back();
            start = end == std::wstring::npos ? names.size() + 1 : end + 1;
            VARIANT value;
            value.vt = VT_BSTR;
            value.bstrVal = SysAllocString(name.c_str());
            IUIAutomationCondition *byName = nullptr, *byProcess = nullptr, *both = nullptr;
            if (prefix)
                uia->CreateTrueCondition(&byName);
            else
                uia->CreatePropertyCondition(UIA_NamePropertyId, value, &byName);
            VARIANT process;
            process.vt = VT_I4;
            process.lVal = LONG(pid);
            uia->CreatePropertyCondition(UIA_ProcessIdPropertyId, process, &byProcess);
            uia->CreateAndCondition(byName, byProcess, &both);
            IUIAutomationElement *found = nullptr;
            for (int attempt = 0; attempt < 40 && !found && !expired(); ++attempt) {
                t0 = Clock::now();
                if (!prefix) {
                    desktop->FindFirst(TreeScope_Descendants, both, &found);
                } else {
                    IUIAutomationElementArray *all = nullptr;
                    desktop->FindAll(TreeScope_Descendants, both, &all);
                    int length = 0;
                    if (all)
                        all->get_Length(&length);
                    // Prefer a tab (document titles) over list or tree rows with the same name.
                    IUIAutomationElement *fallback = nullptr;
                    for (int i = 0; i < length && !found; ++i) {
                        IUIAutomationElement *e = nullptr;
                        all->GetElement(i, &e);
                        BSTR current = nullptr;
                        CONTROLTYPEID type = 0;
                        if (e && SUCCEEDED(e->get_CurrentName(&current)) && current &&
                            std::wstring(current).rfind(name, 0) == 0) {
                            e->get_CurrentControlType(&type);
                            if (type == UIA_TabItemControlTypeId)
                                found = e;
                            else if (!fallback)
                                fallback = e;
                            else
                                e->Release();
                        } else if (e) {
                            e->Release();
                        }
                        SysFreeString(current);
                    }
                    if (!found)
                        found = std::exchange(fallback, nullptr);
                    if (fallback)
                        fallback->Release();
                    if (all)
                        all->Release();
                }
                report.call(ms(t0, Clock::now()), nullptr, 0);
                if (!found)
                    Sleep(100);
            }
            SysFreeString(value.bstrVal);
            byName->Release();
            byProcess->Release();
            both->Release();
            if (!found) {
                ++report.failures;
                break;
            }
            ++report.elements;
            IUIAutomationExpandCollapsePattern *expand = nullptr;
            IUIAutomationInvokePattern *invoke = nullptr;
            IUIAutomationSelectionItemPattern *select = nullptr;
            // Menus open with Expand; invoking a menu-bar item does not open its menu.
            ExpandCollapseState state = ExpandCollapseState_LeafNode;
            if (SUCCEEDED(found->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,
                                                     __uuidof(IUIAutomationExpandCollapsePattern),
                                                     reinterpret_cast<void **>(&expand))) && expand &&
                SUCCEEDED(expand->get_CurrentExpandCollapseState(&state)) && state != ExpandCollapseState_LeafNode) {
                expand->Expand();
                expand->Release();
            } else if (SUCCEEDED(found->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern),
                                                            reinterpret_cast<void **>(&invoke))) && invoke) {
                if (expand)
                    expand->Release();
                invoke->Invoke();
                invoke->Release();
            } else if (SUCCEEDED(found->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                                                            __uuidof(IUIAutomationSelectionItemPattern),
                                                            reinterpret_cast<void **>(&select))) && select) {
                select->Select();
                select->Release();
            } else {
                ++report.failures;
            }
            found->Release();
            Sleep(300);
        }
        desktop->Release();
    } else if (mode == "focus") {
        IUIAutomationTreeWalker *walker = nullptr;
        uia->get_ControlViewWalker(&walker);
        while (!expired()) {
            IUIAutomationElement *focused = nullptr;
            t0 = Clock::now();
            hr = uia->GetFocusedElementBuildCache(cache, &focused);
            report.call(ms(t0, Clock::now()), focused, 0);
            if (FAILED(hr) || !focused) {
                ++report.failures;
                Sleep(50);
                continue;
            }
            ++report.elements;
            // A screen reader announces the focused item with its ancestry.
            int depth = 0;
            for (IUIAutomationElement *e = focused; e && depth < 32; ++depth) {
                IUIAutomationElement *parent = nullptr;
                t0 = Clock::now();
                walker->GetParentElementBuildCache(e, cache, &parent);
                report.call(ms(t0, Clock::now()), parent, depth);
                e->Release();
                e = parent;
            }
            Sleep(50);
        }
        walker->Release();
    }
    const double total = ms(start, Clock::now());
    auto sorted = report.calls;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) { return sorted.empty() ? 0.0 : sorted[std::min(sorted.size() - 1, size_t(p * sorted.size()))]; };
    FILE *out = std::fopen(mode == "invoke" && argc > 5 ? argv[5] : argv[4], "wb");
    if (!out)
        return 5;
    std::fprintf(out,
                 "{\"deep_properties\":%d,\"mode\":\"%s\",\"elements\":%d,\"calls\":%zu,\"failures\":%d,\"max_depth\":%d,"
                 "\"deadline_reached\":%s,\"total_ms\":%.1f,\"call_median_ms\":%.3f,\"call_p95_ms\":%.3f,"
                 "\"call_max_ms\":%.1f,\"slowest\":[",
                 deepProperties, mode.c_str(), report.elements, report.calls.size(), report.failures, report.maxDepth,
                 report.deadline ? "true" : "false", total, pct(0.5), pct(0.95), sorted.empty() ? 0 : sorted.back());
    for (size_t i = 0; i < report.slowest.size(); ++i)
        std::fprintf(out, "%s{\"ms\":%.1f,\"name\":\"%s\",\"control_type\":%d,\"depth\":%d}", i ? "," : "",
                     report.slowest[i].ms, escape(report.slowest[i].name).c_str(), report.slowest[i].type,
                     report.slowest[i].depth);
    std::fprintf(out, "]}\n");
    std::fclose(out);
    root->Release();
    cache->Release();
    uia->Release();
    CoUninitialize();
    return 0;
}
