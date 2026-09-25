#include "factory_policy.h"

#include "core/log.h"

#include <mlt++/Mlt.h>

#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <mutex>
#include <thread>
#include <vector>

#ifndef USTUDIO_MLT_MODULE_DIR
#error "USTUDIO_MLT_MODULE_DIR must be supplied by the build (see src/engine/meson.build)"
#endif

namespace ustudio::engine {

namespace Log = ustudio::core::Log;

namespace {

namespace fs = std::filesystem;

// Denylist is filename-substring matching, e.g. "qt6" matches both
// libmltqt6.so and libmltglaxnimate-qt6.so. Overridable via
// USTUDIO_MLT_DENYLIST (colon-separated, matching MLT's own
// MLT_REPOSITORY_DENY convention) for debugging.
std::vector<std::string> denylist()
{
    if (const char *override = std::getenv("USTUDIO_MLT_DENYLIST"); override && *override) {
        std::vector<std::string> result;
        std::string value = override;
        size_t start = 0;
        while (start <= value.size()) {
            size_t colon = value.find(':', start);
            if (colon == std::string::npos) {
                result.push_back(value.substr(start));
                break;
            }
            result.push_back(value.substr(start, colon - start));
            start = colon + 1;
        }
        return result;
    }
    return {"qt6", "glaxnimate-qt6"};
}

bool isDenied(const std::string &filename, const std::vector<std::string> &deny)
{
    for (const auto &pattern : deny) {
        if (!pattern.empty() && filename.find(pattern) != std::string::npos)
            return true;
    }
    return false;
}

// XDG_RUNTIME_DIR is only set inside a logind session; CI containers and
// other headless environments don't have one. Fall back to the process's
// system temp directory (std::filesystem::temp_directory_path(), which
// itself honours TMPDIR) rather than degrading to unsafe default MLT
// init -- ADR-007's whole point is that Qt6 never loads, and a temp-dir
// location is just as valid a place for the curated symlink farm.
fs::path moduleCacheBaseDir()
{
    if (const char *runtimeDir = std::getenv("XDG_RUNTIME_DIR"); runtimeDir && *runtimeDir)
        return fs::path(runtimeDir);

    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec) {
        Log::warn("[engine] FactoryPolicy: XDG_RUNTIME_DIR not set and no system temp directory found (" +
                  ec.message() + "), falling back to default MLT module loading");
        return {};
    }

    Log::info("[engine] FactoryPolicy: XDG_RUNTIME_DIR not set, using " + tmp.string() + " instead");
    return tmp;
}

// Builds (or refreshes) a curated module directory containing symlinks to
// every module in USTUDIO_MLT_MODULE_DIR except the denylist. Returns the
// curated directory path, or empty on any failure (caller falls back to
// default init).
std::string buildCuratedModuleDir()
{
    fs::path base = moduleCacheBaseDir();
    if (base.empty())
        return {};

    // Not a predictable per-PID path: this directory is what gets
    // dlopen()'d into the process (that's the entire point of it), so a
    // guessable, world-writable location under /tmp (the XDG_RUNTIME_DIR-
    // unset fallback -- headless/CI, not a normal desktop session with a
    // per-user 0700 runtime dir) would let another local user pre-create
    // or race-replace an entry before we symlink into it, getting their
    // code loaded into this process. mkdtemp() creates the directory
    // atomically with a random suffix and mode 0700, closing that off;
    // two processes racing to build a curated dir at the same moment
    // (reproduced by two engine test binaries in the same meson test run
    // with no XDG_RUNTIME_DIR set) now always get two distinct
    // directories instead of contending for one. FactoryPolicy's
    // destructor removes this directory, so it doesn't accumulate across
    // clean runs; a stray one from a crash is a rare, low-cost leftover
    // rather than a stable name anything can plan around.
    std::string curatedTemplate = (base / "ustudio-mlt-modules-XXXXXX").string();
    std::vector<char> curatedBuf(curatedTemplate.begin(), curatedTemplate.end());
    curatedBuf.push_back('\0');
    if (!mkdtemp(curatedBuf.data())) {
        Log::warn(std::string("[engine] FactoryPolicy: could not create a curated module directory under ") +
                  base.string() + " (" + std::strerror(errno) + "), falling back to default MLT module loading");
        return {};
    }
    fs::path curated(curatedBuf.data());
    fs::path source = USTUDIO_MLT_MODULE_DIR;

    std::error_code ec;

    if (!fs::exists(source, ec) || ec) {
        Log::warn("[engine] FactoryPolicy: MLT module source dir " + source.string() +
                  " not found, falling back to default MLT module loading");
        return {};
    }

    std::vector<std::string> deny = denylist();
    int linked = 0;
    int skipped = 0;
    // Separate error_code from the one used per-symlink below: sharing one
    // meant a single failed symlink (a dangling entry, a permissions
    // quirk) left it non-clear after the loop, which this check then
    // misread as "the whole directory scan failed" -- discarding an
    // otherwise-successfully-curated directory and falling back to
    // default (Qt-loading) MLT init over one bad entry, exactly the
    // outcome ADR-007 exists to prevent.
    std::error_code scanEc;
    for (const auto &entry : fs::directory_iterator(source, scanEc)) {
        std::string name = entry.path().filename().string();
        if (isDenied(name, deny)) {
            ++skipped;
            continue;
        }
        std::error_code symlinkEc;
        fs::create_symlink(entry.path(), curated / name, symlinkEc);
        if (symlinkEc) {
            Log::warn("[engine] FactoryPolicy: could not symlink " + name + ": " + symlinkEc.message());
            continue;
        }
        ++linked;
    }
    if (scanEc) {
        Log::warn("[engine] FactoryPolicy: error scanning " + source.string() + " (" + scanEc.message() +
                  "), falling back to default MLT module loading");
        return {};
    }

    Log::info("[engine] FactoryPolicy: curated MLT module dir " + curated.string() + " (" + std::to_string(linked) +
              " linked, " + std::to_string(skipped) + " denied)");
    return curated.string();
}

// MLT initialises some module state lazily, on the first producer that needs
// it, with no lock: the loader's extension dictionary and its normalizer list
// (static pointers checked and loaded in producer_loader.c) and avformat's
// one-time init (factory.c's avformat_initialised). Two threads creating
// their first producers at once -- parallel import probes (doc 19 MT1), or
// thumbnail and waveform workers -- race on them; reproduced as a SIGSEGV in
// attach_normalizers() in about 1 run in 7 of tests/app/test_import_queue
// before this warm-up. Walking each path once here, on the thread that
// called Factory::init, leaves them read-only afterwards.
void warmUpLazyModuleState()
{
    Mlt::Profile profile;
    // A bare path takes the loader's dictionary lookup, which picks avformat
    // for .mp4; the file doesn't exist, so nothing is opened.
    Mlt::Producer missing(profile, "loader", "/nonexistent/ustudio-mlt-warmup.mp4");
    // A producer that opens attaches the loader's normalizers.
    Mlt::Producer colour(profile, "loader", "color:black");
}

} // namespace

