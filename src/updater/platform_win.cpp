/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifdef _WIN32

#include "platform.h"
#include "archivezip.h"
#include "filesystem.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <limits>
#include <utility>

namespace Lemon::Update {

	namespace {
		class Handle {
		public:
			explicit Handle(HANDLE value) : value(value) {}
			~Handle() {
				if (value && value != INVALID_HANDLE_VALUE)
					CloseHandle(value);
			}
			Handle(const Handle &) = delete;
			Handle &operator=(const Handle &) = delete;
			HANDLE get() const noexcept { return value; }

		private:
			HANDLE value;
		};

		std::string toUtf8(std::wstring_view text) {
			if (text.empty())
				return {};
			const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
			                                      static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			if (size == 0)
				return "Windows error (invalid diagnostic encoding)";
			std::string result(static_cast<std::size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
			                    result.data(), size, nullptr, nullptr);
			return result;
		}

		std::wstring fromUtf8(std::string_view text) {
			const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
			                                      static_cast<int>(text.size()), nullptr, 0);
			if (size == 0)
				return L"LemonLime update failed. Cannot decode the error diagnostic.";
			std::wstring result(static_cast<std::size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
			                    result.data(), size);
			return result;
		}

		void windowsFailure(std::string_view operation, const fs::path &path) {
			const auto code = GetLastError();
			fail(std::string(operation) + ": " + pathText(path) + ": " + nativeError(code));
		}

		class WindowsParent final : public ParentProcess {
		public:
			explicit WindowsParent(unsigned long pid, const fs::path &executable)
			    : process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
				if (! process.get())
					windowsFailure("Cannot acquire the parent process", executable);
				std::wstring image(32768, L'\0');
				DWORD length = static_cast<DWORD>(image.size());
				if (! QueryFullProcessImageNameW(process.get(), 0, image.data(), &length))
					windowsFailure("Cannot identify the parent executable", executable);
				image.resize(length);
				if (! fs::equivalent(fs::path(image), executable))
					fail("The supplied parent process is not the installed application.");
				if (exited())
					fail("The parent process exited before the update handshake.");
			}

			bool exited() override {
				const auto status = WaitForSingleObject(process.get(), 0);
				if (status == WAIT_FAILED)
					fail("Cannot wait for the parent process: " + nativeError(GetLastError()));
				return status == WAIT_OBJECT_0;
			}

		private:
			Handle process;
		};

		std::wstring quoteArgument(std::wstring_view text) {
			std::wstring result = L"\"";
			std::size_t slashes = 0;
			for (const auto character : text) {
				if (character == L'\\') {
					++slashes;
					continue;
				}
				result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
				slashes = 0;
				result += character;
			}
			result.append(slashes * 2, L'\\');
			result += L'"';
			return result;
		}

		std::wstring applicationCommand(const Arguments &arguments, bool recovery) {
			auto command = quoteArgument(arguments.executable.native());
			if (recovery) {
				command += L" --update-error " + quoteArgument((arguments.work / "error.log").native());
			} else {
				command += L" --update-cleanup " + quoteArgument(arguments.work.native());
				command += L" --update-helper-pid " + std::to_wstring(currentPid());
			}
			return command;
		}
	}

	std::string nativeError(unsigned long code) {
		wchar_t *buffer = nullptr;
		const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
		                                       FORMAT_MESSAGE_IGNORE_INSERTS,
		                                   nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
		if (length == 0)
			return "Windows error " + std::to_string(code);
		const auto text = toUtf8(std::wstring_view(buffer, length));
		LocalFree(buffer);
		return text;
	}

	unsigned long currentPid() noexcept { return GetCurrentProcessId(); }

	void showError(std::string_view message) noexcept {
		try {
			const auto text = fromUtf8(message);
			MessageBoxW(nullptr, text.c_str(), L"LemonLime update", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
		} catch (...) {
			MessageBoxW(nullptr, L"LemonLime update failed; see error.log in the update work directory.",
			            L"LemonLime update", MB_OK | MB_ICONERROR);
		}
	}

	std::unique_ptr<ParentProcess> acquireParent(unsigned long pid, const fs::path &executable) {
		return std::make_unique<WindowsParent>(pid, executable);
	}

	void stagePackage(const Arguments &arguments, const fs::path &payload) {
		extractZip(arguments.package, payload);
	}

	void checkNativePath(const fs::path &path) {
		const auto attributes = GetFileAttributesW(path.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES)
			windowsFailure("Cannot inspect installation path", path);
		if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			fail("Reparse points are not allowed in update paths: " + pathText(path));
	}

	void checkExclusiveFile(const fs::path &path) {
		const Handle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
		                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
		if (file.get() == INVALID_HANDLE_VALUE)
			windowsFailure("Installation file is locked or not writable", path);
	}

	void copyLeaf(const fs::path &source, const fs::path &destination) {
		requireRegularFile(source);
		if (! CopyFileW(source.c_str(), destination.c_str(), TRUE))
			windowsFailure("Cannot copy update file", destination);
	}

	void copyDirectoryMetadata(const fs::path &source, const fs::path &destination) {
		const Handle input(CreateFileW(source.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
		                                                                      FILE_SHARE_DELETE,
		                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
		const Handle output(CreateFileW(destination.c_str(), FILE_WRITE_ATTRIBUTES,
		                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
		if (input.get() == INVALID_HANDLE_VALUE || output.get() == INVALID_HANDLE_VALUE)
			windowsFailure("Cannot open directory metadata", destination);
		FILETIME creation{}, access{}, modification{};
		if (! GetFileTime(input.get(), &creation, &access, &modification))
			windowsFailure("Cannot read directory metadata", source);
		if (! SetFileTime(output.get(), &creation, &access, &modification))
			windowsFailure("Cannot preserve directory metadata", destination);
	}

	void atomicReplace(const fs::path &source, const fs::path &destination) {
		if (! MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			windowsFailure("Cannot atomically replace installation file", destination);
	}

	void launchApplication(const Arguments &arguments, bool recovery) {
		auto command = applicationCommand(arguments, recovery);
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		const auto directory = arguments.executable.parent_path();
		if (! CreateProcessW(arguments.executable.c_str(), command.data(), nullptr, nullptr, FALSE,
		                      CREATE_UNICODE_ENVIRONMENT, nullptr, directory.c_str(), &startup, &process))
			windowsFailure("Cannot restart the application", arguments.executable);
		const Handle processHandle(process.hProcess);
		const Handle threadHandle(process.hThread);
		const auto status = WaitForSingleObject(processHandle.get(), 1000);
		if (status == WAIT_FAILED)
			windowsFailure("Cannot check the restarted application", arguments.executable);
		if (status != WAIT_TIMEOUT)
			fail("The restarted application exited immediately.");
	}

}

#endif
