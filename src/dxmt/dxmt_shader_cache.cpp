#include "dxmt_shader_cache.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "log/log.hpp"
#include <atomic>

namespace dxmt {

ShaderCache &
ShaderCache::getInstance(WMTMetalVersion version) {
  /* ml724: two fixed slots instead of a hash map.
   *
   * This function used a function-local static std::unordered_map keyed by Metal version.
   * Marvel Cosmic Invasion faulted inside it at getInstance+0x174:
   *
   *   adrp x12, <caches>            ; the static
   *   ldr  x12, [x12, #0x830]       ; __bucket_list_ pointer
   *   ldr  x12, [x12, x10, lsl #3]  ; <-- NULL dereference, addr=0x0
   *
   * i.e. the bucket array pointer was null while the map otherwise looked populated.
   * Instrumentation (ml723) showed each DLL load gets its own fresh static and that
   * find/insert both succeed and read back consistently, so this is NOT the statics
   * surviving a reload incorrectly -- d3d11.dll is loaded, unloaded and reloaded at a
   * different base here, which was the obvious suspicion and is now ruled out.
   *
   * WMTMetalVersion has exactly two values, WMTMetal310 and WMTMetal320. A hash table
   * with heap-allocated buckets, a rehash path and a static destructor is a lot of
   * machinery -- and a lot of failure surface -- for a two-entry lookup that can be two
   * pointers. Function-local statics with non-trivial constructors also need a guard
   * variable and register an atexit destructor, both of which are extra state to get
   * wrong across this port's load/unload/reload cycle.
   *
   * So: two slots, no allocation, no rehash, no destructor. The mutex still serialises
   * first-use construction. This removes the failing construct rather than papering over
   * a container whose exact corruption mechanism is still unexplained -- which is stated
   * plainly because it IS still unexplained. */
  static dxmt::mutex mutex;
  static std::unique_ptr<ShaderCache> cache_310;
  static std::unique_ptr<ShaderCache> cache_320;

  std::lock_guard<dxmt::mutex> lock(mutex);
  std::unique_ptr<ShaderCache> &slot = (version == WMTMetal310) ? cache_310 : cache_320;
  if (!slot)
    slot = std::make_unique<ShaderCache>(version);
  return *slot;
}

ShaderCache::ShaderCache(WMTMetalVersion metal_version) {
  if (env::getEnvVar("DXMT_SHADER_CACHE") == "0")
    return;
  std::string path;
  if (path = env::getEnvVar("DXMT_SHADER_CACHE_PATH"); !path.empty() && path.starts_with("/")) {
    if (!path.ends_with('/'))
      path += "/";
  } else {
    path = str::format("dxmt/", env::getExeName(), "/");
  }
  path += str::format("shaders_", (unsigned int)metal_version, ".db");
  scache_writer_ = WMT::CacheWriter::alloc_init(path.c_str(), kDXMTShaderCacheVersion);
  scache_reader_ = WMT::CacheReader::alloc_init(path.c_str(), kDXMTShaderCacheVersion);
}

} // namespace dxmt