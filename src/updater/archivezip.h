/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <filesystem>

namespace Lemon::Update {
	void extractZip(const std::filesystem::path &package, const std::filesystem::path &payload);
}
