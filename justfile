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

# Format every tracked .cpp/.h in place.
fmt:
    clang-format -i $(git ls-files '*.cpp' '*.h')

# The factory-policy test alone -- the "no Qt in the process" proof.
check-qt: build
    ./{{builddir}}/tests/engine/test_factory_policy

# M7 territory; not wired yet.
flatpak:
    @echo "flatpak packaging lands in milestone M7 (docs/plans/v2/12-roadmap-and-milestones.md)"
    @exit 1
