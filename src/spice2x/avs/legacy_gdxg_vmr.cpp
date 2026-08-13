#include "legacy_gdxg_vmr.h"

#ifndef SPICE64

#include <d3d9.h>
#include <objbase.h>

#include <algorithm>
#include <cstddef>
#include <new>
#include <vector>

#include "launcher/launcher.h"
#include "util/libutils.h"
#include "util/logging.h"

namespace avs::legacy_gdxg::vmr {

    namespace {

        // Private COM classes and interfaces shipped by XG1's libvmrsvr.dll.
        static constexpr GUID CLSID_WIZARD = {
                0x8d372f4d, 0xe0c0, 0x464f, {0xbd, 0xa9, 0x96, 0x39, 0x24, 0x89, 0x0a, 0x35}};
        static constexpr GUID IID_WIZARD = {
                0x6d402155, 0xd941, 0x478a, {0xbe, 0xbb, 0xaf, 0x36, 0x1f, 0x63, 0x38, 0x51}};
        static constexpr GUID CLSID_RENDER_ENGINE = {
                0x9401081e, 0x848a, 0x4da7, {0xb6, 0xc1, 0x98, 0x28, 0xbb, 0x49, 0x3e, 0x4f}};
        static constexpr GUID IID_RENDER_ENGINE = {
                0x6e474394, 0x49fd, 0x416f, {0xae, 0xd4, 0x0e, 0xda, 0xd1, 0x87, 0x9d, 0xc7}};
        static constexpr GUID IID_GAME_MIXER = {
                0x6ee46528, 0x3c31, 0x43af, {0xb8, 0xb6, 0x1c, 0x7a, 0x53, 0xa2, 0xe5, 0xc2}};
        static constexpr GUID IID_COM_IUNKNOWN = {
                0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
        static constexpr GUID IID_COM_CLASS_FACTORY = {
                0x00000001, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

        static constexpr HRESULT VFW_E_NOT_FOUND = static_cast<HRESULT>(0x80040216L);
        static constexpr HRESULT VFW_E_WRONG_STATE = static_cast<HRESULT>(0x80040227L);

        template<std::size_t Index, typename... Args>
        HRESULT invoke(IUnknown *object, Args... args) {
            using function_t = HRESULT (STDMETHODCALLTYPE *)(IUnknown *, Args...);
            auto vtable = *reinterpret_cast<void ***>(object);
            return reinterpret_cast<function_t>(vtable[Index])(object, args...);
        }

        struct GameMixer;

        struct PrimaryVtable {
            HRESULT (STDMETHODCALLTYPE *query_interface)(void *, REFIID, void **);
            ULONG (STDMETHODCALLTYPE *add_ref)(void *);
            ULONG (STDMETHODCALLTYPE *release)(void *);
        };

        struct SecondaryVtable {
            HRESULT (STDMETHODCALLTYPE *query_interface)(void *, REFIID, void **);
            ULONG (STDMETHODCALLTYPE *add_ref)(void *);
            ULONG (STDMETHODCALLTYPE *release)(void *);
            HRESULT (STDMETHODCALLTYPE *set_attached)(void *, IUnknown *);
            HRESULT (STDMETHODCALLTYPE *get_attached)(void *, IUnknown **);
            HRESULT (STDMETHODCALLTYPE *initialize)(void *, BOOL);
            HRESULT (STDMETHODCALLTYPE *terminate)(void *);
            HRESULT (STDMETHODCALLTYPE *reinitialize)(void *, BOOL);
            HRESULT (STDMETHODCALLTYPE *check_state)(void *, DWORD);
            HRESULT (STDMETHODCALLTYPE *set_device)(void *, IDirect3DDevice9 *, DWORD);
            HRESULT (STDMETHODCALLTYPE *add_source)(void *, DWORD, int, int, int, int);
            HRESULT (STDMETHODCALLTYPE *remove_source)(void *, DWORD);
        };

        struct SourceInfo {
            DWORD id;
            float source_width;
            float source_height;
            float target_width;
            float target_height;
            float width_scale;
            float height_scale;
        };

        struct GameMixer {
            const PrimaryVtable *primary_vtable;
            const SecondaryVtable *secondary_vtable;
            LONG reference_count;
            CRITICAL_SECTION lock;
            bool initialized;
            IUnknown *attached;
            std::vector<SourceInfo> sources;

            explicit GameMixer();
            ~GameMixer();

            void *primary() {
                return &primary_vtable;
            }

            void *secondary() {
                return &secondary_vtable;
            }

            static GameMixer *from_primary(void *self) {
                return reinterpret_cast<GameMixer *>(self);
            }

            static GameMixer *from_secondary(void *self) {
                return reinterpret_cast<GameMixer *>(
                        reinterpret_cast<char *>(self) - sizeof(const PrimaryVtable *));
            }

            static HRESULT STDMETHODCALLTYPE query_interface(void *self, REFIID iid, void **object);
            static ULONG STDMETHODCALLTYPE add_ref(void *self);
            static ULONG STDMETHODCALLTYPE release(void *self);

            static HRESULT STDMETHODCALLTYPE secondary_query_interface(void *self, REFIID iid, void **object);
            static ULONG STDMETHODCALLTYPE secondary_add_ref(void *self);
            static ULONG STDMETHODCALLTYPE secondary_release(void *self);
            static HRESULT STDMETHODCALLTYPE set_attached(void *self, IUnknown *attached);
            static HRESULT STDMETHODCALLTYPE get_attached(void *self, IUnknown **attached);
            static HRESULT STDMETHODCALLTYPE initialize(void *self, BOOL enabled);
            static HRESULT STDMETHODCALLTYPE terminate(void *self);
            static HRESULT STDMETHODCALLTYPE reinitialize(void *self, BOOL enabled);
            static HRESULT STDMETHODCALLTYPE check_state(void *self, DWORD reserved);
            static HRESULT STDMETHODCALLTYPE set_device(void *self, IDirect3DDevice9 *device, DWORD reserved);
            static HRESULT STDMETHODCALLTYPE add_source(
                    void *self, DWORD id, int source_width, int source_height, int target_width, int target_height);
            static HRESULT STDMETHODCALLTYPE remove_source(void *self, DWORD id);
        };

        static const PrimaryVtable PRIMARY_VTABLE = {
                GameMixer::query_interface,
                GameMixer::add_ref,
                GameMixer::release,
        };

        static const SecondaryVtable SECONDARY_VTABLE = {
                GameMixer::secondary_query_interface,
                GameMixer::secondary_add_ref,
                GameMixer::secondary_release,
                GameMixer::set_attached,
                GameMixer::get_attached,
                GameMixer::initialize,
                GameMixer::terminate,
                GameMixer::reinitialize,
                GameMixer::check_state,
                GameMixer::set_device,
                GameMixer::add_source,
                GameMixer::remove_source,
        };

        static HMODULE vmr_module = nullptr;
        static IUnknown *wizard = nullptr;
        static IUnknown *render_engine_factory = nullptr;
        static IUnknown *engine = nullptr;
        static IUnknown *engine_internal = nullptr;
        static GameMixer *game_mixer = nullptr;
        static set_d3d_device_t set_d3d_device_callback = nullptr;
        static bool wizard_connected = false;

        using dll_get_class_object_t = HRESULT (STDAPICALLTYPE *)(REFCLSID, REFIID, void **);

        void release(IUnknown *&object) {
            if (object) {
                object->Release();
                object = nullptr;
            }
        }

        HRESULT create_instance(REFCLSID clsid, REFIID iid, IUnknown **object) {
            auto dll_get_class_object = libutils::get_proc<dll_get_class_object_t>(
                    vmr_module, "DllGetClassObject");
            IClassFactory *factory = nullptr;
            auto result = dll_get_class_object(
                    clsid, IID_COM_CLASS_FACTORY, reinterpret_cast<void **>(&factory));
            if (FAILED(result)) {
                return result;
            }
            result = factory->CreateInstance(nullptr, iid, reinterpret_cast<void **>(object));
            factory->Release();
            return result;
        }

        HRESULT log_step(const char *step, HRESULT result) {
            if (FAILED(result)) {
                log_warning("legacy-gdxg", "VMR {} failed: {}", step, FMT_HRESULT(result));
            } else {
                log_info("legacy-gdxg", "VMR {} succeeded", step);
            }
            return result;
        }

        class CriticalSectionLock {
        public:
            explicit CriticalSectionLock(CRITICAL_SECTION *lock) : lock(lock) {
                EnterCriticalSection(lock);
            }

            ~CriticalSectionLock() {
                LeaveCriticalSection(lock);
            }

        private:
            CRITICAL_SECTION *lock;
        };
    }

    GameMixer::GameMixer()
        : primary_vtable(&PRIMARY_VTABLE),
          secondary_vtable(&SECONDARY_VTABLE),
          reference_count(1),
          initialized(false),
          attached(nullptr) {
        InitializeCriticalSection(&lock);
    }

    GameMixer::~GameMixer() {
        if (attached) {
            attached->Release();
            attached = nullptr;
        }
        DeleteCriticalSection(&lock);
    }

    HRESULT STDMETHODCALLTYPE GameMixer::query_interface(void *self, REFIID iid, void **object) {
        if (!object) {
            return E_POINTER;
        }
        auto mixer = from_primary(self);
        if (IsEqualIID(iid, IID_COM_IUNKNOWN)) {
            *object = mixer->primary();
        } else if (IsEqualIID(iid, IID_GAME_MIXER)) {
            *object = mixer->secondary();
        } else {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        InterlockedIncrement(&mixer->reference_count);
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE GameMixer::add_ref(void *self) {
        return static_cast<ULONG>(InterlockedIncrement(&from_primary(self)->reference_count));
    }

    ULONG STDMETHODCALLTYPE GameMixer::release(void *self) {
        auto mixer = from_primary(self);
        const auto count = InterlockedDecrement(&mixer->reference_count);
        if (count == 0) {
            delete mixer;
            return 0;
        }
        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE GameMixer::secondary_query_interface(void *self, REFIID iid, void **object) {
        return query_interface(from_secondary(self)->primary(), iid, object);
    }

    ULONG STDMETHODCALLTYPE GameMixer::secondary_add_ref(void *self) {
        return add_ref(from_secondary(self)->primary());
    }

    ULONG STDMETHODCALLTYPE GameMixer::secondary_release(void *self) {
        return release(from_secondary(self)->primary());
    }

    HRESULT STDMETHODCALLTYPE GameMixer::set_attached(void *self, IUnknown *new_attached) {
        auto mixer = from_secondary(self);
        CriticalSectionLock guard(&mixer->lock);
        if (new_attached) {
            new_attached->AddRef();
        }
        if (mixer->attached) {
            mixer->attached->Release();
        }
        mixer->attached = new_attached;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GameMixer::get_attached(void *self, IUnknown **result) {
        if (!result) {
            return E_POINTER;
        }
        auto mixer = from_secondary(self);
        CriticalSectionLock guard(&mixer->lock);
        if (!mixer->attached) {
            *result = nullptr;
            return VFW_E_NOT_FOUND;
        }
        mixer->attached->AddRef();
        *result = mixer->attached;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GameMixer::initialize(void *self, BOOL enabled) {
        auto mixer = from_secondary(self);
        CriticalSectionLock guard(&mixer->lock);
        if (mixer->initialized) {
            return VFW_E_WRONG_STATE;
        }
        if (!enabled) {
            return E_POINTER;
        }
        mixer->initialized = true;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GameMixer::terminate(void *self) {
        auto mixer = from_secondary(self);
        CriticalSectionLock guard(&mixer->lock);
        mixer->initialized = false;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GameMixer::reinitialize(void *self, BOOL enabled) {
        if (!enabled) {
            return E_POINTER;
        }
        auto mixer = from_secondary(self);
        CriticalSectionLock guard(&mixer->lock);
        mixer->initialized = true;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GameMixer::check_state(void *self, DWORD reserved) {
        (void) reserved;
        auto mixer = from_secondary(self);
        CriticalSectionLock guard(&mixer->lock);
        return mixer->initialized ? S_OK : VFW_E_WRONG_STATE;
    }

    HRESULT STDMETHODCALLTYPE GameMixer::set_device(void *self, IDirect3DDevice9 *device, DWORD reserved) {
        (void) reserved;
        if (set_d3d_device_callback) {
            set_d3d_device_callback(device);
        }
        if (!device) {
            return E_POINTER;
        }
        auto mixer = from_secondary(self);
        CriticalSectionLock guard(&mixer->lock);
        if (!mixer->initialized) {
            return VFW_E_WRONG_STATE;
        }
        return mixer->attached ? S_OK : E_UNEXPECTED;
    }

    HRESULT STDMETHODCALLTYPE GameMixer::add_source(
            void *self, DWORD id, int source_width, int source_height, int target_width, int target_height) {
        auto mixer = from_secondary(self);
        try {
            SourceInfo source{
                    id,
                    static_cast<float>(source_width),
                    static_cast<float>(source_height),
                    static_cast<float>(target_width),
                    static_cast<float>(target_height),
                    target_width ? static_cast<float>(source_width) / static_cast<float>(target_width) : 0.0f,
                    target_height ? static_cast<float>(source_height) / static_cast<float>(target_height) : 0.0f,
            };
            CriticalSectionLock guard(&mixer->lock);
            auto existing = std::find_if(
                    mixer->sources.begin(), mixer->sources.end(), [id](const SourceInfo &item) {
                        return item.id == id;
                    });
            if (existing != mixer->sources.end()) {
                mixer->sources.erase(existing);
            }
            mixer->sources.push_back(source);
        } catch (const std::bad_alloc &) {
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GameMixer::remove_source(void *self, DWORD id) {
        auto mixer = from_secondary(self);
        CriticalSectionLock guard(&mixer->lock);
        auto source = std::find_if(mixer->sources.begin(), mixer->sources.end(), [id](const SourceInfo &item) {
            return item.id == id;
        });
        if (source == mixer->sources.end()) {
            return VFW_E_NOT_FOUND;
        }
        mixer->sources.erase(source);
        return S_OK;
    }

    bool initialize(HWND window, set_d3d_device_t set_d3d_device) {
        if (engine) {
            return true;
        }

        set_d3d_device_callback = set_d3d_device;
        vmr_module = libutils::load_library(MODULE_PATH / "libvmrsvr.dll");

        HRESULT result = create_instance(CLSID_WIZARD, IID_WIZARD, &wizard);
        if (FAILED(log_step("create Wizard", result))) {
            shutdown();
            return false;
        }
        result = create_instance(CLSID_RENDER_ENGINE, IID_RENDER_ENGINE, &render_engine_factory);
        if (FAILED(log_step("create RenderEngine", result))) {
            shutdown();
            return false;
        }

        game_mixer = new (std::nothrow) GameMixer();
        if (!game_mixer) {
            log_warning("legacy-gdxg", "VMR could not allocate the game mixer");
            shutdown();
            return false;
        }

        result = invoke<3>(
                render_engine_factory,
                window,
                static_cast<DWORD>(0),
                reinterpret_cast<IUnknown *>(game_mixer->secondary()));
        if (FAILED(log_step("attach game mixer to RenderEngine", result))) {
            shutdown();
            return false;
        }
        result = invoke<3>(wizard, static_cast<DWORD>(0), window, render_engine_factory);
        if (FAILED(log_step("connect Wizard to RenderEngine", result))) {
            shutdown();
            return false;
        }
        wizard_connected = true;
        result = invoke<13>(wizard, &engine);
        if (FAILED(log_step("get render engine", result))) {
            shutdown();
            return false;
        }
        result = invoke<10>(engine, &engine_internal);
        if (FAILED(log_step("get render engine internal interface", result))) {
            shutdown();
            return false;
        }

        IDirect3DDevice9 *device = nullptr;
        result = invoke<11>(engine, &device);
        if (FAILED(log_step("get D3D9 device", result))) {
            shutdown();
            return false;
        }
        if (set_d3d_device_callback) {
            set_d3d_device_callback(device);
        }
        if (device) {
            device->Release();
        }

        release(render_engine_factory);
        log_info("legacy-gdxg", "XG1 VMR bridge initialized successfully");
        return true;
    }

    IUnknown *render_engine() {
        return engine;
    }

    void stop_rendering() {
        if (wizard_connected && wizard) {
            log_step("shutdown Wizard", invoke<4>(wizard));
        }
        wizard_connected = false;
    }

    void shutdown() {
        stop_rendering();

        release(engine_internal);
        release(engine);
        release(render_engine_factory);
        release(wizard);
        if (game_mixer) {
            GameMixer::release(game_mixer->primary());
            game_mixer = nullptr;
        }
        set_d3d_device_callback = nullptr;
        if (vmr_module) {
            FreeLibrary(vmr_module);
            vmr_module = nullptr;
        }
    }
}

#else

namespace avs::legacy_gdxg::vmr {
    bool initialize(HWND window, set_d3d_device_t set_d3d_device) {
        (void) window;
        (void) set_d3d_device;
        return false;
    }

    IUnknown *render_engine() {
        return nullptr;
    }

    void stop_rendering() {
    }

    void shutdown() {
    }
}

#endif
