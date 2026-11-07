/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "updater.h"

#include <memory>
#include <string_view>

namespace Lemon::Update {

	class ParentProcess {
	  public:
		virtual ~ParentProcess() = default;
		virtual bool exited() = 0;
	};

	std::unique_ptr<ParentProcess> acquireParent(unsigned long pid, const fs::path &executable);
	void stagePackage(const Arguments &arguments, const fs::path &payload);
	void checkNativePath(const fs::path &path);
	void checkExclusiveFile(const fs::path &path);
	void copyLeaf(const fs::path &source, const fs::path &destination);
	void copyDirectoryMetadata(const fs::path &source, const fs::path &destination);
	void atomicReplace(const fs::path &source, const fs::path &destination);
	void launchApplication(const Arguments &arguments, bool recovery);
	void showError(std::string_view message) noexcept;
	std::string nativeError(unsigned long code);
	unsigned long currentPid() noexcept;

} // namespace Lemon::Update
