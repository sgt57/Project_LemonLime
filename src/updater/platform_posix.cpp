/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef _WIN32

#include "platform.h"
#include "filesystem.h"
#include "posix.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <copyfile.h>
#endif

namespace Lemon::Update {

	std::string nativeError(unsigned long code) {
		return std::error_code(static_cast<int>(code), std::generic_category()).message();
	}

	unsigned long currentPid() noexcept { return static_cast<unsigned long>(getpid()); }

	void showError(std::string_view message) noexcept {
		std::fwrite(message.data(), 1, message.size(), stderr);
		std::fputc('\n', stderr);
#ifdef __APPLE__
		const auto text = CFStringCreateWithBytes(kCFAllocatorDefault,
		                                           reinterpret_cast<const UInt8 *>(message.data()),
		                                           static_cast<CFIndex>(message.size()), kCFStringEncodingUTF8,
		                                           false);
		if (text) {
			CFOptionFlags response = 0;
			CFUserNotificationDisplayAlert(0, kCFUserNotificationStopAlertLevel, nullptr, nullptr, nullptr,
			                               CFSTR("LemonLime update"), text, CFSTR("OK"), nullptr, nullptr, &response);
			CFRelease(text);
		}
#endif
	}

	void checkNativePath(const fs::path &path) {
		// Native POSIX directory links are rejected by requireSafeParents. Leaf links are copied, never followed.
		struct stat status{};
		if (lstat(path.c_str(), &status) != 0)
			posixFailure("Cannot inspect update path", path);
	}

	void checkExclusiveFile(const fs::path &path) {
		// POSIX rename does not require writing the old inode, nor does it follow a leaf symbolic link.
		if (access(path.parent_path().c_str(), W_OK | X_OK) != 0)
			posixFailure("Installation directory is not writable", path.parent_path());
	}

#ifndef __APPLE__
	static void writeBytes(int descriptor, const char *buffer, std::size_t length, const fs::path &path) {
		std::size_t offset = 0;
		while (offset < length) {
			const auto written = write(descriptor, buffer + offset, length - offset);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0)
				posixFailure("Cannot write update file", path);
			offset += static_cast<std::size_t>(written);
		}
	}

	static void copyRegularFile(const fs::path &source, const fs::path &destination) {
		const FileDescriptor input(open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
		if (input.get() < 0)
			posixFailure("Cannot open update file", source);
		struct stat status{};
		if (fstat(input.get(), &status) != 0)
			posixFailure("Cannot read update file metadata", source);
		if (! S_ISREG(status.st_mode))
			fail("Update source is not a regular file: " + pathText(source));
		const FileDescriptor output(open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
		                                 S_IRUSR | S_IWUSR));
		if (output.get() < 0)
			posixFailure("Cannot create update file", destination);
		std::array<char, 65536> buffer{};
		for (;;) {
			const auto count = read(input.get(), buffer.data(), buffer.size());
			if (count < 0 && errno == EINTR)
				continue;
			if (count < 0)
				posixFailure("Cannot read update file", source);
			if (count == 0)
				break;
			writeBytes(output.get(), buffer.data(), static_cast<std::size_t>(count), destination);
		}
		if (fchmod(output.get(), status.st_mode & 07777) != 0)
			posixFailure("Cannot preserve file permissions", destination);
		const timespec times[2] = {status.st_atim, status.st_mtim};
		if (futimens(output.get(), times) != 0 || fsync(output.get()) != 0)
			posixFailure("Cannot preserve or flush update file", destination);
	}
#endif

	void copyLeaf(const fs::path &source, const fs::path &destination) {
#ifdef __APPLE__
		if (copyfile(source.c_str(), destination.c_str(), nullptr,
		             COPYFILE_ALL | COPYFILE_EXCL | COPYFILE_NOFOLLOW_SRC | COPYFILE_NOFOLLOW_DST) != 0)
			posixFailure("Cannot preserve update file and metadata", destination);
#else
		if (fs::is_symlink(fs::symlink_status(source)))
			fs::create_symlink(fs::read_symlink(source), destination);
		else
			copyRegularFile(source, destination);
#endif
	}

	void copyDirectoryMetadata(const fs::path &source, const fs::path &destination) {
#ifdef __APPLE__
		if (copyfile(source.c_str(), destination.c_str(), nullptr,
		             COPYFILE_METADATA | COPYFILE_NOFOLLOW_SRC | COPYFILE_NOFOLLOW_DST) != 0)
			posixFailure("Cannot preserve directory metadata", destination);
#else
		fs::permissions(destination, fs::status(source).permissions());
		fs::last_write_time(destination, fs::last_write_time(source));
#endif
	}

	void atomicReplace(const fs::path &source, const fs::path &destination) {
		if (rename(source.c_str(), destination.c_str()) != 0)
			posixFailure("Cannot atomically replace installation file", destination);
	}

#ifndef __APPLE__
	void stagePackage(const Arguments &arguments, const fs::path &payload) {
		const auto relative = arguments.executable.lexically_relative(arguments.installation);
		fs::create_directories((payload / relative).parent_path());
		copyLeaf(arguments.package, payload / relative);
		const auto mode = fs::status(arguments.executable).permissions() & fs::perms::mask;
		fs::permissions(payload / relative, (mode & static_cast<fs::perms>(0777)) | fs::perms::owner_exec);
	}
