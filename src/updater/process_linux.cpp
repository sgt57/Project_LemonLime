/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#if defined(__linux__)

#include "platform.h"
#include "posix.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <poll.h>
#include <sstream>
#include <sys/syscall.h>

namespace Lemon::Update {

	namespace {
		struct ProcessIdentity {
			std::uint64_t startTime;
			bool zombie;
		};

		std::string readProcessStat(int descriptor, const fs::path &path) {
			std::string text;
			std::array<char, 4096> buffer{};
			for (;;) {
				const auto count = read(descriptor, buffer.data(), buffer.size());
				if (count < 0 && errno == EINTR)
					continue;
				if (count < 0)
					posixFailure("Cannot read parent process identity", path);
				if (count == 0)
					return text;
				text.append(buffer.data(), static_cast<std::size_t>(count));
				if (text.size() > 65536)
					fail("Parent process identity is too large.");
			}
		}

		ProcessIdentity parseIdentity(const std::string &text) {
			// comm is parenthesized and may itself contain spaces or parentheses.
			const auto end = text.rfind(')');
			if (end == std::string::npos)
				fail("Cannot parse the parent process identity.");
			std::istringstream fields(text.substr(end + 1));
			char state = '\0';
			fields >> state;
			std::string value;
			for (int field = 4; field <= 22; ++field) {
				if (! (fields >> value))
					fail("Incomplete parent process identity.");
			}
			std::uint64_t startTime = 0;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), startTime);
			if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
				fail("Invalid parent process start time.");
			return {startTime, state == 'Z' || state == 'X'};
		}

		std::optional<ProcessIdentity> processIdentity(pid_t pid) {
			const auto path = fs::path("/proc") / std::to_string(pid) / "stat";
			const FileDescriptor descriptor(open(path.c_str(), O_RDONLY | O_CLOEXEC));
			if (descriptor.get() < 0) {
				if (errno == ENOENT || errno == ESRCH)
					return std::nullopt;
				posixFailure("Cannot open parent process identity", path);
			}
			return parseIdentity(readProcessStat(descriptor.get(), path));
		}

		int openProcessDescriptor(pid_t pid) {
#ifdef SYS_pidfd_open
			const auto descriptor = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
			if (descriptor >= 0)
				return descriptor;
			if (errno != ENOSYS && errno != EINVAL && errno != EPERM && errno != EACCES)
				posixFailure("Cannot acquire the parent process", {});
#endif
			return -1;
		}

		class LinuxParent final : public ParentProcess {
		public:
			LinuxParent(pid_t pid, const fs::path &executable)
			    : pid(pid), identity(processIdentity(pid)), descriptor(openProcessDescriptor(pid)) {
				if (! identity || identity->zombie)
					fail("The parent process is not running.");
				const auto image = fs::path("/proc") / std::to_string(pid) / "exe";
				if (! fs::equivalent(image, executable))
					fail("The supplied parent process is not the installed application.");
				const auto current = processIdentity(pid);
				if (! current || current->zombie || current->startTime != identity->startTime)
					fail("The parent process changed before the update handshake.");
			}

			bool exited() override {
				if (descriptor.get() < 0) {
					const auto current = processIdentity(pid);
					return ! current || current->zombie || current->startTime != identity->startTime;
				}
				pollfd event{descriptor.get(), POLLIN, 0};
				int result;
				do {
					result = poll(&event, 1, 0);
				} while (result < 0 && errno == EINTR);
				if (result < 0 || (event.revents & (POLLERR | POLLNVAL)) != 0)
					posixFailure("Cannot wait for the parent process", {});
				return (event.revents & POLLIN) != 0;
			}

		private:
			pid_t pid;
			std::optional<ProcessIdentity> identity;
			FileDescriptor descriptor;
		};
	}

	std::unique_ptr<ParentProcess> acquireParent(unsigned long pid, const fs::path &executable) {
		if (pid > static_cast<unsigned long>(std::numeric_limits<pid_t>::max()))
			fail("Parent process identifier is out of range.");
		return std::make_unique<LinuxParent>(static_cast<pid_t>(pid), executable);
	}

}

#endif
