#pragma once

namespace starfish::platform::win {

// Session 78: when, from the process's creation, each visible step of the
// startup happened -- the splash must be on screen before anything of the
// desktop changes. Off until enable() (the real launch); then each mark is one
// line of %LOCALAPPDATA%\StarFish\startup.log (rewritten every launch).
namespace StartupTimeline {
void enable();
bool enabled();
// Milliseconds since the process was created.
double now();
void mark(const char* what);
// Once per launch: the first call only.
void markOnce(const char* what);
} // namespace StartupTimeline

} // namespace starfish::platform::win
