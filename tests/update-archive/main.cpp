/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "archivezip.h"

#include <miniz.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
	namespace fs = std::filesystem;

	void require(bool condition, std::string_view message) {
		if (! condition)
			throw std::runtime_error(std::string(message));
	}

	void little(std::string &bytes, std::uint64_t value, unsigned int count) {
		for (unsigned int index = 0; index < count; ++index) {
			bytes.push_back(static_cast<char>(value & 0xffU));
			value >>= 8U;
		}
	}

	struct Fixture {
		std::string name;
		std::string data = "native updater\n";
		std::string compressed;
		std::uint16_t method = 0;
		std::uint32_t attributes = 0100644U << 16U;
		bool corruptCrc = false;
	};

	std::uint32_t crc(const Fixture &entry) {
		auto value = static_cast<std::uint32_t>(mz_crc32(
		    MZ_CRC32_INIT, reinterpret_cast<const unsigned char *>(entry.data.data()), entry.data.size()));
		if (entry.corruptCrc)
			value ^= 0x12345678U;
		return value;
	}

	const std::string &compressed(const Fixture &entry) {
		return entry.method == 0 ? entry.data : entry.compressed;
	}

	std::string zip64Extra(const Fixture &entry, std::uint64_t offset, bool central) {
		std::string extra;
		little(extra, 1, 2);
		little(extra, central ? 24 : 16, 2);
		little(extra, entry.data.size(), 8);
		little(extra, compressed(entry).size(), 8);
		if (central)
			little(extra, offset, 8);
		return extra;
	}

	void localHeader(std::string &bytes, const Fixture &entry, bool zip64) {
		const auto extra = zip64 ? zip64Extra(entry, 0, false) : std::string{};
		little(bytes, 0x04034b50, 4);
		little(bytes, zip64 ? 45 : 20, 2);
		little(bytes, 0x800, 2);
		little(bytes, entry.method, 2);
		little(bytes, 0, 4);
		little(bytes, crc(entry), 4);
		little(bytes, zip64 ? 0xffffffffU : compressed(entry).size(), 4);
		little(bytes, zip64 ? 0xffffffffU : entry.data.size(), 4);
		little(bytes, entry.name.size(), 2);
		little(bytes, extra.size(), 2);
		bytes += entry.name;
		bytes += extra;
		bytes += compressed(entry);
	}

	void centralHeader(std::string &bytes, const Fixture &entry, std::uint64_t offset, bool zip64) {
		const auto extra = zip64 ? zip64Extra(entry, offset, true) : std::string{};
		little(bytes, 0x02014b50, 4);
		little(bytes, 0x32d, 2);
		little(bytes, zip64 ? 45 : 20, 2);
		little(bytes, 0x800, 2);
		little(bytes, entry.method, 2);
		little(bytes, 0, 4);
		little(bytes, crc(entry), 4);
		little(bytes, zip64 ? 0xffffffffU : compressed(entry).size(), 4);
		little(bytes, zip64 ? 0xffffffffU : entry.data.size(), 4);
		little(bytes, entry.name.size(), 2);
		little(bytes, extra.size(), 2);
		little(bytes, 0, 6);
		little(bytes, entry.attributes, 4);
		little(bytes, zip64 ? 0xffffffffU : offset, 4);
		bytes += entry.name;
		bytes += extra;
	}

	void endRecords(std::string &bytes, size_t count, size_t start, size_t size, bool zip64) {
		if (zip64) {
			const auto endOffset = bytes.size();
			little(bytes, 0x06064b50, 4);
			little(bytes, 44, 8);
			little(bytes, 45, 2);
			little(bytes, 45, 2);
			little(bytes, 0, 8);
			little(bytes, count, 8);
			little(bytes, count, 8);
			little(bytes, size, 8);
			little(bytes, start, 8);
			little(bytes, 0x07064b50, 4);
			little(bytes, 0, 4);
			little(bytes, endOffset, 8);
			little(bytes, 1, 4);
		}
		little(bytes, 0x06054b50, 4);
		little(bytes, 0, 4);
		little(bytes, zip64 ? 0xffff : count, 2);
		little(bytes, zip64 ? 0xffff : count, 2);
		little(bytes, zip64 ? 0xffffffffU : size, 4);
		little(bytes, zip64 ? 0xffffffffU : start, 4);
		little(bytes, 0, 2);
	}

	std::string archive(const std::vector<Fixture> &entries, bool zip64 = false) {
		std::string bytes;
		std::vector<size_t> offsets;
		for (const auto &entry : entries) {
			offsets.push_back(bytes.size());
			localHeader(bytes, entry, zip64);
		}
		const auto start = bytes.size();
		for (size_t index = 0; index < entries.size(); ++index)
			centralHeader(bytes, entries[index], offsets[index], zip64);
		endRecords(bytes, entries.size(), start, bytes.size() - start, zip64);
		return bytes;
	}

	void save(const fs::path &path, std::string_view bytes) {
		std::ofstream stream(path, std::ios::binary);
		stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		stream.close();
		require(! stream.fail(), "cannot create test fixture");
	}

	std::string load(const fs::path &path) {
		std::ifstream stream(path, std::ios::binary);
		require(stream.good(), "missing extracted file");
		return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	class Directory {
	  public:
		Directory() {
#ifdef _WIN32
			const auto pid = GetCurrentProcessId();
#else
			const auto pid = getpid();
#endif
			root = fs::temp_directory_path() / fs::path(u8"lemon-update-归档 'files") / std::to_string(pid);
			require(! fs::exists(root), "test directory unexpectedly exists");
			fs::create_directories(root);
			save(root / "outside.txt", "untouched");
		}

		~Directory() {
			std::error_code ignored;
			fs::remove_all(root, ignored);
		}

		Directory(const Directory &) = delete;
		Directory &operator=(const Directory &) = delete;

		fs::path root;
		unsigned int next = 0;

		fs::path payload() {
			auto path = root / ("payload-" + std::to_string(next++));
			fs::create_directory(path);
			return path;
		}

		void extract(std::string_view bytes, const fs::path &destination) const {
			const auto package = root / fs::path(u8"更新 ' package.zip");
			save(package, bytes);
			Lemon::Update::extractZip(package, destination);
		}

		void rejectsBytes(std::string_view bytes, bool untouched = true) {
			const auto destination = payload();
			bool rejected = false;
			try {
				extract(bytes, destination);
			} catch (const std::runtime_error &) {
				rejected = true;
			}
			require(rejected, "unsafe archive was accepted");
			if (untouched)
				require(fs::is_empty(destination), "metadata rejection wrote a payload file");
			require(load(root / "outside.txt") == "untouched", "archive escaped payload");
		}

		void rejects(const std::vector<Fixture> &entries, bool untouched = true) {
			rejectsBytes(archive(entries), untouched);
		}
	};

	Fixture directory(std::string name) {
		Fixture entry{std::move(name), ""};
		entry.attributes = (0040755U << 16U) | 0x10U;
		return entry;
	}

	void storedAndUnicode(Directory &test) {
		std::string large(180000, '\0');
		for (size_t index = 0; index < large.size(); ++index)
			large[index] = static_cast<char>(index % 251);
		const auto destination = test.payload();
		test.extract(archive({directory("release/"), {"release/lemon.exe", large},
		                      {"release/Qt6Core.dll", "runtime"}, directory("release/plugins/"),
		                      {"release/plugins/\xe4\xb8\xad\xe6\x96\x87.txt", "unicode"}}), destination);
		require(load(destination / "lemon.exe") == large, "stored stream data changed");
		require(load(destination / "Qt6Core.dll") == "runtime", "runtime was not extracted");
		require(load(destination / fs::path(u8"plugins/中文.txt")) == "unicode", "Unicode path changed");
		require(! fs::exists(destination / "release"), "package root was not stripped");
	}

	Fixture deflatedFixture() {
		// Fixed raw RFC 1951 stream (zlib -15, level 9), not a metadata-only fixture.
		constexpr char encoded[] =
		    "\xed\xca\xc1\x0d\x80\x20\x10\x00\xb0\xbf\x53\xb0\x1a\xd1\x8b\xf2\x41\x73\xa0\xf3\x1b\xe7\xb0\xcf"
		    "\x26\xed\x75\xb6\x27\xca\x7d\x6d\x75\x46\x96\x9a\xeb\xf1\x39\x63\xcf\x18\xa3\x9d\x7d\xe9\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61\x18\x86\x61"
		    "\x18\x86\x61\x18\x86\x61\x18\x86\xf1\xfb\xf1\x02";
		Fixture entry{"release/lemon.exe", ""};
		for (unsigned int index = 0; index < 4000; ++index)
			entry.data += "native updater archive regression\n";
		entry.compressed.assign(encoded, sizeof(encoded) - 1);
		entry.method = 8;
		return entry;
	}

	void deflatedAndZip64(Directory &test) {
		const auto entry = deflatedFixture();
		for (const bool zip64 : {false, true}) {
			const auto destination = test.payload();
			test.extract(archive({entry}, zip64), destination);
			require(load(destination / "lemon.exe") == entry.data, "deflated stream data changed");
		}
		auto corrupt = entry;
		corrupt.corruptCrc = true;
		test.rejects({corrupt}, false);
		auto truncated = entry;
		truncated.compressed.resize(truncated.compressed.size() / 2);
		test.rejects({truncated}, false);
	}

	void badNames(Directory &test) {
		const std::vector<std::string> names{
		    "../outside.txt", "release/../outside.txt", "release/a/../../outside.txt", "/release/outside.txt",
		    "C:/outside.txt", "release/C:outside.txt", "release/a:stream", "release/\\outside.txt",
		    "release/a\\..\\outside.txt", "release//outside.txt", "release/./outside.txt", "release/name.",
		    "release/name ", "release/CON", "release/aux.txt", "release/NUL.txt", "release/COM1.log",
		    "release/CON .txt", "release/COM1 .log", "release/AUX .txt",
		    "release/LPT9", "release/COM\xc2\xb9.txt", "release/CONIN$", "release/a?b", "release/a*b",
		    "release/a\"b", "release/a<b", "release/a>b", "release/a|b", "release/\x01", "release/\xc0\xaf",
		    "release/\xed\xa0\x80", "release/\xf4\x90\x80\x80", "release/\xe4\xb8", "lemon.exe",
		    "release/" + std::string(256, 'a'), std::string("release/a\0b", 11)};
		for (const auto &name : names)
			test.rejects({{name}});
		test.rejects({directory("release//")});
		std::string longName = "release/";
		for (unsigned int index = 0; index < 6; ++index)
			longName += std::string(100, 'a') + "/";
		test.rejects({{longName + "../outside.txt"}});
		test.rejects({{longName + "CON.txt"}});
	}

	void conflictingEntries(Directory &test) {
		test.rejects({{"release/lemon.exe"}, {"other/Qt6Core.dll"}});
		test.rejects({{"release/lemon.exe"}, {"release/lemon.exe"}});
		test.rejects({{"release/lemon.exe"}, {"release/LEMON.exe"}});
		test.rejects({{"release/Plugins/a"}, {"release/plugins/b"}});
		test.rejects({{"release/a"}, {"release/a/b"}});
		test.rejects({{"release/a/b"}, {"release/a"}});
		test.rejects({directory("release/a/"), directory("release/a/")});
		test.rejects({{"release/a"}, directory("release/a/")});
		test.rejects({});
#ifdef _WIN32
		test.rejects({{"release/\xc3\xa9.exe"}, {"release/\xc3\x89.exe"}});
#endif
	}

	void badAttributesAndCrc(Directory &test) {
		for (const auto type : {0120777U, 0010600U, 0020600U, 0060600U, 0140600U}) {
			Fixture entry{"release/lemon.exe"};
			entry.attributes = type << 16U;
			test.rejects({entry});
		}
		Fixture reparse{"release/lemon.exe"};
		reparse.attributes |= 0x400;
		test.rejects({reparse});
		Fixture corrupt{"release/lemon.exe"};
		corrupt.corruptCrc = true;
		test.rejects({corrupt}, false);
		Fixture emptyCorrupt{"release/lemon.exe", ""};
		emptyCorrupt.corruptCrc = true;
		test.rejects({emptyCorrupt});
		Fixture inconsistent{"release/lemon.exe"};
		inconsistent.attributes = 0040755U << 16U;
		test.rejects({inconsistent});
		Fixture notEmptyDir = directory("release/");
		notEmptyDir.data = "hidden";
		test.rejects({notEmptyDir});
	}

	void malformedLocalHeaders(Directory &test) {
		auto mismatch = archive({{"release/lemon.exe"}});
		mismatch[30] = '/';
		test.rejectsBytes(mismatch);
		auto truncated = archive({{"release/lemon.exe"}});
		truncated.resize(truncated.size() - 20);
		test.rejectsBytes(truncated);
		auto flags = archive({{"release/lemon.exe"}});
		flags[6] = 1;
		test.rejectsBytes(flags);
		Fixture unsupported{"release/lemon.exe"};
		unsupported.method = 99;
		test.rejects({unsupported});
	}

	void rejectsExistingDestination(Directory &test) {
		const auto destination = test.payload();
		save(destination / "existing", "keep");
		bool rejected = false;
		try {
			test.extract(archive({{"release/lemon.exe"}}), destination);
		} catch (const std::runtime_error &) {
			rejected = true;
		}
		require(rejected, "nonempty destination accepted");
		require(load(destination / "existing") == "keep", "existing destination file changed");
#ifndef _WIN32
		const auto link = test.root / "linked-payload";
		fs::create_directory_symlink(test.payload(), link);
		rejected = false;
		try {
			test.extract(archive({{"release/lemon.exe"}}), link);
		} catch (const std::runtime_error &) {
			rejected = true;
		}
		require(rejected, "symbolic-link destination accepted");
#endif
	}
} // namespace

int main() {
	try {
		Directory test;
		storedAndUnicode(test);
		deflatedAndZip64(test);
		badNames(test);
		conflictingEntries(test);
		badAttributesAndCrc(test);
		malformedLocalHeaders(test);
		rejectsExistingDestination(test);
		std::cout << "Native archive extraction regressions passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
