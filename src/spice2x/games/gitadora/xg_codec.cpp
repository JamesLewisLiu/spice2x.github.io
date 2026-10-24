#include "xg_codec.h"

#ifndef SPICE64
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <objbase.h>

#include "avs/game.h"
#include "cfg/configurator.h"
#include "launcher/launcher.h"
#include "util/detour.h"
#include "util/libutils.h"
#include "util/logging.h"

namespace games::gitadora::xg_codec {
namespace {
    constexpr CLSID DECODERS[] = {
        {0x37991d68, 0x42a3, 0x40e3, {0x8c, 5, 3, 0x71, 0x70, 0xe1, 0xa4, 0x2a}},
        {0x8acd52ed, 0x9c2d, 0x4008, {0x91, 0x29, 0xdc, 0xe9, 0x55, 0xd8, 0x60, 0x65}},
    };
    using GetClassObject = HRESULT (WINAPI *)(REFCLSID, REFIID, void **);
    decltype(&CoCreateInstance) original_create = nullptr;
    GetClassObject local_factory = nullptr;
    std::once_flag load_once;
    std::once_flag registered_once[2];
    std::mutex registry_mutex;
    std::mutex configuration_mutex;
    std::unordered_set<HMODULE> configured;
    struct Key {
        HKEY real;
    };
    std::unordered_map<HKEY, Key *> keys;

    bool clvsd_key(LPCSTR path) {
        constexpr char prefix[] = "Software\\Cyberlink\\Common\\CLVSD";
        constexpr size_t length = sizeof(prefix) - 1;
        return path && _strnicmp(path, prefix, length) == 0 &&
                (path[length] == '\0' || path[length] == '\\');
    }

    LSTATUS WINAPI open_key(HKEY root, LPCSTR path, DWORD options, REGSAM access,
            PHKEY result) {
        if (result && (root == HKEY_LOCAL_MACHINE || root == HKEY_CURRENT_USER) &&
                clvsd_key(path) && !(access & (KEY_SET_VALUE | KEY_CREATE_SUB_KEY))) {
            // A virtual read handle also works on machines with no CyberLink registry key.
            HKEY real = nullptr;
            RegOpenKeyExA(root, path, options, access, &real);
            auto key = new Key{real};
            auto handle = reinterpret_cast<HKEY>(key);
            std::lock_guard lock(registry_mutex);
            keys.emplace(handle, key);
            *result = handle;
            return ERROR_SUCCESS;
        }
        return RegOpenKeyExA(root, path, options, access, result);
    }

    LSTATUS WINAPI query_value(HKEY key, LPCSTR name, LPDWORD reserved, LPDWORD type,
            LPBYTE data, LPDWORD size) {
        std::lock_guard lock(registry_mutex);
        auto it = keys.find(key);
        if (it == keys.end()) {
            return RegQueryValueExA(key, name, reserved, type, data, size);
        }
        const std::pair<const char *, DWORD> settings[] = {
            {"EnableVMR", 1}, {"EnableVMR9", 1}, {"PreferYV12", 1},
            {"EnableDXVA", 0}, {"UIUseHVA", 0}, {"TwinviewDXVA", 0},
        };
        for (const auto &[setting, value] : settings) {
            if (name && _stricmp(name, setting) == 0) {
                if (reserved || (data && !size)) {
                    return ERROR_INVALID_PARAMETER;
                }
                if (type) {
                    *type = REG_DWORD;
                }
                auto capacity = size ? *size : 0;
                if (size) {
                    *size = sizeof(value);
                }
                if (data && capacity < sizeof(value)) {
                    return ERROR_MORE_DATA;
                }
                if (data) {
                    memcpy(data, &value, sizeof(value));
                }
                return ERROR_SUCCESS;
            }
        }
        return it->second->real ?
                RegQueryValueExA(it->second->real, name, reserved, type, data, size) :
                ERROR_FILE_NOT_FOUND;
    }

    LSTATUS WINAPI close_key(HKEY key) {
        std::lock_guard lock(registry_mutex);
        auto it = keys.find(key);
        if (it == keys.end()) {
            return RegCloseKey(key);
        }
        if (it->second->real) {
            RegCloseKey(it->second->real);
        }
        delete it->second;
        keys.erase(it);
        return ERROR_SUCCESS;
    }

