#include "capture.h"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include "external/rapidjson/document.h"
#include "hooks/graphics/graphics.h"
#include "util/crypt.h"
#include "util/threadpool.h"

using namespace std::placeholders;
using namespace rapidjson;

namespace api::modules {

    std::optional<uint32_t> CAPTURE_QUALITY;
    std::optional<uint32_t> CAPTURE_DIVIDE;

    static thread_local std::vector<uint8_t> CAPTURE_BUFFER;
    static thread_local int CAPTURE_PREFETCH_SCREEN = 0;
    static thread_local bool CAPTURE_PREFETCH_ENABLED = false;
    static thread_local bool CAPTURE_PREFETCH_QUEUED = false;
    static constexpr std::chrono::seconds CAPTURE_ACTIVE_WINDOW {1};

    struct CachedFrame {
        std::string encoded;
        uint64_t timestamp = 0;
        int width = 0;
        int height = 0;
    };

    struct CaptureState {
        std::shared_ptr<const CachedFrame> frame;
        std::chrono::steady_clock::time_point last_request;
        int quality = 70;
        int divide = 1;
        bool refreshing = false;
        bool capture_pending = false;
    };

    static std::mutex FRAME_CACHE_M;
    static std::unordered_map<int, CaptureState> FRAME_CACHE;

    static ThreadPool &capture_pool() {
        // Construct lazily: this module is loaded as a DLL in many games, so
        // starting threads during static initialization is unsafe.
        static ThreadPool pool(2);
        return pool;
    }

    static void add_frame_response(const CachedFrame &frame, Response &res) {
        Value data;
        data.SetString(frame.encoded.c_str(), frame.encoded.length(), res.doc()->GetAllocator());
        res.add_data(frame.timestamp);
        res.add_data(frame.width);
        res.add_data(frame.height);
        res.add_data(data);
    }

    static void receive_jpeg_byte(uint8_t byte) {
        // TooJpeg emits headers before doing the bulk of its work. Queueing the
        // next capture here lets GPU readback overlap the current JPEG encode.
        if (CAPTURE_PREFETCH_ENABLED) {
            CAPTURE_PREFETCH_ENABLED = false;
            CAPTURE_PREFETCH_QUEUED = true;
            graphics_capture_trigger(CAPTURE_PREFETCH_SCREEN);
        }
        CAPTURE_BUFFER.push_back(byte);
    }

    static std::shared_ptr<const CachedFrame> capture_frame(
            int screen,
            int quality,
            int divide,
            bool capture_pending = false,
            bool prefetch = false,
            bool *prefetch_queued = nullptr) {
        CAPTURE_BUFFER.clear();
        CAPTURE_BUFFER.reserve(1024 * 128);
        CAPTURE_PREFETCH_SCREEN = screen;
        CAPTURE_PREFETCH_ENABLED = prefetch;
        CAPTURE_PREFETCH_QUEUED = false;

        uint64_t timestamp = 0;
        int width = 0;
        int height = 0;
        if (!capture_pending) {
            graphics_capture_trigger(screen);
        }
        const bool success = graphics_capture_receive_jpeg(
                screen, receive_jpeg_byte, true, quality, true, divide,
                &timestamp, &width, &height);
        CAPTURE_PREFETCH_ENABLED = false;
        if (prefetch_queued) {
            *prefetch_queued = CAPTURE_PREFETCH_QUEUED;
        }

        if (!success) {
            CAPTURE_BUFFER.clear();
            return nullptr;
        }

        auto frame = std::make_shared<const CachedFrame>(CachedFrame {
            crypt::base64_encode(CAPTURE_BUFFER.data(), CAPTURE_BUFFER.size()),
            timestamp,
            width,
            height,
        });
        CAPTURE_BUFFER.clear();
        return frame;
    }

