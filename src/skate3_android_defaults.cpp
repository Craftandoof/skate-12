#include "skate3_android_defaults.h"
#if defined(__ANDROID__)
#include <sys/sysinfo.h>
#include <sys/system_properties.h>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <rex/cvar.h>
#include <rex/logging.h>
namespace {
struct AndroidStoreBudgets {
  uint32_t tex_mb;
  uint32_t mesh_mb;
  const char* tier;
};

uint64_t AndroidTotalRamMb() {
  struct sysinfo info = {};
  return sysinfo(&info) == 0 ? (uint64_t(info.totalram) * uint64_t(info.mem_unit)) >> 20 : 0;
}

// True where the machine is small enough that memory, not sharpness, is what
// limits it. Everything gated on this leaves larger devices exactly as they
// were.
bool AndroidIsLowEnd() { return AndroidTotalRamMb() != 0 && AndroidTotalRamMb() < 4000; }

std::string AndroidProp(const char* key) {
  char value[PROP_VALUE_MAX] = {};
  __system_property_get(key, value);
  return value;
}

std::string AndroidLower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

// The Mali-G52 class. MediaTek Helio G70, G80 and G85 (MT6768 / MT6769) pair a
// two-core Mali-G52 with only two fast CPU cores (2x Cortex-A75 plus 6x
// Cortex-A55), for example the Redmi 9. That is well below the RG406V (Mali-G57
// MC4 and four Cortex-A76 cores) the Performance profile was tuned on, so these
// devices get a more conservative set of defaults.
//
// RAM cannot identify them: a 6 GB Redmi 9 reports as a "standard" device and
// would otherwise default to the 720p High-End profile.
//
// `adb shell setprop debug.skate3.weak_gpu 1` forces the class on, for other
// Mali-G52 phones, and `0` forces it off.
bool DetectWeakMaliDevice() {
  bool weak = false;
  const char* reason = "no SoC match";
  const std::string forced = AndroidProp("debug.skate3.weak_gpu");
  if (forced == "1" || forced == "0") {
    weak = forced == "1";
    reason = "debug.skate3.weak_gpu override";
  } else {
    for (const char* key :
         {"ro.board.platform", "ro.hardware", "ro.soc.model", "ro.product.board"}) {
      const std::string value = AndroidLower(AndroidProp(key));
      if (value.find("mt6768") != std::string::npos ||
          value.find("mt6769") != std::string::npos) {
        weak = true;
        reason = key;
        break;
      }
    }
  }
  std::fprintf(stderr, "device class: weak Mali (G52, two fast cores) = %s (%s)\n",
               weak ? "yes" : "no", reason);
  std::fflush(stderr);
  return weak;
}

bool AndroidIsWeakMali() {
  static const bool weak = DetectWeakMaliDevice();
  return weak;
}

// Which cores are the fast ones.
//
// "Eight cores" on a budget phone is rarely eight of the same thing: the Tab
// A7 Lite measured here runs cpu0-3 at 2.3 GHz and cpu4-7 at 1.8 GHz, and left
// to itself the scheduler had the slow four parked at their 400 MHz minimum
// while the game struggled. Read the per-core ceiling and split on it rather
// than assuming a layout, because the arrangement differs per SoC and the
// fast cores are not always the low-numbered ones.
//
// Returns an empty string when every core has the same ceiling, which is the
// signal not to set any affinity at all.
std::vector<int> AndroidCoresByClass(bool fast) {
  std::map<uint64_t, std::vector<int>> by_ceiling;
  for (int cpu = 0; cpu < 64; ++cpu) {
    std::ifstream f("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                    "/cpufreq/cpuinfo_max_freq");
    uint64_t khz = 0;
    if (!f || !(f >> khz) || khz == 0) {
      continue;
    }
    by_ceiling[khz].push_back(cpu);
  }
  if (by_ceiling.size() < 2) {
    return {};  // one cluster, or nothing readable: leave placement alone
  }
  // "Fast" is every core that is not in the SLOWEST cluster - not the single
  // fastest cluster.
  //
  // Taking only the top group assumes a two-cluster big/little phone, and
  // current ones have three. A Galaxy S23 FE reports 4x1.79, 3x2.50 and one
  // 2.99 GHz prime core, so the top group is that one core: every
  // frame-critical thread landed on cpu7 together while three 2.5 GHz cores
  // sat idle, and the phone went from a locked 60 to visibly slow. Measured
  // after this change: 60.1 fps, p95 16.66 ms, 404% CPU across cpu4-7.
  std::vector<int> cores;
  for (auto it = by_ceiling.begin(); it != by_ceiling.end(); ++it) {
    const bool is_slowest = it == by_ceiling.begin();
    if (fast != is_slowest) {
      cores.insert(cores.end(), it->second.begin(), it->second.end());
    }
  }
  return cores;
}

std::string JoinCores(const std::vector<int>& cores) {
  std::string out;
  for (const int c : cores) {
    if (!out.empty()) {
      out += '+';
    }
    out += std::to_string(c);
  }
  return out;
}

std::string AndroidCoreList(bool fast) { return JoinCores(AndroidCoresByClass(fast)); }

// Keep the threads a frame waits on off the slow cores, and push the ones that
// only have to keep up onto them. Names must match what the threads call
// themselves, and only the first 15 characters survive (the thread layer
// truncates before matching), so every prefix here is short by construction.
//
// Only positive nice appears: an unprivileged app is refused a negative one,
// so lowering a background thread is the half of the lever that actually
// works.
std::string AndroidThreadPlacement(bool weak_mali) {
  const std::string fast = AndroidCoreList(true);
  const std::string slow = AndroidCoreList(false);
  if (fast.empty() || slow.empty()) {
    return {};
  }
  const std::string f = "cpu:" + fast;
  const std::string s = "cpu:" + slow;
  if (weak_mali) {
    // Two fast cores cannot hold the six hot threads the map below pins to
    // them, so the frame would queue behind itself. Give each of the two frame
    // producers a fast core of its own, keep streaming off them as before, and
    // leave the remaining threads (render, kernel dispatch, audio) unpinned so
    // the scheduler can place them by load. Only applied when the SoC really
    // has two fast cores; anything else keeps the original map.
    const std::vector<int> fast_cores = AndroidCoresByClass(true);
    if (fast_cores.size() == 2) {
      return "Main XThread=cpu:" + std::to_string(fast_cores.back()) +
             ";GPU Commands=cpu:" + std::to_string(fast_cores.front()) +
             ";XMA Decoder=" + s + ",nice:5;rwfilesys=" + s + ",nice:5;load_thread=" + s +
             ",nice:5;presence_thread=" + s + ",nice:10";
    }
  }
  return
      // The frame depends on these finishing.
      "Main XThread=" + f + ";GPU Commands=" + f + ";render_thread=" + f +
      ";Kernel Dispatch=" + f +
      // Audio stays fast too: starving it is audible, and it is cheap.
      ";Audio Worker=" + f + ";RwAudioCore=" + f +
      // These only have to keep up with streaming, and they are what competes
      // with the frame today.
      ";XMA Decoder=" + s + ",nice:5;rwfilesys=" + s + ",nice:5;load_thread=" + s +
      ",nice:5;presence_thread=" + s + ",nice:10";
}

AndroidStoreBudgets PickAndroidStoreBudgets() {
  const uint64_t total_mb = AndroidTotalRamMb();
  AndroidStoreBudgets b;
  if (total_mb == 0) {
    b = {288, 224, "unknown"};
  } else if (total_mb >= 5000) {
    b = {288, 224, "standard (6 GB+)"};
  } else {
    // The stores used to be clamped at 256 MB apiece, which on a 3 GB device
    // pinned half a gigabyte while the system paged gigabytes through zram.
    // The floor is 64 now, so this can ask for what the device can actually
    // spare.
    b = {128, 96, "small (under 4 GB)"};
  }
  std::fprintf(stderr, "store budgets: %s tier (%llu MB RAM) -> tex %u MB, mesh %u MB\n", b.tier,
               (unsigned long long)total_mb, b.tex_mb, b.mesh_mb);
  std::fflush(stderr);
  return b;
}

}  // namespace
#endif

void ApplySkate3AndroidDefaults() {
#if defined(__ANDROID__)
  // Adapted from Andrew Nakas's Android bootstrap. Saved settings load AFTER
  // this function so the settings menu remains authoritative in Buku's shell.
  constexpr std::pair<std::string_view, std::string_view> defaults[] = {
    {"skate3_native_render_scene_handheld_potato", "false"},
    {"skate3_native_render_scene", "true"},
    {"skate3_native_render_scene_boot_native", "true"},
    {"skate3_native_render_scene_fmv_native", "true"},
    {"skate3_native_render_scene_menu_sync_compilation", "true"},
    {"skate3_native_render_scene_hdr", "true"},
    {"skate3_guest_fps_cap", "60"},
    {"skate3_guest_fps_cap_auto", "false"},
    {"vsync", "true"},
    {"log_flush_interval", "1"},
    {"log_flush_level", "warn"},
    {"vulkan_spirv_optimize", "true"},
    {"skate3_instance_free_defer_ms", "250"},
    {"texture_cache_memory_limit_soft", "256"},
    {"texture_cache_memory_limit_hard", "384"},
    {"texture_cache_memory_limit_soft_lifetime", "30"},
    {"texture_cache_memory_limit_render_to_texture", "24"},
    {"native_render_suppress_mode", "1"},
    {"resolution_scale", "1"},
    {"draw_resolution_scale_x", "1"},
    {"draw_resolution_scale_y", "1"},
    {"skate3_native_render_scene_msaa", "1"},
    {"skate3_native_render_scene_shadow_static_size", "1024"},
    {"skate3_native_render_scene_shadow_pcss", "false"},
    {"skate3_native_render_scene_ssao", "false"},
    {"skate3_native_render_scene_bloom", "false"},
    {"skate3_native_render_scene_shafts", "false"},
    {"skate3_draw_distance_scale", "1.0"},
    {"skate3_lod_distance_scale", "1.0"},
    {"gpu_wait_reg_mem_timeout_ms", "20"},
    {"gpu_idle_spin_iterations", "32"},
    {"rtl_critical_section_max_spin", "256"},
    {"audio_device_channels", "2"},
    {"audio_device_sample_frames", "512"},
    {"presenter_present_cadence_log", "false"},
    {"vulkan_present_timing_log", "false"},
    {"skate3_native_render_scene_perf_log", "false"},
  };
  for (const auto& [name, value] : defaults) {
    if (!rex::cvar::SetFlagByName(name, value)) {
      REXLOG_ERROR("Android default could not be applied: {}={}", name, value);
    }
  }
  const bool weak_mali = AndroidIsWeakMali();
  if (AndroidIsLowEnd() || weak_mali) {
    rex::cvar::SetFlagByName("skate3_native_render_scene_tex_base_mip_px", "256");
    rex::cvar::SetFlagByName("skate3_native_render_scene_tex_base_mip2_px", "1024");
    rex::cvar::SetFlagByName("skate3_native_render_guest_static_refresh", "2");
    rex::cvar::SetFlagByName("skate3_native_render_lw_refresh", "2");
  }
  const auto budgets = PickAndroidStoreBudgets();
  rex::cvar::SetFlagByName("skate3_native_render_scene_tex_store_mb", std::to_string(budgets.tex_mb));
  rex::cvar::SetFlagByName("skate3_native_render_scene_mesh_store_mb", std::to_string(budgets.mesh_mb));
  rex::cvar::SetFlagByName("android_thread_placement_map", AndroidThreadPlacement(weak_mali));
  rex::cvar::SetFlagByName("skate3_android_quality_profile",
                           (AndroidIsLowEnd() || weak_mali) ? "0" : "1");
  if (weak_mali) {
    // Redmi 9 / Mali-G52 / Android 12 class (docs/REDMI9_ANDROID12.md). Saved
    // settings still load after this, so the in-game menu stays authoritative.
    //
    // 30 FPS: a steady 30 beats an uneven 40-50 on two fast cores, and the
    // guest frame pacing only evens out frames it is allowed to wait on.
    rex::cvar::SetFlagByName("skate3_guest_fps_cap", "30");
    // 1024-frame audio buffer: the 512-frame default underruns when the two
    // fast cores are saturated, and a crackle costs more than 10 ms of latency.
    rex::cvar::SetFlagByName("audio_device_sample_frames", "1024");
    std::fprintf(stderr,
                 "weak Mali defaults: Performance profile, 30 FPS cap, 1024-frame audio buffer, "
                 "reduced base mips\n");
    std::fflush(stderr);
  }
#endif
}

bool Skate3AndroidIsWeakMaliDevice() {
#if defined(__ANDROID__)
  return AndroidIsWeakMali();
#else
  return false;
#endif
}

bool Skate3AndroidDebugFlag(const char* name) {
#if defined(__ANDROID__)
  return AndroidProp((std::string("debug.skate3.") + name).c_str()) == "1";
#else
  (void)name;
  return false;
#endif
}