FactoryPolicy::FactoryPolicy()
{
#ifdef __GLIBC__
    // Process-wide, so it runs here: FactoryPolicy is constructed once, on
    // the main thread, before any other thread exists (main.cpp; the tests'
    // shared instance), and before MLT allocates anything.
    //
    // MLT's "mix" transition embeds two 192,000-sample x 6-channel float
    // buffers: a 9.2 MB struct from calloc() (transition_mix.c:37-38, :450,
    // v7.40.0), one per track and one per dissolve. glibc serves that from fresh, already-zero
    // mmap pages -- until its dynamic mmap threshold climbs past 9.2 MB,
    // which it does as soon as such a block is freed (the old graph, on the
    // first edit). From then on every one comes from the heap and calloc
    // zeroes all of it: measured on a 5,000-clip project with 1,992
    // dissolves, rebuilds went 0.35 s -> 5.7 s and resident memory to 36 GB
    // within nine rebuilds (doc 19 MT2). Setting the threshold turns the
    // dynamic adjustment off; 4 MiB keeps these on mmap: 0.3 s, 400 MB.
    // MLT's and FFmpeg's frame buffers are pooled, so the extra mmaps are
    // only for allocations that weren't reused anyway (the M2 soak, doc 19,
    // compares 4K60 playback with and without). Each mix is now its own
    // mapping, twice over while two graphs are alive during a swap: about
    // 4,000 at 2,000 dissolves, against vm.max_map_count's default 65,530.
    mallopt(M_MMAP_THRESHOLD, 4 * 1024 * 1024);
#endif
    std::string curated = buildCuratedModuleDir();
    if (!curated.empty()) {
        Mlt::Factory::init(curated.c_str());
        m_moduleDirectoryUsed = curated;
    } else {
        Log::warn("[engine] FactoryPolicy: initializing MLT with default (uncurated) module directory — Qt6 may load");
        Mlt::Factory::init();
        m_moduleDirectoryUsed.clear();
    }
    warmUpLazyModuleState();
    raiseAvformatDecoderLimit(8);
}

void FactoryPolicy::raiseAvformatDecoderLimit(size_t tracks)
{
    // MLT keeps at most N avformat producers' decoder state alive process
    // wide (mlt_cache, default 4, "producer_avformat") and evicts the least
    // recently used when another producer decodes. The cache jobs (doc 19
    // MT3) decode on several pool threads while playback decodes its own
    // masters; with more producers active than the limit they evict each
    // other's state mid-decode across threads, and playback crashed in
    // producer_get_audio -> init_cache -> mlt_properties_get (reproduced
    // 2026-09-24: a 1080p timeline playing while caches filled). kdenlive
    // sizes it the same way: threads + 2 per track (timelinemodel.cpp).
    // Set first here, on the main thread before any other thread exists,
    // which also creates MLT's lazily-made cache object; later only raised,
    // from the engine thread (EngineSync::rebuildAll()).
    static std::mutex mutex; // the engine thread and a render thread both rebuild
    static size_t current = 0;
    std::lock_guard<std::mutex> lock(mutex);
    const size_t wanted =
        std::max<size_t>(4, static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency())) + (tracks + 1) * 2);
    if (wanted <= current)
        return;
    current = std::min<size_t>(wanted, 200); // mlt_cache.c's MAX_CACHE_SIZE
    mlt_service_cache_set_size(nullptr, "producer_avformat", static_cast<int>(current));
}

FactoryPolicy::~FactoryPolicy()
{
    Mlt::Factory::close();

    // Only ever removes what buildCuratedModuleDir() itself created (a
    // per-PID directory of symlinks); empty when init fell back to the
    // default, uncurated MLT module loading.
    if (!m_moduleDirectoryUsed.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(m_moduleDirectoryUsed, ec);
    }
}

} // namespace ustudio::engine
