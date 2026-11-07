#pragma once

class QApplication;

namespace starfish::app {

// Session 93: the README's screenshots, fully synthetic -- dummy files in a
// temp folder, generic zone names, a drawn gradient for the wallpaper, fake
// system readings. Never the user's layout, desktop or running StarFish; runs
// isolated like every probe. Output: docs/images/*.png.
int runReadmeProbe(QApplication& app);

} // namespace starfish::app
