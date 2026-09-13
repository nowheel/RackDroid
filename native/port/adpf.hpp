#pragma once

#include <cstddef>
#include <cstdint>

/* ADPF -- Android's Dynamic Performance Framework -- in one sentence: instead
of letting the CPU governor work out what we need by watching us consume, we
declare the deadline up front and report how long the work actually took.

The governor is reactive, so it is always a beat late: by the time it notices
we needed more, the underrun has happened and been heard. It is late the other
way too, holding the cores high after we have finished. A hint session closes
both gaps -- the system raises clocks and picks big cores to MEET a deadline it
has been told about, and drops them the moment the work fits.

It fits this engine unusually well. The API describes a session as "a group of
threads with an inter-related workload", which is exactly the audio callback
plus the engine's workers: all blocked on the same barrier, all having to
finish by the same instant. And we already have both numbers -- the deadline is
the block size over the sample rate, and the duration is measured anyway to
count underruns.

Everything here is a no-op below Android 13 (the NDK marks these functions
__INTRODUCED_IN(__ANDROID_API_T__), API 33) and on any device whose system does
not offer the service. minSdk is 29, so the calls are guarded at runtime.

This does NOT replace checkThreadCount(). The two work on different things:
the tuner decides how many threads to use, ADPF asks for enough power to run
the ones chosen. They are introduced separately and on purpose -- the tuner
first, measured, then this -- so that a change in behaviour can be attributed
to one of them rather than to both at once. */
namespace rackdroid {

/** Hands the session the threads doing the work: the audio callback thread
first, then the engine's workers. Called from the render thread whenever the
worker set changes (which, since the tuner moves, is not rare). Creates the
session on the first call. Does IPC -- never call from the audio callback. */
void adpfSetThreads(const int* tids, size_t count);

/** The deadline for one callback, in nanoseconds: frames / sampleRate. Called
from the render thread when the block size or rate changes. */
void adpfSetTargetNanos(int64_t nanos);

/** How long the last callback's work actually took. Called from the audio
callback, and written to be safe there: it never blocks (it gives up rather
than wait for the render thread) and it drops reports that say nothing new,
since each one is a round trip to the system. */
void adpfReportNanos(int64_t nanos);

/** Releases the session. Render thread. */
void adpfClose();

/** True once a session exists -- i.e. the device is Android 13+ and granted
one. For the log, so a report of "no difference" can be told apart from "it was
never on". */
bool adpfActive();

} // namespace rackdroid
