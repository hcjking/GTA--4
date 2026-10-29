// GTA IV PS5 - the TV's refresh rate and frame pacing (see ps5_display.h).
#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include "ps5_display.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>

#include <time.h>

#include <rex/cvar.h>
#include <rex/frame_stats.h>
#include <rex/logging.h>

REXCVAR_DEFINE_UINT32(ps5_refresh_rate, 60, "PS5",
                      "The TV's refresh rate, 60 or 120 Hz (120 needs a TV that takes it); read when the app starts");
REXCVAR_DEFINE_STRING(ps5_frame_generation, "off", "PS5",
                      "Frame generation's rate: off, 30-60 (the game at 30, at either refresh rate) or 60-120 (the "
                      "game at 60 at 120 Hz, 30 where it can't hold 60); the hub sets gta4_native_frame_generation with it")
    .allowed({"off", "30-60", "60-120"});
REXCVAR_DEFINE_BOOL(ps5_pacing_log, true, "PS5",
                    "Every 10 s, log how long the TV showed each of the game's frames and how evenly they came");
REXCVAR_DECLARE(uint32_t, gta4_frame_limit);
REXCVAR_DECLARE(bool, ps5_overlay_every_vblank);
REXCVAR_DECLARE(bool, gta4_native_texture_watch);
REXCVAR_DECLARE(std::string, log_level);

extern "C" {
int sceVideoOutGetFlipStatus(int32_t handle, uint64_t status[16]);
int __real_sceVideoOutIsOutputSupported(int32_t handle, uint32_t mode, const void* reserved1,
                                        const void* reserved2, const void* reserved3);
int __real_sceVideoOutConfigureOutput(int32_t handle, uint32_t mode, const void* reserved1,
                                      const void* reserved2, const void* reserved3);
int __real_sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
int sceVideoOutWaitVblank(int32_t handle);
}

namespace gta4ps5::display {
namespace {

constexpr uint32_t kHighFrameRateMode = 15;  // (videoout.h: 119.88 Hz)
constexpr uint32_t kRestoreMode = 1;

// After the 120 Hz mode is taken (ps5_display.h): a TV at a fixed 60 Hz has
// every vblank kFixed60MinNs..kFixed60MaxNs apart (59.94 Hz: 16.683 ms). The
// median under kHighRefreshLimitNs is a fixed 120 Hz; past kFixed60MaxNs, the
// TV idling at its VRR floor; any one interval under kFixed60MinNs, a TV
// refreshing faster than 60 Hz (120, or VRR following flips).
constexpr int64_t kHighRefreshLimitNs = 12500000;
constexpr int64_t kFixed60MinNs = 14500000;
constexpr int64_t kFixed60MaxNs = 18500000;

std::atomic<int32_t> g_handle{-1};
std::atomic<bool> g_high{false};
std::atomic<bool> g_vrr{false};
std::atomic<bool> g_game{false};
std::atomic<int32_t> g_rate{-1};  // (the rate VideoOut has: -1 unknown)
std::mutex g_switch_mutex;               // (one switch at a time)

// The hub's setting, straight from its file: VideoOut opens before the config
// is loaded (as ps5_save_logs in windowed_app_main_sdl.cpp).
bool WantsHighRefresh() {
  bool high = false;
  if (std::FILE* f = std::fopen("/app0/UserData/LibertyRecomp/native.toml", "rb")) {
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
      const char* start = line + std::strspn(line, " \t");
      if (std::strncmp(start, "ps5_refresh_rate", 16) != 0) continue;
      const char* value = std::strchr(start, '=');
      if (!value) continue;
      value += 1 + std::strspn(value + 1, " \t\"");
      high = std::strtoul(value, nullptr, 10) >= 100;
    }
    std::fclose(f);
  }
  return high;
}

// Vblanks a frame stays for, less one (sceVideoOutSetFlipRate's rate).
int32_t WantedRate() {
  if (!g_game.load(std::memory_order_acquire)) return 0;
  // Frame generation: each image (generated or real) its own vblank, or two
  // while the game runs at a quarter of 120 Hz. The frame limit doesn't apply.
  if (rex::frame_stats::g_frame_generation_active.load(std::memory_order_acquire))
    return int32_t(std::max<uint32_t>(rex::frame_stats::g_fg_vblanks_per_image.load(std::memory_order_acquire), 1)) - 1;
  const uint32_t fps = REXCVAR_GET(gta4_frame_limit);
  // VRR: the TV refreshes when a frame comes; the frame limit's own pacing
  // (Ps5LimitFrameRate) spaces them. (30 and 40 are under most TVs' VRR
  // floor: held to vblanks as at a fixed 120 Hz.)
  if (g_vrr.load(std::memory_order_acquire) && (fps == 0 || fps == 60 || fps == 120)) return 0;
  if (g_high.load(std::memory_order_acquire)) {
    if (fps == 40) return 2;
    if (fps == 30 || fps == 60) return 1;
    return 0;
  }
  return fps == 30 ? 1 : 0;
}

void Apply(const char* why) {
  const int32_t handle = g_handle.load(std::memory_order_acquire);
  const int32_t rate = WantedRate();
  if (handle < 0 || g_rate.load(std::memory_order_acquire) == rate) return;
  const int result = __real_sceVideoOutSetFlipRate(handle, rate);
  if (result == 0) g_rate.store(rate, std::memory_order_release);
  REXLOG_INFO("PS5 display: each frame shows for at least {} vblank{} at {} Hz{} ({}; {})", rate + 1,
              rate ? "s" : "", g_high.load() ? "119.88" : "59.94", g_vrr.load() ? " VRR" : "", why,
              result == 0 ? std::string("ok") : fmt::format("failed {:08X}", uint32_t(result)));
}

// The time between vblanks over eight of them, the first wait finding the
// phase, sorted (a wait that misses a vblank gives a longer interval, never a
// shorter one). False when a wait fails.
bool VblankIntervals(int32_t handle, std::array<int64_t, 8>& intervals) {
  if (sceVideoOutWaitVblank(handle) != 0) return false;
  auto previous = std::chrono::steady_clock::now();
  for (int64_t& interval : intervals) {
    if (sceVideoOutWaitVblank(handle) != 0) return false;
    const auto now = std::chrono::steady_clock::now();
    interval = std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous).count();
    previous = now;
  }
  std::sort(intervals.begin(), intervals.end());
  return true;
}

