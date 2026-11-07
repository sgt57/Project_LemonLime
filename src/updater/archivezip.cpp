/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "archivezip.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Lemon::Update {
	namespace {
		namespace fs = std::filesystem;

		[[noreturn]] void fail(std::string_view message) {
			throw std::runtime_error("ZIP update: " + std::string(message));
		}

		class Reader {
		  public:
			explicit Reader(const fs::path &package) : stream(package, std::ios::binary) {
				if (! stream)
					fail("cannot open package");
				archive.m_pRead = read;
				archive.m_pIO_opaque = &stream;
				if (! mz_zip_reader_init(&archive, fs::file_size(package),
				                         MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY))
					fail("cannot read archive directory");
			}

			~Reader() { mz_zip_reader_end(&archive); }

			Reader(const Reader &) = delete;
			Reader &operator=(const Reader &) = delete;

			mz_zip_archive archive{};

		  private:
			std::ifstream stream;

			static size_t read(void *opaque, mz_uint64 offset, void *buffer, size_t count) {
				if (offset > static_cast<mz_uint64>(std::numeric_limits<std::streamoff>::max()) ||
				    count > static_cast<size_t>(std::numeric_limits<std::streamsize>::max()))
					return 0;
				auto &input = *static_cast<std::ifstream *>(opaque);
				input.clear();
				input.seekg(static_cast<std::streamoff>(offset));
				input.read(static_cast<char *>(buffer), static_cast<std::streamsize>(count));
				return static_cast<size_t>(input.gcount());
			}
		};

		struct Utf8Prefix {
			unsigned int trailing;
			std::uint32_t value;
			std::uint32_t minimum;
		};

		Utf8Prefix utf8Prefix(unsigned char first) {
			if (first >= 0xc2 && first <= 0xdf)
				return {1, first & 0x1fU, 0x80};
			if (first >= 0xe0 && first <= 0xef)
				return {2, first & 0x0fU, 0x800};
			if (first >= 0xf0 && first <= 0xf4)
				return {3, first & 0x07U, 0x10000};
			fail("invalid UTF-8 filename");
		}

		std::uint32_t codePoint(std::string_view text, size_t &offset) {
			const auto first = static_cast<unsigned char>(text[offset++]);
			if (first < 0x80)
				return first;
			auto prefix = utf8Prefix(first);
			for (unsigned int i = 0; i < prefix.trailing; ++i) {
				if (offset == text.size())
					fail("truncated UTF-8 filename");
				const auto next = static_cast<unsigned char>(text[offset++]);
				if ((next & 0xc0U) != 0x80U)
					fail("invalid UTF-8 continuation");
				prefix.value = (prefix.value << 6U) | (next & 0x3fU);
			}
			if (prefix.value < prefix.minimum || prefix.value > 0x10ffff ||
			    (prefix.value >= 0xd800 && prefix.value <= 0xdfff))
				fail("invalid UTF-8 code point");
			return prefix.value;
		}

		char asciiUpper(char value) {
			return value >= 'a' && value <= 'z' ? static_cast<char>(value - ('a' - 'A')) : value;
		}

		bool deviceNumber(std::string_view suffix) {
			if (suffix.size() == 1)
				return suffix[0] >= '1' && suffix[0] <= '9';
			return suffix == "\xc2\xb9" || suffix == "\xc2\xb2" || suffix == "\xc2\xb3";
		}

		bool asciiEqual(std::string_view left, std::string_view right) {
			return std::ranges::equal(left, right,
			                          [](char a, char b) { return asciiUpper(a) == asciiUpper(b); });
		}

		bool reservedDevice(std::string_view component) {
			const auto dot = component.find('.');
			auto stem = component.substr(0, dot);
			while (! stem.empty() && stem.back() == ' ')
				stem.remove_suffix(1);
			constexpr std::array devices{"CON", "PRN", "AUX", "NUL", "CLOCK$", "CONIN$", "CONOUT$"};
			if (std::ranges::any_of(devices,
			                        [stem](std::string_view device) { return asciiEqual(stem, device); }))
				return true;
			const auto prefix = stem.substr(0, 3);
			if (! (asciiEqual(prefix, "COM") || asciiEqual(prefix, "LPT")))
				return false;
			return deviceNumber(stem.substr(3));
		}

		void validateCharacters(std::string_view component) {
			size_t units = 0;
			size_t offset = 0;
			while (offset < component.size()) {
				const auto value = codePoint(component, offset);
				if (value < 128 &&
				    (value < 32 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(value)) !=
				                       std::string_view::npos))
					fail("invalid Windows filename character");
				units += value > 0xffff ? 2 : 1;
			}
			if (units > 255)
				fail("filename component is too long");
		}

		void validateComponent(std::string_view component) {
			if (component.empty() || component == "." || component == "..")
				fail("empty or relative filename component");
			if (component.back() == '.' || component.back() == ' ' || reservedDevice(component))
				fail("ambiguous or reserved Windows filename");
			validateCharacters(component);
		}

		fs::path utf8Path(std::string_view text) {
			return fs::path(std::u8string_view(reinterpret_cast<const char8_t *>(text.data()), text.size()));
		}

		std::string filename(mz_zip_archive &archive, mz_uint index) {
			const auto count = mz_zip_reader_get_filename(&archive, index, nullptr, 0);
			if (count <= 1)
				fail("empty archive filename");
			std::string name(count, '\0');
			if (mz_zip_reader_get_filename(&archive, index, name.data(), count) != count)
				fail("cannot read full archive filename");
			name.pop_back();
			if (name.find('\0') != std::string::npos)
				fail("embedded NUL in filename");
			return name;
		}

		void readExactly(mz_zip_archive &archive, mz_uint64 offset, void *buffer, size_t size) {
			if (offset > archive.m_archive_size || size > archive.m_archive_size - offset ||
			    mz_zip_read_archive_data(&archive, offset, buffer, size) != size)
				fail("truncated local entry");
		}

		void compareLocalName(mz_zip_archive &archive, mz_uint64 offset, std::string_view name) {
			std::array<char, 4096> buffer{};
			size_t done = 0;
			while (done < name.size()) {
				const auto count = std::min(buffer.size(), name.size() - done);
				readExactly(archive, offset + done, buffer.data(), count);
				if (std::string_view(buffer.data(), count) != name.substr(done, count))
					fail("local and central filenames disagree");
				done += count;
			}
		}

		void validateLocalHeader(mz_zip_archive &archive, const mz_zip_archive_file_stat &stat,
		                         std::string_view name) {
			std::array<unsigned char, 30> header{};
			readExactly(archive, stat.m_local_header_ofs, header.data(), header.size());
			if (MZ_READ_LE32(header.data()) != 0x04034b50U)
				fail("invalid local entry signature");
			if (MZ_READ_LE16(header.data() + 6) != stat.m_bit_flag ||
			    MZ_READ_LE16(header.data() + 8) != stat.m_method)
				fail("local and central entry flags disagree");
			const auto nameSize = MZ_READ_LE16(header.data() + 26);
			const auto extraSize = MZ_READ_LE16(header.data() + 28);
			if (nameSize != name.size())
				fail("local and central filename lengths disagree");
			const auto nameOffset = stat.m_local_header_ofs + header.size();
			const auto remaining = archive.m_archive_size - nameOffset;
			if (nameSize + extraSize > remaining || stat.m_comp_size > remaining - nameSize - extraSize)
				fail("entry extends beyond the archive");
			compareLocalName(archive, nameOffset, name);
		}

		void validateSizes(const mz_zip_archive_file_stat &stat, bool directory) {
			if (directory && (stat.m_uncomp_size != 0 || stat.m_crc32 != 0 || stat.m_comp_size != 0))
				fail("directory entry contains data");
			if (stat.m_comp_size == 0 && (stat.m_uncomp_size != 0 || stat.m_crc32 != 0))
				fail("invalid empty entry size or CRC");
		}

		void validateType(const mz_zip_archive_file_stat &stat, bool directory) {
			const auto type = (stat.m_external_attr >> 16U) & 0170000U;
			if (type != 0 && type != 0100000U && type != 0040000U)
				fail("links and special files are forbidden");
			if (type == 0040000U && ! directory)
				fail("inconsistent directory mode");
			if (type == 0100000U && directory)
				fail("inconsistent regular-file mode");
		}

		void validateAttributes(const mz_zip_archive_file_stat &stat, bool directory) {
			if (! stat.m_is_supported || stat.m_is_encrypted)
				fail("unsupported or encrypted entry");
			if ((stat.m_external_attr & 0x400U) != 0)
				fail("reparse entry is forbidden");
			if (static_cast<bool>(stat.m_is_directory) != directory)
				fail("inconsistent directory attributes");
			validateType(stat, directory);
			validateSizes(stat, directory);
		}

		struct PathLess {
			bool operator()(const fs::path &left, const fs::path &right) const {
#ifdef _WIN32
				const auto &a = left.native();
				const auto &b = right.native();
				return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
				                            static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
#else
				return std::lexicographical_compare(
				    left.native().begin(), left.native().end(), right.native().begin(), right.native().end(),
				    [](char a, char b) { return asciiUpper(a) < asciiUpper(b); });
#endif
			}
		};

		struct Node {
			bool directory;
			bool explicitEntry;
		};

		using Nodes = std::map<fs::path, Node, PathLess>;

		void registerNode(Nodes &nodes, const fs::path &path, bool directory, bool explicitEntry) {
			const auto [position, inserted] = nodes.try_emplace(path, Node{directory, explicitEntry});
			if (inserted)
				return;
			if (position->first.native() != path.native())
				fail("case-colliding archive paths");
			auto &node = position->second;
			if (! node.directory || ! directory)
				fail("duplicate entry or file/directory conflict");
			if (explicitEntry && node.explicitEntry)
				fail("duplicate directory entry");
			node.explicitEntry = node.explicitEntry || explicitEntry;
		}

		void checkRoot(std::string_view name, bool directory, std::string &root) {
			if (name.starts_with('/') || name.find("//") != std::string_view::npos)
				fail("absolute path or empty filename component");
			const auto slash = name.find('/');
			if (slash == std::string_view::npos && ! directory)
				fail("top-level entry is not a directory");
			const auto component = name.substr(0, slash);
			if (root.empty())
				root = component;
			if (root != component)
				fail("archive must contain exactly one top-level directory");
		}

		fs::path registerPath(Nodes &nodes, std::string_view name, bool directory, std::string &root) {
			checkRoot(name, directory, root);
			if (directory)
				name.remove_suffix(1);
			fs::path path;
			size_t offset = 0;
			while (offset < name.size()) {
				const auto slash = name.find('/', offset);
				const auto end = slash == std::string_view::npos ? name.size() : slash;
				const auto component = name.substr(offset, end - offset);
				validateComponent(component);
				path /= utf8Path(component);
				const bool last = end == name.size();
				registerNode(nodes, path, ! last || directory, last);
				offset = end + 1;
			}
			return path.lexically_relative(utf8Path(root));
		}

		struct Entry {
			mz_uint index;
			fs::path relative;
			bool directory;
			mz_uint64 size;
		};

		std::vector<Entry> inspect(mz_zip_archive &archive) {
			Nodes nodes;
			std::string root;
			std::vector<Entry> entries;
			const auto count = mz_zip_reader_get_num_files(&archive);
			entries.reserve(count);
			for (mz_uint index = 0; index < count; ++index) {
				mz_zip_archive_file_stat stat{};
				if (! mz_zip_reader_file_stat(&archive, index, &stat))
					fail("invalid archive entry");
				const auto name = filename(archive, index);
				const bool directory = name.ends_with('/');
				validateAttributes(stat, directory);
				validateLocalHeader(archive, stat, name);
				entries.push_back(
				    {index, registerPath(nodes, name, directory, root), directory, stat.m_uncomp_size});
			}
			if (entries.empty())
				fail("empty archive");
			return entries;
		}

		void validateDirectory(const fs::path &path) {
			const auto status = fs::symlink_status(path);
			if (! fs::is_directory(status) || fs::is_symlink(status))
				fail("destination has a non-directory or symbolic-link parent");
#ifdef _WIN32
			const auto attributes = GetFileAttributesW(path.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
				fail("destination has a reparse-point parent");
#endif
		}

		void validateParents(const fs::path &path) {
			fs::path current = path.root_path();
			validateDirectory(current);
			for (const auto &part : path.relative_path()) {
				current /= part;
				validateDirectory(current);
			}
		}

		void makeDirectories(const fs::path &payload, const fs::path &relative) {
			validateParents(payload);
			auto current = payload;
			for (const auto &part : relative) {
				if (part == ".")
					continue;
				current /= part;
				fs::create_directory(current);
				validateDirectory(current);
			}
		}

		class Output {
		  public:
			explicit Output(const fs::path &path, mz_uint64 size) : expected(size) {
#ifdef _WIN32
				handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
				                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
				if (handle == INVALID_HANDLE_VALUE)
#else
				handle = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
				if (handle == -1)
#endif
					fail("cannot exclusively create payload file");
			}

			~Output() {
#ifdef _WIN32
				if (handle != INVALID_HANDLE_VALUE)
					CloseHandle(handle);
#else
				if (handle != -1)
					::close(handle);
#endif
			}

			Output(const Output &) = delete;
			Output &operator=(const Output &) = delete;

			static size_t write(void *opaque, mz_uint64 offset, const void *buffer, size_t size) {
				auto &output = *static_cast<Output *>(opaque);
				if (offset != output.written || size > output.expected - output.written)
					return 0;
				size_t done = 0;
				while (done < size) {
					const auto count =
					    output.writeChunk(static_cast<const char *>(buffer) + done, size - done);
					if (count == 0)
						return done;
					done += count;
				}
				output.written += done;
				return done;
			}

			void finish() {
				if (written != expected)
					fail("unexpected extracted file size");
#ifdef _WIN32
				const auto old = handle;
				handle = INVALID_HANDLE_VALUE;
				if (! CloseHandle(old))
#else
				const auto old = handle;
				handle = -1;
				if (::close(old) != 0)
#endif
					fail("cannot close extracted file");
			}

		  private:
#ifdef _WIN32
			HANDLE handle = INVALID_HANDLE_VALUE;
#else
			int handle = -1;
#endif
			mz_uint64 expected;
			mz_uint64 written = 0;

			size_t writeChunk(const char *buffer, size_t size) {
#ifdef _WIN32
				const auto count = static_cast<DWORD>(std::min(size, static_cast<size_t>(MAXDWORD)));
				DWORD result = 0;
				if (! WriteFile(handle, buffer, count, &result, nullptr))
					return 0;
				return result;
#else
				const auto count = std::min(size, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
				ssize_t result = 0;
				do {
					result = ::write(handle, buffer, count);
				} while (result == -1 && errno == EINTR);
				return result < 0 ? 0 : static_cast<size_t>(result);
#endif
			}
		};

		void extractEntries(mz_zip_archive &archive, const std::vector<Entry> &entries,
		                    const fs::path &payload) {
			for (const auto &entry : entries) {
				if (entry.directory) {
					makeDirectories(payload, entry.relative);
					continue;
				}
				makeDirectories(payload, entry.relative.parent_path());
				Output output(payload / entry.relative, entry.size);
				if (! mz_zip_reader_extract_to_callback(&archive, entry.index, Output::write, &output, 0))
					fail(mz_zip_get_error_string(mz_zip_peek_last_error(&archive)));
				output.finish();
			}
		}
	} // namespace

	void extractZip(const fs::path &package, const fs::path &payload) {
		try {
			const auto destination = fs::absolute(payload).lexically_normal();
			validateParents(destination);
			if (! fs::is_empty(destination))
				fail("payload destination must be empty");
			Reader reader(package);
			const auto entries = inspect(reader.archive);
			extractEntries(reader.archive, entries, destination);
		} catch (const fs::filesystem_error &error) {
			fail("filesystem operation failed (code " + std::to_string(error.code().value()) + ")");
		}
	}
} // namespace Lemon::Update
