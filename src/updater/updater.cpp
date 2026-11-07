/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "updater.h"
#include "filesystem.h"
#include "platform.h"

#include <charconv>
#include <chrono>
#include <limits>
#include <thread>

namespace Lemon::Update {

	static fs::path absoluteArgument(const fs::path &path) {
		if (! path.is_absolute())
			fail("Updater paths must be absolute: " + pathText(path));
		return path.lexically_normal();
	}

	static unsigned long parsePid(const fs::path &argument) {
		const auto text = pathText(argument);
		unsigned long pid = 0;
		const auto result = std::from_chars(text.data(), text.data() + text.size(), pid);
		if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || pid == 0)
			fail("Invalid parent process identifier.");
		return pid;
	}

	static Arguments parseArguments(const std::vector<fs::path> &input) {
		if (input.size() != 6)
			fail("Expected prepare <work> <package> <installation> <executable> or install <work> "
			     "<installation> <executable> <parentPid>.");
		Arguments arguments;
		arguments.work = absoluteArgument(input[2]);
		if (input[1] == "prepare") {
			arguments.prepare = true;
			arguments.package = absoluteArgument(input[3]);
			arguments.installation = absoluteArgument(input[4]);
			arguments.executable = absoluteArgument(input[5]);
		} else if (input[1] == "install") {
			arguments.installation = absoluteArgument(input[3]);
			arguments.executable = absoluteArgument(input[4]);
			arguments.parentPid = parsePid(input[5]);
		} else {
			fail("Unknown updater command: " + pathText(input[1]));
		}
		return arguments;
	}

	static void validateArguments(const Arguments &arguments) {
		requireRealDirectory(arguments.work);
		requireSafeParents(arguments.work);
		requireRealDirectory(arguments.installation);
		requireSafeParents(arguments.installation);
		if (isWithin(arguments.work, arguments.installation) ||
		    isWithin(arguments.installation, arguments.work))
			fail("The update work directory must be separate from the installation.");
		if (! isWithin(arguments.executable, arguments.installation) ||
		    arguments.executable == arguments.installation)
			fail("The application executable is outside the installation.");
		requireSafeParents(arguments.executable.parent_path());
		requireRegularFile(arguments.executable);
	}

	static void validateStagedExecutable(const Arguments &arguments) {
		const auto executable =
		    arguments.work / "payload" / arguments.executable.lexically_relative(arguments.installation);
		requireSafeParents(executable.parent_path());
		requireRegularFile(executable);
		if (fs::file_size(executable) == 0)
			fail("The staged application executable is empty.");
#ifndef _WIN32
		const auto permissions = fs::status(executable).permissions();
		if ((permissions & fs::perms::owner_exec) == fs::perms::none)
			fail("The staged application executable is not executable.");
#endif
	}

	static void prepareUpdate(const Arguments &arguments) {
		requireRegularFile(arguments.package);
		const auto payload = arguments.work / "payload";
		createDirectory(payload);
		stagePackage(arguments, payload);
		validatePayload(payload);
		validateStagedExecutable(arguments);
		probeDirectory(arguments.installation);
	}

	static bool waitForParent(ParentProcess &process, const fs::path &work) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
		while (std::chrono::steady_clock::now() < deadline) {
			if (present(work / "cancel"))
				return false;
			if (process.exited())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		fail("The application did not exit within five minutes; installation was not attempted.");
		return false;
	}

	void writeError(const fs::path &work, const std::string &message) {
		requireSafeParents(work);
		requireRealDirectory(work);
		writeText(work / "error.log", message + "\n");
	}

	static std::string describeException(const std::exception &exception) {
		const auto *filesystem = dynamic_cast<const fs::filesystem_error *>(&exception);
		if (! filesystem)
			return exception.what();
		return "Filesystem operation failed: " + pathText(filesystem->path1()) + " " +
		       pathText(filesystem->path2()) + ": " + filesystem->code().message();
	}

	static void recover(const Arguments &arguments, Transaction &transaction, bool parentExited,
	                    const std::exception &exception) {
		auto message = describeException(exception);
		const auto rollbackErrors = transaction.rollback();
		message += "\nRecovery files: " + pathText(arguments.work) + rollbackErrors;
		writeError(arguments.work, message);
		if (! parentExited || ! rollbackErrors.empty()) {
			showError(message);
			return;
		}
		try {
			launchApplication(arguments, true);
		} catch (const std::exception &error) {
			message += "\nCannot restart the original application: " + describeException(error);
			writeError(arguments.work, message);
			showError(message);
		}
	}

	static int installUpdate(const Arguments &arguments) {
		Transaction transaction(arguments);
		bool parentExited = false;
		try {
			validatePayload(arguments.work / "payload");
			validateStagedExecutable(arguments);
			auto parent = acquireParent(arguments.parentPid, arguments.executable);
			writeText(arguments.work / "ready", "ready");
			if (! waitForParent(*parent, arguments.work))
				return 0;
			parentExited = true;
			if (present(arguments.work / "cancel"))
				return 0;
			transaction.prepare();
			if (present(arguments.work / "cancel"))
				return 0;
			transaction.apply();
			launchApplication(arguments, false);
		} catch (const std::exception &exception) {
			recover(arguments, transaction, parentExited, exception);
			return 1;
		}
#ifndef _WIN32
		// The successful restart is the commit point. Cleanup must not roll back live application files.
		fs::remove_all(arguments.work);
#endif
		return 0;
	}

	int run(const std::vector<fs::path> &input) noexcept { // NOLINT(bugprone-exception-escape)
		fs::path work;
		try {
			if (input.size() > 2)
				work = input[2];
			const auto arguments = parseArguments(input);
			work = arguments.work;
			validateArguments(arguments);
			if (! arguments.prepare)
				return installUpdate(arguments);
			prepareUpdate(arguments);
			return 0;
		} catch (const std::exception &error) {
			auto message = describeException(error);
			try {
				writeError(work, message);
			} catch (const std::exception &logError) {
				message += "\nCannot write error.log: " + describeException(logError);
			}
			showError(message);
			return 1;
		}
	}

} // namespace Lemon::Update
