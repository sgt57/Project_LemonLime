/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "updater.h"
#include "platform.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
	int count = 0;
	wchar_t **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
	if (! arguments) {
		Lemon::Update::showError("Cannot read updater arguments: " + Lemon::Update::nativeError(GetLastError()));
		return 1;
	}
	std::vector<std::filesystem::path> paths;
	try {
		paths.reserve(static_cast<std::size_t>(count));
		for (int index = 0; index < count; ++index)
			paths.emplace_back(arguments[index]);
	} catch (const std::exception &error) {
		LocalFree(static_cast<HLOCAL>(arguments));
		Lemon::Update::showError(error.what());
		return 1;
	}
	LocalFree(static_cast<HLOCAL>(arguments));
	return Lemon::Update::run(paths);
}

#else

int main(int count, char *arguments[]) {
	std::vector<std::filesystem::path> paths;
	try {
		paths.reserve(static_cast<std::size_t>(count));
		for (int index = 0; index < count; ++index)
			paths.emplace_back(arguments[index]);
	} catch (const std::exception &error) {
		Lemon::Update::showError(error.what());
		return 1;
	}
	return Lemon::Update::run(paths);
}

#endif
