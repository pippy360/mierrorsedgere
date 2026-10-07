#pragma once

// Texture sampling and row-parallel loops shared by the reference renderer's 2D and 3D halves.

#include "frontend_assets.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace me::fe {

inline float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// srgb_to_linear for a byte, from a table: sampling is the renderer's inner loop.
inline const float* srgb_table() {
    static float table[256];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 256; ++i) table[i] = srgb_to_linear(static_cast<float>(i) / 255.0f);
        built = true;
    }
    return table;
}

inline int wrap_or_clamp(int i, int n, bool wrap) {
    if (wrap) {
        i %= n;
        return i < 0 ? i + n : i;
    }
    return std::clamp(i, 0, n - 1);
}

// Bilinear, texel centres at (i + 0.5) / size. Returns 0..1 per channel; `linear` undoes the
// sRGB encoding of the colour channels the way a material's sampler does for an SRGB texture.
inline void sample(const Image& img, float u, float v, float out[4], bool linear = false) {
    if (!img.valid()) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return;
    }
    const float fx = u * static_cast<float>(img.w) - 0.5f;
    const float fy = v * static_cast<float>(img.h) - 0.5f;
    const float flx = std::floor(fx), fly = std::floor(fy);
    const float tx = fx - flx, ty = fy - fly;
    const int x0 = wrap_or_clamp(static_cast<int>(flx), img.w, img.wrap_x);
    const int x1 = wrap_or_clamp(static_cast<int>(flx) + 1, img.w, img.wrap_x);
    const int y0 = wrap_or_clamp(static_cast<int>(fly), img.h, img.wrap_y);
    const int y1 = wrap_or_clamp(static_cast<int>(fly) + 1, img.h, img.wrap_y);
    const uint8_t* p00 = &img.px[(static_cast<size_t>(y0) * img.w + x0) * 4];
    const uint8_t* p10 = &img.px[(static_cast<size_t>(y0) * img.w + x1) * 4];
    const uint8_t* p01 = &img.px[(static_cast<size_t>(y1) * img.w + x0) * 4];
    const uint8_t* p11 = &img.px[(static_cast<size_t>(y1) * img.w + x1) * 4];
    const float w00 = (1.0f - tx) * (1.0f - ty), w10 = tx * (1.0f - ty), w01 = (1.0f - tx) * ty, w11 = tx * ty;
    if (linear) {
        const float* lut = srgb_table();
        for (int k = 0; k < 3; ++k) out[k] = lut[p00[k]] * w00 + lut[p10[k]] * w10 + lut[p01[k]] * w01 + lut[p11[k]] * w11;
    } else {
        for (int k = 0; k < 3; ++k) out[k] = (p00[k] * w00 + p10[k] * w10 + p01[k] * w01 + p11[k] * w11) * (1.0f / 255.0f);
    }
    out[3] = (p00[3] * w00 + p10[3] * w10 + p01[3] * w01 + p11[3] * w11) * (1.0f / 255.0f);
}

inline float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }

inline int worker_count() {
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    return static_cast<int>(std::min<unsigned>(hw, 16u));
}

// The threads the renderer's row loops run on. They are started once: a frame has a dozen such
// loops, and starting sixteen threads for each costs more than the loops themselves.
class RowPool {
public:
    static RowPool& get() {
        static RowPool pool;
        return pool;
    }

    // Calls fn(row0, row1) for bands that together cover [0, rows); returns when all are done.
    // Not re-entrant: fn must not start another row loop.
    void run(int rows, const std::function<void(int, int)>& fn) {
        // Four bands a thread, handed out as threads come free: the city fills the bottom of the
        // frame and the sky the top, so equal bands, one each, leave most threads idle.
        const int bands = std::min((static_cast<int>(threads_.size()) + 1) * 4, std::max(rows, 1));
        if (bands <= 1 || threads_.empty()) {
            fn(0, rows);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            fn_ = &fn;
            rows_ = rows;
            bands_ = bands;
            next_ = 0;
            pending_ = bands;
            ++generation_;
        }
        wake_.notify_all();
        work();  // the calling thread takes bands as well
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return pending_ == 0; });
        fn_ = nullptr;
    }

private:
    RowPool() {
        const int n = worker_count() - 1;
        for (int i = 0; i < n; ++i) threads_.emplace_back([this] { loop(); });
    }
    ~RowPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        for (std::thread& t : threads_) t.join();
    }

    void loop() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [&] { return quit_ || (generation_ != seen && fn_ != nullptr && next_ < bands_); });
                if (quit_) return;
                seen = generation_;
            }
            work();
        }
    }

    void work() {
        for (;;) {
            const std::function<void(int, int)>* fn = nullptr;
            int band = 0, rows = 0, bands = 1;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!fn_ || next_ >= bands_) return;
                band = next_++;
                fn = fn_;
                rows = rows_;
                bands = bands_;
            }
            (*fn)(rows * band / bands, rows * (band + 1) / bands);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (--pending_ == 0) done_.notify_all();
            }
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    const std::function<void(int, int)>* fn_ = nullptr;
    int rows_ = 0;
    int bands_ = 0;
    int next_ = 0;
    int pending_ = 0;
    uint64_t generation_ = 0;
    bool quit_ = false;
};

// Runs fn(row0, row1) over [0, rows) split into bands. Each band is written by one thread only,
// so nothing in the loop body needs a lock.
template <typename Fn>
void parallel_rows(int rows, Fn&& fn) {
    const std::function<void(int, int)> call(std::ref(fn));
    RowPool::get().run(rows, call);
}

}  // namespace me::fe