    void configure(HMODULE module) {
        std::lock_guard lock(configuration_mutex);
        if (!configured.insert(module).second) {
            return;
        }
        // Only this decoder's imports see the override, including its per-EXE/HKCU reads.
        // Install query/close before open can publish any virtual handles.
        auto query = detour::iat_try("RegQueryValueExA", query_value, module);
        auto close = detour::iat_try("RegCloseKey", close_key, module);
        auto open = query && close ? detour::iat_try("RegOpenKeyExA", open_key, module) :
                nullptr;
        if (!open) {
            log_warning("xg-codec", "Decoder registry imports unsupported; DXVA override unavailable");
        } else {
            log_info("xg-codec", "Enabled process-local VMR9/software decoding settings");
        }
    }

    void load_local() {
        const auto directory = libutils::module_file_name(nullptr).parent_path();
        const std::array paths = {directory / "CLVSD.ax", MODULE_PATH / "CLVSD.ax"};
        for (size_t i = 0; i < paths.size(); ++i) {
            const auto path = std::filesystem::absolute(paths[i]);
            if (i && path == std::filesystem::absolute(paths[0])) {
                continue;
            }
            auto attributes = GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                continue;
            }
            auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!module) {
                log_warning("xg-codec", "Cannot load {} (error {})", path, GetLastError());
                continue;
            }
            auto factory = reinterpret_cast<GetClassObject>(
                    GetProcAddress(module, "DllGetClassObject"));
            if (!factory) {
                FreeLibrary(module);
                continue;
            }
            configure(module);
            local_factory = factory;
            // Keep the loader reference: libmovie calls CoFreeUnusedLibraries between graphs.
            log_info("xg-codec", "Using local decoder {} without registration", path);
            return;
        }
        log_info("xg-codec", "No usable local CLVSD.ax; using registered decoder");
    }

    void configure_registered(REFCLSID clsid) {
        wchar_t guid[40]{};
        StringFromGUID2(clsid, guid, 40);
        const auto key = std::wstring(L"CLSID\\") + guid + L"\\InprocServer32";
        wchar_t path[32768]{};
        DWORD size = sizeof(path);
        HKEY registration = nullptr;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key.c_str(), 0, KEY_READ,
                &registration) != ERROR_SUCCESS) {
            return;
        }
        DWORD type = 0;
        auto status = RegQueryValueExW(registration, nullptr, nullptr, &type,
                reinterpret_cast<LPBYTE>(path), &size);
        RegCloseKey(registration);
        if (status == ERROR_SUCCESS && type == REG_SZ && size >= sizeof(wchar_t) &&
                size <= sizeof(path) && path[size / sizeof(wchar_t) - 1] == L'\0') {
            auto module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (module) {
                configure(module);
                // Retain this reference too, so the installed imports remain valid.
            }
        }
    }

    HRESULT WINAPI create_instance(REFCLSID clsid, IUnknown *outer, DWORD context,
            REFIID iid, void **object) {
        for (size_t i = 0; i < std::size(DECODERS); ++i) {
            if (!IsEqualGUID(clsid, DECODERS[i]) || !(context & CLSCTX_INPROC_SERVER)) {
                continue;
            }
            if (!object) {
                return E_POINTER;
            }
            std::call_once(load_once, load_local);
            if (local_factory) {
                *object = nullptr;
                IClassFactory *factory = nullptr;
                auto hr = local_factory(clsid, IID_IClassFactory,
                        reinterpret_cast<void **>(&factory));
                if (SUCCEEDED(hr) && factory) {
                    hr = factory->CreateInstance(outer, iid, object);
                }
                if (factory) {
                    factory->Release();
                }
                if (SUCCEEDED(hr) && *object) {
                    return hr;
                }
                *object = nullptr;
                log_misc("xg-codec", "Local decoder creation failed; keeping native COM fallback");
            }
            std::call_once(registered_once[i], [&]() { configure_registered(clsid); });
            break;
        }
        return original_create(clsid, outer, context, iid, object);
    }
}

    void attach() {
        if (cfg::CONFIGURATOR_STANDALONE ||
                !avs::game::is_model({"K32", "K33", "L32", "L33"})) {
            return;
        }
        auto movie = GetModuleHandleW(L"libmovie.dll");
        auto slot = movie ? detour::iat_find("CoCreateInstance", movie, "ole32.dll") : nullptr;
        if (!slot) {
            log_warning("xg-codec", "libmovie CoCreateInstance import unavailable; codec hook skipped");
            return;
        }
        if (*slot == reinterpret_cast<void *>(create_instance)) {
            return;
        }
        original_create = reinterpret_cast<decltype(original_create)>(*slot);
        detour::iat("CoCreateInstance", create_instance, movie);
        log_info("xg-codec", "Installed libmovie decoder hook after shared COM hooks");
    }
}
#else
namespace games::gitadora::xg_codec {
    void attach() {}
}
#endif
