#pragma once

// The test drop-in's shell layer (IP5's consumer), one of each host, the way
// drop-ins/effects/app will use them:
//   inspector page   the selection and how many project changes it has seen
//   action           "<name>-hello" (Ctrl+Alt+H), listed in Help; says hello
//   hints            its page and action, under "Test drop-in" in Help
//   preview overlay  an outline around the frame's middle quarter, placed
//                    with ShellHost::previewMapping()
//   timeline         a 12 px lane under each video track; a click in it is
//                    claimed and reported with the frame
//   import handler   ".ustest": a text file naming a colour; imports a
//                    75-frame colour clip through the undo stack

namespace ustudio::app {
class ShellHost;
} // namespace ustudio::app

namespace ustudio::testdropin {

void extendShell(app::ShellHost &host);

// What the shell layer saw (this binary's copy), for tests.
struct ShellLog
{
    int selectionChanges = 0, projectChanges = 0, hellos = 0, lanePresses = 0, imports = 0;
};
ShellLog &shellLog();

} // namespace ustudio::testdropin
