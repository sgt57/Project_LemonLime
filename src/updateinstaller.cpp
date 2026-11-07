/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "updateinstaller.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>
#include <iterator>

#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#include <qt_windows.h>
#endif

namespace {
	bool inside(const QString &path, const QString &root) {
#ifdef Q_OS_WIN
		constexpr auto sensitivity = Qt::CaseInsensitive;
#else
		constexpr auto sensitivity = Qt::CaseSensitive;
#endif
		if (! path.startsWith(root, sensitivity))
			return false;
		return path.size() == root.size() || root.endsWith('/') || path.at(root.size()) == '/';
	}

	QString installationRoot() {
		QString destination = QFileInfo(QCoreApplication::applicationFilePath()).absolutePath();
#ifdef Q_OS_MACOS
		while (! destination.endsWith(".app") && ! QDir(destination).isRoot())
			destination = QFileInfo(destination).absolutePath();
		if (! destination.endsWith(".app"))
			return {};
#endif
		return QFileInfo(destination).canonicalFilePath();
	}

	bool waitForHelper(const QString &directory) {
		QEventLoop loop;
		QTimer timer;
		QElapsedTimer elapsed;
		elapsed.start();
		bool acknowledged = false;
		const QString readyFile = QDir(directory).filePath("ready");
		const QString errorFile = QDir(directory).filePath("error.log");
		QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
			acknowledged = QFileInfo::exists(readyFile);
			if (acknowledged || QFileInfo::exists(errorFile) || elapsed.elapsed() >= 10000)
				loop.quit();
		});
		timer.start(25);
		loop.exec(QEventLoop::ExcludeUserInputEvents);
		return acknowledged;
	}

	QString readError(const QString &directory) {
		QFile log(QDir(directory).filePath("error.log"));
		return log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.read(64LL * 1024)).trimmed() : QString();
	}

#ifdef Q_OS_WIN
	QString cleanupDirectory(const QString &path) {
		const QFileInfo info(path);
		if (info.isSymLink() || ! info.isDir() || ! info.fileName().startsWith("lemonlime-update-"))
			return {};
		QString directory = info.canonicalFilePath();
		if (QFileInfo(directory).absolutePath().compare(QFileInfo(QDir::tempPath()).canonicalFilePath(),
		                                               Qt::CaseInsensitive) != 0)
			return {};
		return directory;
	}

	bool isUpdateHelper(HANDLE process, const QString &directory) {
		wchar_t path[32768];
		DWORD length = static_cast<DWORD>(std::size(path));
		if (! QueryFullProcessImageNameW(process, 0, path, &length))
			return false;
		const QString executable = QFileInfo(QString::fromWCharArray(path, static_cast<qsizetype>(length)))
		                               .canonicalFilePath();
		return executable.compare(QDir(directory).filePath("lemon-updater.exe"), Qt::CaseInsensitive) == 0;
	}

	void removeUpdateDirectory(const QString &directory) {
		if (! QDir(directory).removeRecursively())
			qWarning("Cannot remove update directory: %s", qPrintable(directory));
	}

	class UpdateCleanup : public QObject {
	  public:
		UpdateCleanup(HANDLE process, const QString &directory)
		    : QObject(QCoreApplication::instance()), process(process), notifier(process, this) {
			connect(&notifier, &QWinEventNotifier::activated, this, [this, directory]() {
				notifier.setEnabled(false);
				removeUpdateDirectory(directory);
				deleteLater();
			});
			QTimer::singleShot(30000, this, &QObject::deleteLater);
		}

		~UpdateCleanup() override {
			notifier.setEnabled(false);
			CloseHandle(process);
		}

	  private:
		HANDLE process;
		QWinEventNotifier notifier;
	};
#endif
} // namespace

UpdateInstaller::UpdateInstaller(QObject *parent)
    : QObject(parent), process(new QProcess(this)), timeout(new QTimer(this)) {
	timeout->setSingleShot(true);
	process->setStandardInputFile(QProcess::nullDevice());
	connect(process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
		finishPreparation(status == QProcess::NormalExit && code == 0);
	});
	connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
		if (error == QProcess::FailedToStart)
			finishPreparation(false);
	});
	connect(timeout, &QTimer::timeout, this, [this]() {
		timedOut = true;
		process->kill();
	});
}

