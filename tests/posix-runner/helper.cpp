/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <csignal>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

int main(int argc, char **argv) {
	// interpreterAsWatcher passes the executable as argv[1] and its arguments as argv[2].
	if (argc < 3)
		return 2;
	const std::string prefix = argv[2];
	std::signal(SIGTERM, SIG_IGN);
	const auto temporary = prefix + ".tmp";
	const auto readyPath = prefix + ".ready";
	const auto release = prefix + ".release";
	std::ofstream ready(temporary);
	ready << getpid();
	ready.close();
	if (! ready || std::rename(temporary.c_str(), readyPath.c_str()) != 0)
		return 3;
	// The release file lets a failed test clean up even if forced termination is broken.
	while (access(release.c_str(), F_OK) != 0)
		usleep(10000);
	return 0;
}
