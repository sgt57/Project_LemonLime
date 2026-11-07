/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#ifndef _WIN32

#include "platform.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace Lemon::Update {

	class FileDescriptor {
	public:
		explicit FileDescriptor(int descriptor) : value(descriptor) {
			// Keep dup2 sources away from stdin/stdout/stderr even when the launcher closed them.
			if (value >= 0 && value < 3) {
				value = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
				const int error = errno;
				close(descriptor);
				errno = error;
			}
		}
		~FileDescriptor() { closeNow(); }
		FileDescriptor(const FileDescriptor &) = delete;
		FileDescriptor &operator=(const FileDescriptor &) = delete;
		int get() const noexcept { return value; }
		void closeNow() noexcept {
			if (value >= 0) {
				close(value);
				value = -1;
			}
		}

	private:
		int value;
	};

	[[noreturn]] inline void posixFailure(std::string_view operation, const fs::path &path) {
		const auto code = errno;
		fail(std::string(operation) + ": " + pathText(path) + ": " + nativeError(static_cast<unsigned long>(code)));
	}

	void runSystemUtility(const std::vector<std::string> &arguments, const fs::path &log);

}

#endif