UpdateInstaller::~UpdateInstaller() {
	disconnect(process, nullptr, this, nullptr);
	if (process->state() != QProcess::NotRunning) {
		process->kill();
		process->waitForFinished(5000);
	}
}

void UpdateInstaller::prepare(const QString &packagePath, const QString &workDirectory) {
	if (preparing || launched) {
		emit failed(tr("An update installation is already in progress."));
		return;
	}
	ready = false;
	timedOut = false;
	directory = QFileInfo(workDirectory).canonicalFilePath();
	executable = QCoreApplication::applicationFilePath();
	installationDirectory = installationRoot();
	if (directory.isEmpty() || installationDirectory.isEmpty() || inside(directory, installationDirectory) ||
	    inside(installationDirectory, directory)) {
		emit failed(tr("The update working directory must be outside the application installation."));
		return;
	}
#ifdef Q_OS_WIN
	helper = QDir(directory).filePath("lemon-updater.exe");
#else
	helper = QDir(directory).filePath("lemon-updater");
#endif
	if (! QFile::copy(":/update/updater", helper) ||
	    ! QFile::setPermissions(helper, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
		emit failed(tr("Cannot create the independent update helper."));
		return;
	}
	preparing = true;
	process->setProgram(helper);
	process->setArguments({"prepare", directory, QFileInfo(packagePath).absoluteFilePath(), installationDirectory,
	                       executable});
	timeout->start(300000);
	process->start();
}

void UpdateInstaller::finishPreparation(bool success) {
	if (! preparing)
		return;
	timeout->stop();
	preparing = false;
	ready = success;
	if (success) {
		emit prepared();
		return;
	}
	if (timedOut) {
		emit failed(tr("The update preparation tool timed out."));
		return;
	}
	QString error = readError(directory);
	if (error.isEmpty())
		error = process->errorString();
	emit failed(tr("Update preparation failed: %1").arg(error));
}

bool UpdateInstaller::launch(QString *error) {
	if (! ready || launched) {
		*error = tr("No prepared update is available.");
		return false;
	}
	QProcess updater;
	updater.setProgram(helper);
	updater.setArguments({"install", directory, installationDirectory, executable,
	                      QString::number(QCoreApplication::applicationPid())});
	updater.setWorkingDirectory(directory);
	updater.setStandardInputFile(QProcess::nullDevice());
	updater.setProcessChannelMode(QProcess::MergedChannels);
	updater.setStandardOutputFile(QDir(directory).filePath("helper.log"));
	if (! updater.startDetached()) {
		*error = tr("Cannot start the independent update helper: %1").arg(updater.errorString());
		return false;
	}
	if (! waitForHelper(directory)) {
		QFile cancellation(QDir(directory).filePath("cancel"));
		if (cancellation.open(QIODevice::WriteOnly))
			cancellation.close();
		*error = tr("The update helper did not become ready. %1").arg(readError(directory));
		return false;
	}
	launched = true;
	return true;
}

#ifdef Q_OS_WIN
void UpdateInstaller::cleanupPreviousUpdate(const QStringList &arguments) {
	const qsizetype directoryOption = arguments.indexOf("--update-cleanup");
	const qsizetype processOption = arguments.indexOf("--update-helper-pid");
	if (directoryOption < 0 || processOption < 0 || directoryOption + 1 >= arguments.size() ||
	    processOption + 1 >= arguments.size())
		return;
	const QString directory = cleanupDirectory(arguments.at(directoryOption + 1));
	bool validPid = false;
	const quint32 pid = arguments.at(processOption + 1).toUInt(&validPid);
	if (directory.isEmpty() || ! validPid || pid == 0)
		return;
	HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (! process) {
		if (GetLastError() == ERROR_INVALID_PARAMETER)
			removeUpdateDirectory(directory);
		return;
	}
	if (! isUpdateHelper(process, directory)) {
		CloseHandle(process);
		return;
	}
	new UpdateCleanup(process, directory);
}
#endif
