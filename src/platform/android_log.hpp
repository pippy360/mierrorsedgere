#pragma once

// Android: everything the game prints to stdout / stderr (std::cout, printf, ...) shows up in
// logcat under the tag "mirrorsedge". A no-op on the other platforms.

namespace me {

// Replaces file descriptors 1 and 2 with a pipe read by a thread that forwards every line to
// __android_log_write. Call once, first thing in main(). Safe to call again (does nothing).
void install_android_log_redirect();

}  // namespace me
