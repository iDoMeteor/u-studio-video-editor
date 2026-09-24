#include "core/commands/timeline_edits.h"
#include "core/commands/undo_stack.h"
#include "core/model/model.h"
#include <cstdio>
using namespace ustudio::core;
int main()
{
    Model model = Model::createEmpty();
    UndoStack stack(model);
    auto add = std::make_unique<AddMarker>(100, "a");
    AddMarker *raw = add.get();
    stack.execute(std::move(add));
    MarkerId id = raw->markerId();
    // One drag: away and back to where it started.
    std::printf("move to 150: %d\n", stack.execute(std::make_unique<EditMarker>(id, 150, "a")));
    std::printf("move to 100: %d\n", stack.execute(std::make_unique<EditMarker>(id, 100, "a")));
    std::printf("undo: %d at=%lld\n", stack.undo(), static_cast<long long>(model.marker(id).at));
    std::printf("redo...\n");
    std::fflush(stdout);
    std::printf("redo: %d\n", stack.redo());
}
