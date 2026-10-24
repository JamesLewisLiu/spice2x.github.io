// Exercise the production implementation without the launcher or a game process.
#include "../../games/gitadora/xg_codec.cpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

std::filesystem::path MODULE_PATH;
namespace cfg { bool CONFIGURATOR_STANDALONE = false; }
namespace avs::game {
    bool is_model(const std::initializer_list<const char *>) { return true; }
}
namespace logger {
    void push(std::string text, Style, bool) { std::cout << text; }
}
std::string_view log_get_datetime(std::time_t) { return "test"; }
namespace libutils {
    std::filesystem::path module_file_name(HMODULE module) {
        wchar_t path[32768]{};
        GetModuleFileNameW(module, path, std::size(path));
        return path;
    }
}
namespace detour {
    // Minimal real PE import patching; registry APIs inside the AX must actually
    // reach production wrappers during the COM integration test.
    void **iat_find(const char *function, HMODULE module, const char *dll) {
        if (!module) {
            return nullptr;
        }
        auto base = reinterpret_cast<BYTE *>(module);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        auto rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        if (!rva) {
            return nullptr;
        }
        auto descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + rva);
        for (; descriptor->Name; ++descriptor) {
            if (dll && _stricmp(reinterpret_cast<char *>(base + descriptor->Name), dll)) {
                continue;
            }
            if (!descriptor->OriginalFirstThunk) {
                continue;
            }
            auto names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->OriginalFirstThunk);
            auto slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++slots) {
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                    continue;
                }
                auto entry = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
                if (!strcmp(reinterpret_cast<char *>(entry->Name), function)) {
                    return reinterpret_cast<void **>(&slots->u1.Function);
                }
            }
        }
        return nullptr;
    }
    void *iat_try(const char *function, void *replacement, HMODULE module, const char *dll) {
        auto slot = iat_find(function, module, dll);
        if (!slot) {
            return nullptr;
        }
        DWORD protection;
        if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) {
            return nullptr;
        }
        auto original = *slot;
        *slot = replacement;
        VirtualProtect(slot, sizeof(*slot), protection, &protection);
        return original;
    }
    void *iat(const char *function, void *replacement, HMODULE module) {
        auto original = iat_try(function, replacement, module, nullptr);
        if (!original) {
            throw std::runtime_error("missing required IAT entry");
        }
        return original;
    }
}

namespace {
    void require(bool condition, const char *message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }
    using namespace games::gitadora::xg_codec;
    unsigned fallback_calls = 0;
    HRESULT WINAPI fallback_sentinel(REFCLSID, IUnknown *, DWORD, REFIID, void **object) {
        ++fallback_calls;
        if (object) {
            *object = nullptr;
        }
        return E_NOINTERFACE;
    }

