/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "filesystem.h"
#include "platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <system_error>

namespace Lemon::Update {

	std::string pathText(const fs::path &path) {
		const auto text = path.u8string();
		return {reinterpret_cast<const char *>(text.data()), text.size()};
	}

	void fail(const std::string &message) { throw std::runtime_error(message); }

	bool isWithin(const fs::path &path, const fs::path &root) {
		auto component = path.begin();
		for (const auto &parent : root) {
			if (component == path.end() || *component != parent)
				return false;
			++component;
		}
		return true;
	}

	bool present(const fs::path &path) {
		std::error_code error;
		const auto status = fs::symlink_status(path, error);
		if (error && error != std::errc::no_such_file_or_directory)
			throw fs::filesystem_error("Cannot inspect path", path, error);
		return status.type() != fs::file_type::not_found;
	}

	void requireRealDirectory(const fs::path &path) {
		if (fs::symlink_status(path).type() != fs::file_type::directory)
			fail("Not a real directory: " + pathText(path));
		checkNativePath(path);
	}

	void requireSafeParents(const fs::path &path) {
		for (auto current = path; ! current.empty(); current = current.parent_path()) {
			if (present(current))
				requireRealDirectory(current);
			if (current == current.root_path())
				break;
		}
	}

	void requireRegularFile(const fs::path &path) {
		if (fs::symlink_status(path).type() != fs::file_type::regular)
			fail("Not a regular file: " + pathText(path));
		checkNativePath(path);
	}

	static void validatePayloadEntry(const fs::path &entry, const fs::path &payload) {
		const auto status = fs::symlink_status(entry);
		if (fs::is_symlink(status)) {
			const auto resolved = fs::canonical(entry);
			if (! isWithin(resolved, payload))
				fail("Payload symbolic link escapes its root: " + pathText(entry));
			return;
		}
		if (! fs::is_directory(status) && ! fs::is_regular_file(status))
			fail("Unsupported payload file: " + pathText(entry));
		checkNativePath(entry);
	}

	void validatePayload(const fs::path &payload) {
		requireRealDirectory(payload);
		if (fs::is_empty(payload))
			fail("The update payload is empty.");
		for (const auto &entry : fs::recursive_directory_iterator(payload))
			validatePayloadEntry(entry.path(), payload);
	}

	void createDirectory(const fs::path &path) {
		if (! fs::create_directory(path))
			fail("Directory already exists: " + pathText(path));
	}

	void copyTree(const fs::path &source, const fs::path &destination) {
		validatePayload(source);
		std::vector<fs::path> directories;
		for (const auto &entry : fs::recursive_directory_iterator(source)) {
			const auto relative = entry.path().lexically_relative(source);
			if (fs::is_directory(entry.symlink_status())) {
				createDirectory(destination / relative);
				directories.push_back(relative);
			} else {
				copyLeaf(entry.path(), destination / relative);
			}
		}
		for (auto directory = directories.rbegin(); directory != directories.rend(); ++directory)
			copyDirectoryMetadata(source / *directory, destination / *directory);
		copyDirectoryMetadata(source, destination);
	}

	void writeText(const fs::path &path, std::string_view text) {
		if (present(path))
			requireRegularFile(path);
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output.exceptions(std::ios::failbit | std::ios::badbit);
		output.write(text.data(), static_cast<std::streamsize>(text.size()));
		output.close();
	}

	fs::path makeTemporaryDirectory(const fs::path &parent) {
		static std::atomic<unsigned long> counter{0};
		const auto clock = std::chrono::steady_clock::now().time_since_epoch().count();
		const auto name = ".lemon-update-" + std::to_string(currentPid()) + "-" + std::to_string(clock) + "-" +
		                  std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
		const auto path = parent / name;
		createDirectory(path);
		return path;
	}

	void probeDirectory(const fs::path &directory) {
		requireSafeParents(directory);
		const auto temporary = makeTemporaryDirectory(directory);
		fs::remove(temporary);
	}

	static void removeTemporary(const fs::path &path, const std::string &original) {
		std::error_code error;
		fs::remove_all(path, error);
		if (error)
			fail(original + "\nCannot remove temporary directory: " + pathText(path) + ": " + error.message());
	}