// The pacing log (StartGamePacing). VideoOut's flip status (word 3: the flip
// shown last, numbered as RADV submits them, which is the presenter's image
// count: frame_stats::g_image_frame) is looked at every millisecond off the
// renderer's cores: when the frame on screen changes, the one before was shown
// from when it came until now (seen at most ~1 ms late), rounded to vblanks.
// A repeat of the same frame (Every Vblank) counts as the same frame. The
// game's own frame times come from frame_stats' history.
struct PacingWindow {
  std::array<uint32_t, 4> shown{};  // frames on screen for 1, 2, 3, 4 or more vblanks
  uint32_t changes = 0;             // a frame held for a different number than the one before
  uint32_t unseen = 0;              // frames shown and gone between two looks
  int64_t shown_min = 0, shown_max = 0;
  std::array<uint32_t, 4> game{};  // the game's frames: to 17.5 ms, 25, 34.5, longer
  uint32_t jumps = 0;              // a game frame 4 ms or more off the one before
};

void PacingMain() {
  using namespace rex::frame_stats;
  uint64_t cpus_applied = UINT64_MAX;
  int64_t flip = -1;             // the flip shown at the last look
  uint64_t frame = 0;            // the game frame (publication) it showed
  int64_t frame_since_ns = 0;    // when that frame came on screen (0: not known)
  uint32_t last_held = 0, last_game_us = 0;
  uint64_t game_seen = g_presents.load(std::memory_order_acquire);
  PacingWindow w;
  int64_t window_ns = NowNs();
  for (;;) {
    FollowRendererCpuMask(cpus_applied, RendererRole::kOther);
    const timespec delay{0, 1000000};
    nanosleep(&delay, nullptr);
    const int64_t now = NowNs();
    const int32_t handle = g_handle.load(std::memory_order_acquire);
    uint64_t status[16] = {};
    if (handle >= 0 && sceVideoOutGetFlipStatus(handle, status) == 0 && int64_t(status[3]) != flip) {
      const int64_t shown = int64_t(status[3]);
      const uint64_t sent = g_images_sent.load(std::memory_order_acquire);
      const bool known = shown > 0 && uint64_t(shown) <= sent && sent - uint64_t(shown) < kImageHistory;
      if (known && flip > 0 && shown > flip && shown - flip <= int64_t(kImageHistory)) {
        // the frames of the flips since the last look: a change of frame ends the one before
        uint32_t changed = 0;
        uint64_t f = frame;
        for (int64_t i = flip + 1; i <= shown; ++i) {
          const uint64_t next = g_image_frame[uint64_t(i) % kImageHistory].load(std::memory_order_relaxed);
          if (next != f) ++changed, f = next;
        }
        if (changed == 1 && frame_since_ns) {
          const int64_t held = now - frame_since_ns;
          const double vblank = g_high.load(std::memory_order_relaxed) ? 8341700.0 : 16683400.0;
          const uint32_t n = uint32_t(std::clamp<long>(std::lround(double(held) / vblank), 1, 4));
          ++w.shown[n - 1];
          if (last_held && n != last_held) ++w.changes;
          last_held = n;
          w.shown_min = w.shown_min ? std::min(w.shown_min, held) : held;
          w.shown_max = std::max(w.shown_max, held);
        } else if (changed > 1) {
          w.unseen += changed - 1;
          last_held = 0;
        }
        if (changed) frame = f, frame_since_ns = now;
      } else {
        frame = known ? g_image_frame[uint64_t(shown) % kImageHistory].load(std::memory_order_relaxed) : 0;
        frame_since_ns = 0;
        last_held = 0;
      }
      flip = shown;
    }
    // the game's frames since the last look
    const uint64_t presents = g_presents.load(std::memory_order_acquire);
    if (presents - game_seen > kHistory) game_seen = presents - kHistory;
    for (; game_seen < presents; ++game_seen) {
      const uint32_t us = g_history_us[game_seen % kHistory].load(std::memory_order_relaxed);
      if (!us) continue;
      ++w.game[us <= 17500 ? 0 : us <= 25000 ? 1 : us <= 34500 ? 2 : 3];
      if (last_game_us && (us > last_game_us + 4000 || last_game_us > us + 4000)) ++w.jumps;
      last_game_us = us;
    }
    if (now - window_ns < 10000000000ll) continue;
    window_ns = now;
    if (!REXCVAR_GET(ps5_pacing_log)) {
      w = {};
      continue;
    }
    const uint32_t shown_total = w.shown[0] + w.shown[1] + w.shown[2] + w.shown[3];
    const uint32_t game_total = w.game[0] + w.game[1] + w.game[2] + w.game[3];
    REXLOG_INFO(
        "ps5 pacing: the TV showed {} frames: {} for 1 vblank, {} for 2, {} for 3, {} for 4 or more ({} times "
        "held for a different number than the frame before; {:.1f} to {:.1f} ms; {} gone between looks); the "
        "game's {} frames: {} to 17.5 ms, {} to 25, {} to 34.5, {} longer, {} jumps of 4 ms or more; {} "
        "images queued, not shown yet; {} Hz{}, frame limit {}, frames {}, texture watch {}, log {}; version {}",
        shown_total, w.shown[0], w.shown[1], w.shown[2], w.shown[3], w.changes, w.shown_min / 1e6,
        w.shown_max / 1e6, w.unseen, game_total, w.game[0], w.game[1], w.game[2], w.game[3], w.jumps,
        flip > 0 ? int64_t(g_images_sent.load(std::memory_order_acquire)) - flip : -1,
        g_high.load() ? "119.88" : "59.94", g_vrr.load() ? " VRR" : "", REXCVAR_GET(gta4_frame_limit),
        REXCVAR_GET(ps5_overlay_every_vblank) ? "every vblank" : "as finished",
        REXCVAR_GET(gta4_native_texture_watch) ? "on" : "off", REXCVAR_GET(log_level), GTA4PS5_VERSION);
    w = {};
  }
}

}  // namespace