    void verify_creation_hook(GetClassObject actual_factory) {
        original_create = fallback_sentinel;
        unsigned successes = 0;
        for (auto &clsid : DECODERS) {
            IClassFactory *probe = nullptr;
            auto supported = actual_factory(clsid, IID_IClassFactory, reinterpret_cast<void **>(&probe));
            if (probe) {
                probe->Release();
            }
            unsigned before = fallback_calls;
            IUnknown *object = nullptr;
            auto status = create_instance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown,
                    reinterpret_cast<void **>(&object));
            if (SUCCEEDED(supported)) {
                require(SUCCEEDED(status) && object && fallback_calls == before, "supported class uses local factory");
                object->Release();
                ++successes;
            } else {
                require(status == E_NOINTERFACE && !object && fallback_calls == before + 1,
                        "unsupported class preserves native fallback");
            }
        }
        require(successes > 0, "AX supports at least one target decoder");
    }

    void registry_tests() {
        require(!clvsd_key(nullptr), "null prefix");
        require(clvsd_key("software\\CYBERLINK\\common\\clvsd"), "case insensitive prefix");
        require(!clvsd_key("Software\\Cyberlink\\Common\\CLVSDother"), "prefix boundary");
        require(!clvsd_key("Software\\Cyberlink\\Common\\CLVS"), "short prefix");

        // A unique nonexistent subkey proves the settings work without creating
        // or relying on any registry entry on the test machine.
        auto path = std::string("Software\\Cyberlink\\Common\\CLVSD\\SpiceReadOnlyTest-") +
                std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount());
        HKEY native = nullptr;
        auto native_status = RegOpenKeyExA(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &native);
        if (native) {
            RegCloseKey(native);
        }
        require(native_status == ERROR_FILE_NOT_FOUND, "test subkey must not exist");
        HKEY key = nullptr;
        require(open_key(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS,
                "virtual key open");
        require(keys.at(key)->real == nullptr, "no underlying registry key");
        const std::pair<const char *, DWORD> expected[] = {
            {"EnableVMR", 1}, {"EnableVMR9", 1}, {"PreferYV12", 1},
            {"EnableDXVA", 0}, {"UIUseHVA", 0}, {"TwinviewDXVA", 0},
        };
        for (auto [name, value] : expected) {
            DWORD result = ~value, type = 0, size = sizeof(result);
            require(query_value(key, name, nullptr, &type, reinterpret_cast<BYTE *>(&result), &size)
                    == ERROR_SUCCESS && result == value && type == REG_DWORD && size == 4,
                    "six DWORD overrides");
            size = 0;
            require(query_value(key, name, nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS && size == 4,
                    "size query");
            require(query_value(key, name, nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS && type == REG_DWORD,
                    "type only query");
            require(query_value(key, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS,
                    "existence query");
            BYTE short_buffer[] = {0xAA, 0xBB};
            size = sizeof(short_buffer);
            require(query_value(key, name, nullptr, &type, short_buffer, &size) == ERROR_MORE_DATA &&
                    size == 4 && short_buffer[0] == 0xAA && short_buffer[1] == 0xBB,
                    "short buffer remains untouched");
        }
        DWORD data, size = sizeof(data), reserved = 0;
        require(query_value(key, "enabledxva", nullptr, nullptr, reinterpret_cast<BYTE *>(&data), &size)
                == ERROR_SUCCESS && data == 0, "case insensitive values");
        require(query_value(key, "EnableDXVA", nullptr, nullptr, reinterpret_cast<BYTE *>(&data), nullptr)
                == ERROR_INVALID_PARAMETER, "missing data capacity");
        require(query_value(key, "EnableDXVA", &reserved, nullptr, nullptr, &size)
                == ERROR_INVALID_PARAMETER, "reserved parameter");
        require(query_value(key, "UnknownSpiceValue", nullptr, nullptr, nullptr, &size)
                == ERROR_FILE_NOT_FOUND, "unknown value");
        require(close_key(key) == ERROR_SUCCESS && keys.empty(), "close releases virtual handle");

        // Race queries against close without allocating new keys that could reuse
        // the pointer. Queries serialized after close must reject the stale handle.
        for (int round = 0; round < 100; ++round) {
            require(open_key(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS, "race open");
            std::atomic<bool> start{false};
            std::atomic<unsigned> invalid{0};
            std::vector<std::thread> threads;
            for (int i = 0; i < 4; ++i) {
                threads.emplace_back([&] {
                    while (!start.load()) {
                        std::this_thread::yield();
                    }
                    for (int j = 0; j < 100; ++j) {
                        DWORD value = 9, capacity = 4;
                        auto status = query_value(key, "EnableDXVA", nullptr, nullptr,
                                reinterpret_cast<BYTE *>(&value), &capacity);
                        if (!((status == ERROR_SUCCESS && value == 0) ||
                                status == ERROR_INVALID_HANDLE)) {
                            ++invalid;
                        }
                    }
                });
            }
            start = true;
            require(close_key(key) == ERROR_SUCCESS, "race close");
            for (auto &thread : threads) {
                thread.join();
            }
            require(invalid == 0 && keys.empty(), "concurrent query/close");
        }
        require(RegOpenKeyExA(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &native)
                == ERROR_FILE_NOT_FOUND, "tests did not create a registry entry");
        std::cout << "PASS: registry values, buffers, boundaries and 40,000 concurrent queries\n";
    }

    void integration_test(const char *path) {
        auto module = LoadLibraryExW(std::filesystem::absolute(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        require(module != nullptr, "load real 32-bit CLVSD.ax");
        configure(module);
        // Repeat configuration to catch accidental self-recursive original API pointers.
        configure(module);
        require(*detour::iat_find("RegOpenKeyExA", module, nullptr) == reinterpret_cast<void *>(open_key),
                "AX registry open hooked");
        auto factory_function = reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject"));
        require(factory_function != nullptr, "real class factory export");
        auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        require(SUCCEEDED(hr), "COM initialize");
        IClassFactory *factory = nullptr;
        for (size_t index = 0; index < std::size(DECODERS); ++index) {
            hr = factory_function(DECODERS[index], IID_IClassFactory, reinterpret_cast<void **>(&factory));
            std::cout << "Real decoder CLSID index " << index << " factory HRESULT 0x"
                    << std::hex << static_cast<unsigned>(hr) << std::dec << '\n';
            if (SUCCEEDED(hr) && factory) {
                break;
            }
        }
        require(SUCCEEDED(hr) && factory, "real decoder factory");
        IUnknown *filter = nullptr;
        hr = factory->CreateInstance(nullptr, IID_IUnknown, reinterpret_cast<void **>(&filter));
        require(SUCCEEDED(hr) && filter, "real decoder COM creation");
        filter->Release();
        factory->Release();
        std::call_once(load_once, [] {});
        local_factory = factory_function;
        verify_creation_hook(factory_function);
        CoUninitialize();
        require(keys.empty(), "real filter released registry handles");
        // Match production's pinned loader lifetime; no decoder unload while its
        // imports contain test callbacks. Process exit reclaims the module.
        std::cout << "PASS: configured real CLVSD.ax twice; created and released decoder COM object\n";
    }

    void loader_probe(const char *expected_path, const char *modules) {
        MODULE_PATH = modules;
        require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "probe COM initialize");
        std::call_once(load_once, load_local);
        auto module = GetModuleHandleW(std::filesystem::absolute(expected_path).c_str());
        require(module && local_factory == reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject")),
                "loader selected expected AX path");
        verify_creation_hook(local_factory);
        require(keys.empty(), "probe COM objects closed registry handles");
        CoUninitialize();
        std::cout << "PASS: production loader path and create_instance fallback\n";
    }

    void loader_subprocess_tests(const char *ax) {
        auto executable = libutils::module_file_name(nullptr);
        auto base = std::filesystem::temp_directory_path() /
                ("spice-xgcodec-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount()));
        std::filesystem::create_directory(base);
        for (auto scenario : {"root-first", "modules-fallback"}) {
            auto root = base / scenario;
            auto modules = root / "modules";
            auto cwd = root / "arbitrary-cwd";
            std::filesystem::create_directories(modules);
            std::filesystem::create_directory(cwd);
            auto child = root / "codec-test.exe";
            std::filesystem::copy_file(executable, child);
            auto module_ax = modules / "CLVSD.ax";
            std::filesystem::copy_file(ax, module_ax);
            std::filesystem::permissions(module_ax, std::filesystem::perms::owner_write,
                    std::filesystem::perm_options::add);
            auto expected = module_ax;
            if (!strcmp(scenario, "root-first")) {
                expected = root / "CLVSD.ax";
                std::filesystem::copy_file(ax, expected);
                std::filesystem::permissions(expected, std::filesystem::perms::owner_write,
                        std::filesystem::perm_options::add);
            }
            std::wstring command = L"\"" + child.wstring() + L"\" --loader \"" + expected.wstring() +
                    L"\" \"" + modules.wstring() + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            require(CreateProcessW(child.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                    nullptr, cwd.c_str(), &startup, &process), "launch isolated loader probe");
            auto wait = WaitForSingleObject(process.hProcess, 30000);
            DWORD code = 1;
            GetExitCodeProcess(process.hProcess, &code);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            require(wait == WAIT_OBJECT_0 && code == 0, "isolated loader probe passed");
            // Remove only exact files/directories created above; no recursive deletion.
            std::filesystem::remove(child);
            std::filesystem::remove(module_ax);
            if (expected != module_ax) {
                std::filesystem::remove(expected);
            }
            std::filesystem::remove(cwd);
            std::filesystem::remove(modules);
            std::filesystem::remove(root);
        }
        std::filesystem::remove(base);
        std::cout << "PASS: root priority and modules fallback from arbitrary working directories\n";
    }
}
int main(int argc, char **argv) {
    try {
        static_assert(sizeof(void *) == 4, "real decoder test requires x86");
        if (argc == 4 && !strcmp(argv[1], "--loader")) {
            loader_probe(argv[2], argv[3]);
            return 0;
        }
        registry_tests();
        if (argc > 1) {
            integration_test(argv[1]);
            loader_subprocess_tests(argv[1]);
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << " (Win32 " << GetLastError() << ")\n";
        return 1;
    }
}