	void replaceLeaf(const fs::path &source, const fs::path &destination) {
		requireSafeParents(destination.parent_path());
		const auto temporary = makeTemporaryDirectory(destination.parent_path());
		try {
			copyLeaf(source, temporary / "item");
			atomicReplace(temporary / "item", destination);
		} catch (const std::exception &error) {
			removeTemporary(temporary, error.what());
			throw;
		}
		fs::remove(temporary);
	}

	Transaction::Transaction(const Arguments &arguments)
	    : arguments(arguments), payload(arguments.work / "payload"), backupDirectory(arguments.work / "backup") {}

	void Transaction::inspectDirectory(const fs::path &relative) {
		const auto target = arguments.installation / relative;
		requireSafeParents(target);
		if (present(target))
			requireRealDirectory(target);
		directories.push_back({relative, false});
	}

	void Transaction::inspectLeaf(const fs::path &relative) {
		const auto target = arguments.installation / relative;
		requireSafeParents(target.parent_path());
		const bool existed = present(target);
		if (existed) {
			const auto status = fs::symlink_status(target);
			if (! fs::is_regular_file(status) && ! fs::is_symlink(status))
				fail("Cannot replace non-file installation entry: " + pathText(target));
			checkNativePath(target);
			checkExclusiveFile(target);
		}
		leaves.push_back({relative, existed, false});
	}

	void Transaction::collect() {
		for (const auto &entry : fs::recursive_directory_iterator(payload)) {
			const auto relative = entry.path().lexically_relative(payload);
			if (fs::is_directory(entry.symlink_status()))
				inspectDirectory(relative);
			else
				inspectLeaf(relative);
		}
	}

	void Transaction::backup() {
		createDirectory(backupDirectory);
		for (const auto &leaf : leaves) {
			if (! leaf.existed)
				continue;
			const auto saved = backupDirectory / leaf.relative;
			fs::create_directories(saved.parent_path());
			copyLeaf(arguments.installation / leaf.relative, saved);
		}
	}

	void Transaction::prepare() {
		validatePayload(payload);
		requireSafeParents(arguments.installation);
		collect();
		probeDirectory(arguments.installation);
		for (const auto &leaf : leaves) {
			auto parent = (arguments.installation / leaf.relative).parent_path();
			while (! present(parent))
				parent = parent.parent_path();
			probeDirectory(parent);
		}
		backup();
	}

	void Transaction::apply() {
		for (auto &directory : directories) {
			const auto target = arguments.installation / directory.relative;
			requireSafeParents(target.parent_path());
			if (! present(target)) {
				createDirectory(target);
				directory.created = true;
			}
		}
		for (auto &leaf : leaves) {
			// Record intent first: native replacement failures may still change directory entries.
			leaf.attempted = true;
			replaceLeaf(payload / leaf.relative, arguments.installation / leaf.relative);
		}
		for (auto directory = directories.rbegin(); directory != directories.rend(); ++directory) {
			if (directory->created)
				copyDirectoryMetadata(payload / directory->relative, arguments.installation / directory->relative);
		}
	}

	void Transaction::restoreLeaf(const LeafChange &leaf) {
		if (! leaf.attempted)
			return;
		const auto target = arguments.installation / leaf.relative;
		requireSafeParents(target.parent_path());
		if (leaf.existed)
			replaceLeaf(backupDirectory / leaf.relative, target);
		else
			fs::remove(target);
	}

	void Transaction::removeDirectory(const DirectoryChange &directory) {
		if (! directory.created)
			return;
		const auto target = arguments.installation / directory.relative;
		requireSafeParents(target);
		if (! fs::remove(target))
			fail("Cannot remove rollback directory: " + pathText(target));
	}

	std::string Transaction::rollback() noexcept { // NOLINT(bugprone-exception-escape)
		std::string errors;
		for (auto leaf = leaves.rbegin(); leaf != leaves.rend(); ++leaf) {
			try {
				restoreLeaf(*leaf);
			} catch (const std::exception &error) {
				errors += "\nCannot restore " + pathText(leaf->relative) + ": " + error.what();
			}
		}
		for (auto directory = directories.rbegin(); directory != directories.rend(); ++directory) {
			try {
				removeDirectory(*directory);
			} catch (const std::exception &error) {
				errors += "\nCannot remove " + pathText(directory->relative) + ": " + error.what();
			}
		}
		return errors;
	}

}
