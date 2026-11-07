/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "updater.h"

namespace Lemon::Update {

	bool isWithin(const fs::path &path, const fs::path &root);
	bool present(const fs::path &path);
	void requireRealDirectory(const fs::path &path);
	void requireSafeParents(const fs::path &path);
	void requireRegularFile(const fs::path &path);
	void validatePayload(const fs::path &payload);
	void copyTree(const fs::path &source, const fs::path &destination);
	void createDirectory(const fs::path &path);
	void writeText(const fs::path &path, std::string_view text);
	fs::path makeTemporaryDirectory(const fs::path &parent);
	void probeDirectory(const fs::path &directory);
	void replaceLeaf(const fs::path &source, const fs::path &destination);

	struct LeafChange {
		fs::path relative;
		bool existed = false;
		bool attempted = false;
	};

	struct DirectoryChange {
		fs::path relative;
		bool created = false;
	};

	class Transaction {
	public:
		explicit Transaction(const Arguments &arguments);
		void prepare();
		void apply();
		std::string rollback() noexcept;

	private:
		void collect();
		void inspectDirectory(const fs::path &relative);
		void inspectLeaf(const fs::path &relative);
		void backup();
		void restoreLeaf(const LeafChange &leaf);
		void removeDirectory(const DirectoryChange &directory);

		const Arguments &arguments;
		fs::path payload;
		fs::path backupDirectory;
		std::vector<LeafChange> leaves;
		std::vector<DirectoryChange> directories;
	};

}
