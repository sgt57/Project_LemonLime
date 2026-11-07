/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>
#endif

namespace {
	namespace fs = std::filesystem;
	using namespace std::chrono_literals;

	void write(const fs::path &path, const std::string &text) {
		std::ofstream file(path, std::ios::binary);
		file.exceptions(std::ios::failbit | std::ios::badbit);
		file << text;
		file.close();
	}

	void waitFor(const fs::path &path) {
		const auto deadline = std::chrono::steady_clock::now() + 30s;
		while (! fs::exists(path)) {
			if (std::chrono::steady_clock::now() >= deadline)
				throw std::runtime_error("fixture control timeout");
			std::this_thread::sleep_for(20ms);
		}
	}
} // namespace

int main(int argc, char **argv) {
	try {
#ifdef _WIN32
		const auto *rootText = _wgetenv(L"LEMON_UPDATER_TEST_ROOT");
#else
		const auto *rootText = std::getenv("LEMON_UPDATER_TEST_ROOT");
#endif
		if (! rootText)
			return 2;
		const fs::path root(rootText);
		if (argc == 2 && std::string(argv[1]) == "--parent") {
			write(root / "parent-ready", FIXTURE_VERSION);
			waitFor(root / "exit-parent");
			return 0;
		}
		const auto work = root / "update work";
#ifdef _WIN32
		// Consume the same cleanup arguments as a restarted application.
		int wideArgc = 0;
		wchar_t **wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
		if (! wideArgv)
			throw std::runtime_error("cannot decode restart arguments");
		const bool valid = wideArgc == 5 && std::wstring(wideArgv[1]) == L"--update-cleanup" &&
		                   fs::path(wideArgv[2]) == work &&
		                   std::wstring(wideArgv[3]) == L"--update-helper-pid";
		const auto pid = valid ? std::stoul(wideArgv[4]) : 0UL;
		LocalFree(wideArgv);
		if (! valid)
			throw std::runtime_error("invalid cleanup arguments");
		const auto helper = OpenProcess(SYNCHRONIZE, FALSE, pid);
		if (! helper)
			throw std::runtime_error("cannot acquire updater process");
#else
		if (argc != 1)
			throw std::runtime_error("unexpected restart arguments");
#endif
		// Observe the backup before the successful restart commits and deletes the work directory.
		fs::copy(work / "backup", root / "observed-backup", fs::copy_options::recursive);
		const auto directory = fs::current_path().u8string();
		write(root / "restarted.tmp",
		      std::string(FIXTURE_VERSION) + "\n" + std::string(directory.begin(), directory.end()));
		fs::rename(root / "restarted.tmp", root / "restarted");
#ifdef _WIN32
		const auto status = WaitForSingleObject(helper, 15000);
		CloseHandle(helper);
		if (status != WAIT_OBJECT_0)
			throw std::runtime_error("updater did not exit before cleanup");
		fs::remove_all(work);
		write(root / "cleanup-done", "done");
#endif
		// Stay alive beyond the updater's one-second restart check, without a timing-based exit.
		waitFor(root / "stop-new");
		write(root / "new-exited", "done");
		return 0;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
