/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Lemon::Update {
	namespace fs = std::filesystem;

	struct Arguments {
		bool prepare = false;
		fs::path work;
		fs::path package;
		fs::path installation;
		fs::path executable;
		unsigned long parentPid = 0;
	};

	int run(const std::vector<fs::path> &arguments) noexcept;
	std::string pathText(const fs::path &path);
	[[noreturn]] void fail(const std::string &message);
	void writeError(const fs::path &work, const std::string &message);

} // namespace Lemon::Update
