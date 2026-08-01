#pragma once

namespace microide::platform {

// Close `fd` without blocking the caller. Negative values are ignored.
//
// This exists for one syscall in particular: closing an **inotify** descriptor.
// It looks like a pointer store, and it is not. Releasing an fsnotify group
// makes the kernel wait for an SRCU grace period before the marks can be freed,
// so `close(2)` on an inotify fd sporadically blocks for milliseconds regardless
// of how many watches it holds -- a standalone 20-line C program that creates a
// descriptor with a SINGLE watch and immediately closes it measures 0.02 ms most
// of the time and 3-6 ms on a random subset of iterations.
//
// The editor closes such descriptors on the shell thread on every project switch
// and project close (the file-index watcher and the project tree watcher each own
// one), where those milliseconds are a stall the user sees. Nothing about the
// close needs the caller: it is one integer and one syscall, with no ordering
// requirement against anything the caller does next -- the descriptor number
// cannot be recycled until the close completes, so a watcher started immediately
// afterwards can never collide with it.
//
// Ordering caveat: retired closes are not observable to the caller, so do NOT use
// this for a descriptor whose release something else waits on (a lock file, a
// pipe whose peer reads EOF as a signal, a socket a peer polls for hangup).
void RetireDescriptorAsync(int fd);

}  // namespace microide::platform