#endif

	namespace {
		struct ChildConfiguration {
			char *const *arguments;
			const char *directory;
			int input;
			int output;
			int errorRead;
			int errorWrite;
			bool detached;
		};

		[[noreturn]] void reportChildFailure(int descriptor) noexcept {
			const int error = errno;
			const auto *bytes = reinterpret_cast<const char *>(&error);
			std::size_t sent = 0;
			while (sent < sizeof(error)) {
				const auto count = write(descriptor, bytes + sent, sizeof(error) - sent);
				if (count < 0 && errno == EINTR)
					continue;
				if (count <= 0)
					break;
				sent += static_cast<std::size_t>(count);
			}
			_exit(127);
		}

		[[noreturn]] void executeChild(const ChildConfiguration &configuration) noexcept {
			close(configuration.errorRead);
			if (configuration.detached && setsid() < 0)
				reportChildFailure(configuration.errorWrite);
			if (dup2(configuration.input, STDIN_FILENO) < 0 || dup2(configuration.output, STDOUT_FILENO) < 0 ||
			    dup2(configuration.output, STDERR_FILENO) < 0)
				reportChildFailure(configuration.errorWrite);
			if (configuration.directory && chdir(configuration.directory) != 0)
				reportChildFailure(configuration.errorWrite);
			execv(configuration.arguments[0], configuration.arguments);
			reportChildFailure(configuration.errorWrite);
		}

		int waitChild(pid_t pid, int options) {
			int status = 0;
			pid_t result;
			do {
				result = waitpid(pid, &status, options);
			} while (result < 0 && errno == EINTR);
			if (result < 0)
				posixFailure("Cannot wait for child process", {});
			return result == 0 ? -1 : status;
		}

		void checkExec(int descriptor, pid_t pid) {
			int error = 0;
			auto *bytes = reinterpret_cast<char *>(&error);
			std::size_t received = 0;
			while (received < sizeof(error)) {
				const auto count = read(descriptor, bytes + received, sizeof(error) - received);
				if (count < 0 && errno == EINTR)
					continue;
				if (count < 0)
					posixFailure("Cannot check child process startup", {});
				if (count == 0)
					break;
				received += static_cast<std::size_t>(count);
			}
			if (received != 0) {
				waitChild(pid, 0);
				fail("Cannot execute child process: " + nativeError(static_cast<unsigned long>(error)));
			}
		}

		pid_t spawn(const std::vector<std::string> &arguments, const fs::path &directory, const fs::path &log,
		            bool detached) {
			requireSafeParents(log.parent_path());
			if (present(log))
				requireRegularFile(log);
			const FileDescriptor output(open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
			                                 S_IRUSR | S_IWUSR));
			const FileDescriptor input(open("/dev/null", O_RDONLY | O_CLOEXEC));
			if (output.get() < 0 || input.get() < 0)
				posixFailure("Cannot open process logging", log);
			std::vector<char *> pointers;
			pointers.reserve(arguments.size() + 1);
			for (const auto &argument : arguments)
				pointers.push_back(const_cast<char *>(argument.c_str()));
			pointers.push_back(nullptr);
			int pipeDescriptors[2] = {-1, -1};
			if (pipe(pipeDescriptors) != 0)
				posixFailure("Cannot create process handshake pipe", {});
			const FileDescriptor errorRead(pipeDescriptors[0]);
			const FileDescriptor errorWrite(pipeDescriptors[1]);
			if (fcntl(errorRead.get(), F_SETFD, FD_CLOEXEC) < 0 ||
			    fcntl(errorWrite.get(), F_SETFD, FD_CLOEXEC) < 0)
				posixFailure("Cannot secure process handshake pipe", {});
			const ChildConfiguration configuration{pointers.data(), directory.empty() ? nullptr : directory.c_str(),
			                                       input.get(), output.get(), errorRead.get(), errorWrite.get(),
			                                       detached};
			const auto pid = fork();
			if (pid < 0)
				posixFailure("Cannot create child process", {});
			if (pid == 0)
				executeChild(configuration);
			// Release our writer before reading, so successful exec is observable as EOF.
			close(errorWrite.get());
			checkExec(errorRead.get(), pid);
			return pid;
		}
	}

	void runSystemUtility(const std::vector<std::string> &arguments, const fs::path &log) {
		const auto pid = spawn(arguments, {}, log, false);
		const auto status = waitChild(pid, 0);
		if (! WIFEXITED(status) || WEXITSTATUS(status) != 0)
			fail("System utility failed: " + arguments.front() + "; see " + pathText(log));
	}

	void launchApplication(const Arguments &arguments, bool recovery) {
		std::vector<std::string> command{pathText(arguments.executable)};
		if (recovery) {
			command.emplace_back("--update-error");
			command.push_back(pathText(arguments.work / "error.log"));
		}
		const auto pid = spawn(command, arguments.executable.parent_path(), arguments.work / "restart.log", true);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (std::chrono::steady_clock::now() < deadline) {
			if (waitChild(pid, WNOHANG) != -1)
				fail("The restarted application exited immediately.");
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}

}

#endif
