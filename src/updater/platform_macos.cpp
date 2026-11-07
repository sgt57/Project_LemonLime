/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef __APPLE__

#include "platform.h"
#include "filesystem.h"
#include "posix.h"

#include <limits>
#include <libproc.h>
#include <optional>
#include <sys/event.h>
#include <sys/sysctl.h>
#include <sys/time.h>

namespace Lemon::Update {

	namespace {
		std::optional<timeval> processStartTime(pid_t pid) {
			int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
			kinfo_proc process{};
			std::size_t size = sizeof(process);
			if (sysctl(mib, 4, &process, &size, nullptr, 0) != 0)
				posixFailure("Cannot inspect the parent process", {});
			if (size == 0)
				return std::nullopt;
			return process.kp_proc.p_starttime;
		}

		bool sameStartTime(const std::optional<timeval> &first, const std::optional<timeval> &second) {
			return first && second && first->tv_sec == second->tv_sec && first->tv_usec == second->tv_usec;
		}

		class MacParent final : public ParentProcess {
		public:
			MacParent(pid_t pid, const fs::path &executable) : queue(kqueue()) {
				if (queue.get() < 0)
					posixFailure("Cannot acquire a parent process event queue", {});
				const auto identity = processStartTime(pid);
				if (! identity)
					fail("The parent application is not running.");
				char image[PROC_PIDPATHINFO_MAXSIZE]{};
				if (proc_pidpath(pid, image, sizeof(image)) <= 0)
					posixFailure("Cannot identify the parent executable", executable);
				if (! fs::equivalent(fs::path(image), executable))
					fail("The supplied parent process is not the installed application.");
				struct kevent event{};
				EV_SET(&event, static_cast<uintptr_t>(pid), EVFILT_PROC, EV_ADD | EV_ENABLE | EV_ONESHOT,
				       NOTE_EXIT, 0, nullptr);
				if (kevent(queue.get(), &event, 1, nullptr, 0, nullptr) < 0)
					posixFailure("Cannot acquire the parent process", executable);
				if (! sameStartTime(identity, processStartTime(pid)))
					fail("The parent process changed before the update handshake.");
			}

			bool exited() override {
				struct kevent event{};
				const timespec timeout{};
				int result;
				do {
					result = kevent(queue.get(), nullptr, 0, &event, 1, &timeout);
				} while (result < 0 && errno == EINTR);
				if (result < 0)
					posixFailure("Cannot wait for the parent process", {});
				if (result != 0 && (event.flags & EV_ERROR) != 0)
					fail("Parent process event failed: " + nativeError(static_cast<unsigned long>(event.data)));
				return result != 0 && (event.fflags & NOTE_EXIT) != 0;
			}

		private:
			FileDescriptor queue;
		};

		fs::path findApplicationBundle(const fs::path &mount) {
			fs::path bundle;
			for (const auto &entry : fs::directory_iterator(mount)) {
				if (entry.path().extension() != ".app")
					continue;
				requireRealDirectory(entry.path());
				if (! bundle.empty())
					fail("The update disk image contains more than one application bundle.");
				bundle = entry.path();
			}
			if (bundle.empty())
				fail("The update disk image does not contain an application bundle.");
			return bundle;
		}

		void detachImage(const fs::path &mount, const fs::path &log) {
			runSystemUtility({"/usr/bin/hdiutil", "detach", pathText(mount)}, log);
			fs::remove(mount);
		}
	}

	std::unique_ptr<ParentProcess> acquireParent(unsigned long pid, const fs::path &executable) {
		if (pid > static_cast<unsigned long>(std::numeric_limits<pid_t>::max()))
			fail("Parent process identifier is out of range.");
		return std::make_unique<MacParent>(static_cast<pid_t>(pid), executable);
	}

	void stagePackage(const Arguments &arguments, const fs::path &payload) {
		const auto mount = arguments.work / "mount";
		const auto log = arguments.work / "disk-image.log";
		createDirectory(mount);
		runSystemUtility({"/usr/bin/hdiutil", "attach", "-readonly", "-nobrowse", "-noautoopen", "-mountpoint",
		                  pathText(mount), pathText(arguments.package)}, log);
		try {
			copyTree(findApplicationBundle(mount), payload);
		} catch (const std::exception &error) {
			const std::string original = error.what();
			try {
				detachImage(mount, log);
			} catch (const std::exception &detachError) {
				fail(original + "\nCannot detach update disk image: " + detachError.what());
			}
			throw;
		}
		detachImage(mount, log);
	}

}

#endif