bool HighRefresh() { return g_high.load(std::memory_order_acquire); }
bool Vrr() { return g_vrr.load(std::memory_order_acquire); }
bool Ready() { return g_handle.load(std::memory_order_acquire) >= 0; }

bool SetHighRefresh(bool high) {
  std::lock_guard lock(g_switch_mutex);
  const int32_t handle = g_handle.load(std::memory_order_acquire);
  if (handle < 0) {
    REXLOG_WARN("PS5 display: no TV open yet; {} Hz not switched", high ? 120 : 60);
    return false;
  }
  if (g_high.load(std::memory_order_acquire) == high) return true;
  if (!high) {
    const int result = __real_sceVideoOutConfigureOutput(handle, kRestoreMode, nullptr, nullptr, nullptr);
    g_high.store(false, std::memory_order_release);
    g_vrr.store(false, std::memory_order_release);
    REXLOG_INFO("PS5 display: back to 60 Hz ({})", result);
    Apply("60 Hz");
    return true;
  }
  const int supported = __real_sceVideoOutIsOutputSupported(handle, kHighFrameRateMode, nullptr, nullptr, nullptr);
  if (supported <= 0) {
    REXLOG_INFO("PS5 display: 120 Hz not offered on the PS5's video output ({}); staying at 60 Hz", supported);
    return false;
  }
  const int configured = __real_sceVideoOutConfigureOutput(handle, kHighFrameRateMode, nullptr, nullptr, nullptr);
  if (configured != 0) {
    REXLOG_INFO("PS5 display: 120 Hz refused ({:08X}); staying at 60 Hz", uint32_t(configured));
    return false;
  }
  std::array<int64_t, 8> intervals{};
  const bool measured = VblankIntervals(handle, intervals);
  const int64_t shortest = intervals.front(), median = intervals[intervals.size() / 2];
  const bool fixed_120 = measured && median <= kHighRefreshLimitNs;
  const bool faster_than_60 = measured && shortest < kFixed60MinNs;
  const bool vrr_floor = measured && median > kFixed60MaxNs;
  if (!fixed_120 && !faster_than_60 && !vrr_floor) {
    __real_sceVideoOutConfigureOutput(handle, kRestoreMode, nullptr, nullptr, nullptr);
    REXLOG_INFO("PS5 display: 120 Hz taken, but a vblank comes every {:.3f} ms (shortest {:.3f}); back to 60 Hz",
                median / 1e6, shortest / 1e6);
    return false;
  }
  g_vrr.store(!fixed_120, std::memory_order_release);
  g_high.store(true, std::memory_order_release);
  REXLOG_INFO("PS5 display: 120 Hz on{} (vblanks {:.3f} to {:.3f} ms apart, median {:.3f})",
              fixed_120 ? "" : " as VRR", shortest / 1e6, intervals.back() / 1e6, median / 1e6);
  Apply("120 Hz");
  return true;
}

