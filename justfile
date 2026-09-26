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
asan *tests:
    [ -d builddir-asan ] || meson setup builddir-asan -Db_sanitize=address,undefined -Db_lundef=false -Dtests=enabled
    meson compile -C builddir-asan
    LD_PRELOAD="{{asan_ffmpeg}}" \
    ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0:verify_asan_link_order=0:detect_stack_use_after_return=1:halt_on_error=1 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    LSAN_OPTIONS=suppressions={{justfile_directory()}}/tests/sanitizers/lsan.supp \
        meson test -C builddir-asan -t 6 --print-errorlogs {{tests}}

# Both sanitizer recipes take optional test names (`just tsan engine-thread`)
# to run only those tests; with none they run the whole suite.
#
# tsan: ThreadSanitizer. ignore_noninstrumented_modules hides accesses from
# uninstrumented libraries (GLib's futex-based main-context lock looks like a
# race to TSan). report_thread_leaks=0 because the only thread leaks are
# threads created inside MLT modules that are unloaded before exit, and our
# own threads are std::thread, which terminate() if not joined anyway.
tsan *tests:
    [ -d builddir-tsan ] || meson setup builddir-tsan -Db_sanitize=thread -Db_lundef=false -Dtests=enabled
    meson compile -C builddir-tsan
    TSAN_OPTIONS=ignore_noninstrumented_modules=1:report_thread_leaks=0:second_deadlock_stack=1:halt_on_error=1 \
        meson test -C builddir-tsan -t 10 --print-errorlogs {{tests}}

# The factory-policy test alone -- the "no Qt in the process" proof.
check-qt: build
    ./{{builddir}}/tests/engine/test_factory_policy

# The tester Flatpak (packaging/flatpak/): builds MLT, FFmpeg + x264 and
# the app against the GNOME runtime, then a single-file bundle at
# build-flatpak/u-studio-video-editor-<version>.flatpak. Needs flatpak-builder
# and the Flathub remote (the runtime and SDK install as --user). The first
# build downloads and compiles FFmpeg and MLT; later ones reuse the cache in
# build-flatpak/state. --runtime-repo embeds Flathub in the bundle, so
# `flatpak install --user ./file.flatpak` or a software centre can fetch the
# runtime.
version := `sed -n "s/^  version: '\(.*\)',$/\1/p" meson.build`
flatpak:
    flatpak-builder --user --force-clean --install-deps-from=flathub \
        --state-dir=build-flatpak/state --repo=build-flatpak/repo \
        build-flatpak/app packaging/flatpak/com.ustudio.VideoEditor.yml
    flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
        build-flatpak/repo build-flatpak/u-studio-video-editor-{{version}}.flatpak com.ustudio.VideoEditor
    @ls -lh build-flatpak/u-studio-video-editor-{{version}}.flatpak

# Drop-in configurations (ADR-013/014, doc 15 "Gating"): the full suite with
# every drop-in built in, or every one as a loadable module (each in its own
# build dir). The default build (`just test`) has them all disabled. Needs
# the drop-in folders (drop-ins/effects, drop-ins/titles) to exist.
dropins-builtin:
    [ -d builddir-dropins-builtin ] || meson setup builddir-dropins-builtin -Dtests=enabled -Ddropin_effects=builtin -Ddropin_titles=builtin
    meson test -C builddir-dropins-builtin --print-errorlogs

dropins-module:
    [ -d builddir-dropins-module ] || meson setup builddir-dropins-module -Dtests=enabled -Ddropin_effects=module -Ddropin_titles=module
    meson test -C builddir-dropins-module --print-errorlogs