    static void refresh_cache(int screen) {
        while (true) {
            int quality;
            int divide;
            bool capture_pending;
            {
                std::lock_guard<std::mutex> lock(FRAME_CACHE_M);
                auto &state = FRAME_CACHE[screen];
                quality = state.quality;
                divide = state.divide;
                capture_pending = state.capture_pending;
            }

            bool next_capture_pending = false;
            auto frame = capture_frame(
                    screen, quality, divide, capture_pending, true, &next_capture_pending);

            std::lock_guard<std::mutex> lock(FRAME_CACHE_M);
            auto &state = FRAME_CACHE[screen];
            state.capture_pending = next_capture_pending;
            if (!frame) {
                state.refreshing = false;
                return;
            }
            state.frame = std::move(frame);

            // Keep producing while a mirror client is actively polling. This
            // overlaps capture/JPEG work with transmission and client decode.
            if (std::chrono::steady_clock::now() - state.last_request > CAPTURE_ACTIVE_WINDOW) {
                state.refreshing = false;
                return;
            }
        }
    }

    static std::shared_ptr<const CachedFrame> request_cached_frame(
            int screen, int quality, int divide) {
        std::shared_ptr<const CachedFrame> cached;
        bool start_refresh = false;
        {
            std::lock_guard<std::mutex> lock(FRAME_CACHE_M);
            auto &state = FRAME_CACHE[screen];
            state.last_request = std::chrono::steady_clock::now();
            state.quality = quality;
            state.divide = divide;
            cached = state.frame;
            if (cached && !state.refreshing) {
                state.refreshing = true;
                start_refresh = true;
            }
        }

        if (start_refresh) {
            capture_pool().add([screen] {
                refresh_cache(screen);
            });
        }
        return cached;
    }

    static void publish_first_frame(
            int screen,
            int quality,
            int divide,
            std::shared_ptr<const CachedFrame> frame) {
        {
            std::lock_guard<std::mutex> lock(FRAME_CACHE_M);
            auto &state = FRAME_CACHE[screen];
            state.frame = std::move(frame);
            state.last_request = std::chrono::steady_clock::now();
            state.quality = quality;
            state.divide = divide;
            state.refreshing = true;
            state.capture_pending = false;
        }
        capture_pool().add([screen] {
            refresh_cache(screen);
        });
    }

    Capture::Capture() : Module("capture") {
        functions["get_screens"] = std::bind(&Capture::get_screens, this, _1, _2);
        functions["get_jpg"] = std::bind(&Capture::get_jpg, this, _1, _2);
    }

    /**
     * get_screens()
     */
    void Capture::get_screens(Request &req, Response &res) {

        // aquire screens
        std::vector<int> screens;
        graphics_screens_get(screens);

        // add screens to response
        for (auto &screen : screens) {
            res.add_data(screen);
        }
    }

    /**
     * get_jpg([screen=0, quality=70, downscale=0, divide=1])
     * screen: uint specifying the window
     * quality: uint in range [0, 100]
     * reduce: uint for dividing image size
     */
    void Capture::get_jpg(Request &req, Response &res) {
        // settings
        int screen = 0;
        int quality = 70;
        int divide = 1;
        if (req.params.Size() > 0 && req.params[0].IsUint()) {
            screen = req.params[0].GetUint();
        }

        if (CAPTURE_QUALITY.has_value()) {
            quality = CAPTURE_QUALITY.value();
        } else if (req.params.Size() > 1 && req.params[1].IsUint()) {
            quality = req.params[1].GetUint();
        }

        if (CAPTURE_DIVIDE.has_value()) {
            divide = CAPTURE_DIVIDE.value();
        } else if (req.params.Size() > 2 && req.params[2].IsUint()) {
            divide = req.params[2].GetUint();
        }

        // Once the first frame is available, return it immediately and refresh
        // the cache in the background. This removes capture and JPEG encoding
        // from the request/response critical path.
        auto frame = request_cached_frame(screen, quality, divide);
        if (frame) {
            add_frame_response(*frame, res);
            return;
        }

        // Preserve the original first-frame behavior so callers don't receive
        // an empty response while the game is already presenting frames.
        frame = capture_frame(screen, quality, divide);
        if (frame) {
            add_frame_response(*frame, res);
            publish_first_frame(screen, quality, divide, std::move(frame));
        }
    }
}
