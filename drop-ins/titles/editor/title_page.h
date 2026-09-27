#pragma once

// The editor's Title inspector page (doc 16, T4; IP5): the selected title
// clip's fields, one entry each, so one lower-third file serves every
// guest. Typing edits the clip (one undo step per entry visit); an entry
// left at the title's default keeps following the title.

#include "app/shell_host.h"

namespace ustudio::titles {

void addTitlePage(app::ShellHost &host);

} // namespace ustudio::titles