void StartGamePacing() {
  g_game.store(true, std::memory_order_release);
  rex::frame_stats::g_display_pacing_changed.store(+[](const char* why) { Apply(why); },
                                                   std::memory_order_release);
  static std::atomic<bool> registered{false};
  if (!registered.exchange(true)) {
    rex::cvar::RegisterChangeCallback("gta4_frame_limit",
                                      [](std::string_view, std::string_view) { Apply("frame limit changed"); });
    std::thread(PacingMain).detach();
  }
  Apply("game started");
}

}  // namespace gta4ps5::display

using namespace gta4ps5::display;

extern "C" int __wrap_sceVideoOutIsOutputSupported(int32_t handle, uint32_t mode, const void* reserved1,
                                                   const void* reserved2, const void* reserved3) {
  if (handle >= 0) g_handle.store(handle, std::memory_order_release);
  if (mode == kHighFrameRateMode && !WantsHighRefresh()) {
    REXLOG_INFO("PS5 display: 60 Hz (the hub's Refresh Rate)");
    return 0;
  }
  const int result = __real_sceVideoOutIsOutputSupported(handle, mode, reserved1, reserved2, reserved3);
  if (mode == kHighFrameRateMode)
    REXLOG_INFO("PS5 display: 120 Hz asked for; the TV {} it ({})", result > 0 ? "takes" : "doesn't take", result);
  return result;
}

extern "C" int __wrap_sceVideoOutConfigureOutput(int32_t handle, uint32_t mode, const void* reserved1,
                                                 const void* reserved2, const void* reserved3) {
  if (handle >= 0) g_handle.store(handle, std::memory_order_release);
  const int result = __real_sceVideoOutConfigureOutput(handle, mode, reserved1, reserved2, reserved3);
  if (mode == kHighFrameRateMode) {
    g_high.store(result == 0, std::memory_order_release);
    REXLOG_INFO("PS5 display: 120 Hz mode {} ({})", result == 0 ? "on" : "refused", result);
  } else if (mode == kRestoreMode && g_high.exchange(false)) {
    g_vrr.store(false, std::memory_order_release);
    // (RADV measured the vblanks and found the TV still at 60 Hz)
    REXLOG_INFO("PS5 display: the TV stayed at 60 Hz; 120 Hz mode off");
  }
  return result;
}

extern "C" int __wrap_sceVideoOutSetFlipRate(int32_t handle, int32_t rate) {
  g_handle.store(handle, std::memory_order_release);
  const int32_t wanted = WantedRate();
  const int result = __real_sceVideoOutSetFlipRate(handle, wanted);
  g_rate.store(result == 0 ? wanted : -1, std::memory_order_release);
  (void)rate;
  // (RADV doesn't ask for 120 Hz with our param.json: the hub switches at its
  // first frame, ps5_display.h)
  return result;
}

#endif  // REX_PLATFORM_PS5
