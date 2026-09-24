#pragma once

namespace ustudio::app::stall {

// doc 19 MT0's main-thread stall monitor. Logs, at warn level with a
// "[stall]" prefix, every main-loop iteration that ran longer than 16 ms
// (one frame at 60 Hz), naming the slowest core::trace::Scope markers that
// ran inside it: "[stall] main loop blocked 48.2 ms: execute: Move clip >
// EngineSync::rebuildAll 41.0 ms; ...".
//
// On in debug builds and whenever USTUDIO_LOG_LEVEL=debug; otherwise
// install() does nothing and the trace markers stay one atomic load each.
//
// Mechanism: it wraps the default GMainContext's poll function
// (g_main_context_get_poll_func / g_main_context_set_poll_func, GLib
// 2.88's gmain.h). The time from one poll returning to the next poll being
// entered is exactly the work one iteration did (dispatching sources,
// GTK's frame clock, idle callbacks). Main thread, once, before the loop.
void install();

bool installed();

} // namespace ustudio::app::stall
