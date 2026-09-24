# Local dev loop -- mirrors exactly what CI runs (docs/plans/v2/11-build-test-ci-packaging.md)
# so a green `just test` locally means a green CI run.

builddir := "builddir"

# One-time (or after meson.build changes) configure.
setup:
    meson setup {{builddir}} -Dbuildtype=debug -Dtests=enabled

build:
    meson compile -C {{builddir}}

test: build
    meson test -C {{builddir}} --print-errorlogs

run: build
    ./{{builddir}}/src/app/u-studio-video-editor

# Format every tracked .cpp/.h in place (excludes vendored subprojects/).
fmt:
    clang-format -i $(git ls-files '*.cpp' '*.h' ':(exclude)subprojects/*')

# Sanitizer runs (docs/audit/2026-09-23-sanitizer-report.md), each in its own
# build dir. Both should pass clean; any report is then a real finding.
# asan: ASan + UBSan, leak check on, with tests/sanitizers/lsan.supp covering
# the leaks that aren't ours (MLT loader and repository, FFmpeg worker
# threads, SDL).
# - fast_unwind_on_malloc=0: MLT modules have no frame pointers, so the
#   default unwinder stops at the first module frame and the suppressions
#   never see the libmlt frames past it.
# - LD_PRELOAD of the FFmpeg libraries MLT's avformat module links: without
#   it, Factory::close() unloads them before the leak report, so their
#   worker-thread buffers show only "<unknown module>" frames that no
#   suppression can match. verify_asan_link_order=0 allows the preload.
#   These libraries don't define malloc, so ASan's interception still works.
asan_ffmpeg := "/lib64/libavutil.so.60 /lib64/libavcodec.so.62 /lib64/libavformat.so.62 /lib64/libswscale.so.9 /lib64/libswresample.so.6 /lib64/libx264.so.165"
asan:
    [ -d builddir-asan ] || meson setup builddir-asan -Db_sanitize=address,undefined -Db_lundef=false -Dtests=enabled
    meson compile -C builddir-asan
    LD_PRELOAD="{{asan_ffmpeg}}" \
    ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0:verify_asan_link_order=0:detect_stack_use_after_return=1:halt_on_error=1 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    LSAN_OPTIONS=suppressions={{justfile_directory()}}/tests/sanitizers/lsan.supp \
        meson test -C builddir-asan -t 6 --print-errorlogs

# tsan: ThreadSanitizer. ignore_noninstrumented_modules hides accesses from
# uninstrumented libraries (GLib's futex-based main-context lock looks like a
# race to TSan). report_thread_leaks=0 because the only thread leaks are
# threads created inside MLT modules that are unloaded before exit, and our
# own threads are std::thread, which terminate() if not joined anyway.
tsan:
    [ -d builddir-tsan ] || meson setup builddir-tsan -Db_sanitize=thread -Db_lundef=false -Dtests=enabled
    meson compile -C builddir-tsan
    TSAN_OPTIONS=ignore_noninstrumented_modules=1:report_thread_leaks=0:second_deadlock_stack=1:halt_on_error=1 \
        meson test -C builddir-tsan -t 10 --print-errorlogs

# The factory-policy test alone -- the "no Qt in the process" proof.
check-qt: build
    ./{{builddir}}/tests/engine/test_factory_policy

# M7 territory; not wired yet.
flatpak:
    @echo "flatpak packaging lands in milestone M7 (docs/plans/v2/12-roadmap-and-milestones.md)"
    @exit 1
