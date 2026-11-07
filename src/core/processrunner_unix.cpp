/*
 * SPDX-FileCopyrightText: 2011-2018 Project Lemon, Zhipeng Jia
 * SPDX-FileCopyrightText: 2018-2019 Project LemonPlus, Dust1404
 * SPDX-FileCopyrightText: 2019-2022 Project LemonLime
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

#ifndef Q_OS_WIN32

#include "processrunner_unix.h"
#include "base/LemonLog.hpp"

#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <QUuid>
#include <QtMath>

#define LEMON_MODULE_NAME "ProcessRunner"

ProcessRunnerResult UnixProcessRunner::run() {
	ProcessRunnerResult res;
	res.result = CorrectAnswer;
	int extraTime = qCeil(qMax(2000, config.timeLimit * 2) * config.extraTimeRatio);
	const auto childError = [&] {
		QFile file(config.workingDirectory + "_tmperr");
		if (! file.open(QIODevice::ReadOnly) || ! file.seek(qMax<qint64>(0, file.size() - 1024)))
			return QString{};
		return QString::fromLocal8Bit(file.read(1024));
	};

#ifdef Q_OS_LINUX
	// TODO: rewrite with cgroup
	QFile watcher(config.workingDirectory + QUuid::createUuid().toString(QUuid::Id128));

	if (config.interpreterAsWatcher) {
		QFile::copy(config.executableFile, watcher.fileName());
	} else {
		QFile::copy(":/watcher/watcher_unix", watcher.fileName());
	}

	watcher.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
	QProcess runner;
	QStringList argumentsList;

	argumentsList << "--dev" << "/dev";
	argumentsList << "--proc" << "/proc";
	argumentsList << "--ro-bind" << "/usr" << "/usr";
	argumentsList << "--symlink" << "/usr/lib" << "/lib";
	argumentsList << "--symlink" << "/usr/lib64" << "/lib64";
	argumentsList << "--symlink" << "/usr/bin" << "/bin";
	argumentsList << "--symlink" << "/usr/sbin" << "/sbin";
	argumentsList << "--tmpfs" << "/tmp";

	argumentsList << "--unshare-all" << "--die-with-parent";

	argumentsList << "--chdir" << config.workingDirectory;

	argumentsList << "--bind" << config.workingDirectory << config.workingDirectory;

	if (config.standardInputCheck) {
		argumentsList << "--ro-bind" << QFileInfo(config.inputFile).absoluteFilePath()
		              << QFileInfo(config.inputFile).absoluteFilePath();
	}

	argumentsList << watcher.fileName();

	argumentsList << config.executableFile;
	argumentsList << config.arguments;

	if (config.standardInputCheck) {
		argumentsList << QFileInfo(config.inputFile).absoluteFilePath();
	} else {
		argumentsList << "";
	}

	if (config.standardOutputCheck) {
		argumentsList << "_tmpout";
	} else {
		argumentsList << "";
	}

	argumentsList << "_tmperr";
	argumentsList << QString("%1").arg(config.timeLimit);
	argumentsList << QString("%1").arg(config.memoryLimit);
	argumentsList << QString("%1").arg(config.rawTimeLimit);
	argumentsList << QString("%1").arg(config.rawMemoryLimit);

	if (config.standardInputCheck) {
		argumentsList << "";
	} else {
		argumentsList << config.inputFileName;
	}

	if (config.standardOutputCheck) {
		argumentsList << "";
	} else {
		argumentsList << config.outputFileName;
	}

	qDebug() << argumentsList;

	QString bwrapPath = QStandardPaths::findExecutable("bwrap");
	if (bwrapPath.isEmpty()) {
		res.score = 0;
		res.result = CannotStartProgram;
		res.message = QObject::tr("bwrap not found. Please install bubblewrap.");
		return res;
	}

	runner.setProcessEnvironment(config.environment);
	runner.setWorkingDirectory(config.workingDirectory);
	runner.start(bwrapPath, argumentsList);

#else

	QFile watcher(config.workingDirectory + QUuid::createUuid().toString(QUuid::Id128));

	if (config.interpreterAsWatcher) {
		QFile::copy(config.executableFile, watcher.fileName());
	} else {
		QFile::copy(":/watcher/watcher_unix", watcher.fileName());
	}

	watcher.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
	QProcess runner;
	QStringList argumentsList;

	argumentsList << config.executableFile;
	argumentsList << config.arguments;

	if (config.standardInputCheck) {
		argumentsList << QFileInfo(config.inputFile).absoluteFilePath();
	} else {
		argumentsList << "";
	}

	if (config.standardOutputCheck) {
		argumentsList << "_tmpout";
	} else {
		argumentsList << "";
	}

	argumentsList << "_tmperr";
	argumentsList << QString("%1").arg(config.timeLimit);
	argumentsList << QString("%1").arg(config.memoryLimit);
	argumentsList << QString("%1").arg(config.rawTimeLimit);
	argumentsList << QString("%1").arg(config.rawMemoryLimit);

	if (config.standardInputCheck) {
		argumentsList << "";
	} else {
		argumentsList << config.inputFileName;
	}

	if (config.standardOutputCheck) {
		argumentsList << "";
	} else {
		argumentsList << config.outputFileName;
	}

	qDebug() << argumentsList;

	runner.setProcessEnvironment(config.environment);
	runner.setWorkingDirectory(config.workingDirectory);
	runner.start(watcher.fileName(), argumentsList);

#endif

	if (! runner.waitForStarted(30000)) {
		res.score = 0;
		res.result = CannotStartProgram;
		res.message = QString("Start runner failed: %1\n%2")
		                  .arg(runner.errorString(), QString::fromLocal8Bit(runner.readAllStandardError()));
		WARN(config.executableFile, res.message);
		return res;
	}
	const auto stopRunner = [&] {
		if (runner.state() == QProcess::NotRunning)
			return;
		runner.terminate();
		if (runner.waitForFinished(1000) || runner.state() == QProcess::NotRunning)
			return;
		WARN(config.executableFile, "Watcher ignored termination; forcing exit of pid", runner.processId());
		runner.kill();
		if (! runner.waitForFinished(1000) && runner.state() != QProcess::NotRunning)
			WARN(config.executableFile, "Cannot reap watcher after forced termination", runner.errorString());
	};

	bool isProgramFinishedInExtraTimeLimit = false;
	QElapsedTimer timer;
	timer.start();

	// Using rlimit to limit CPU time can only be accurate to seconds,
	// so here it is rounded up to an integer second.
	long long killTimeLimit = (config.timeLimit + 999) / 1000 * 1000 + extraTime;
	while (timer.elapsed() <= killTimeLimit) {
		if (runner.waitForFinished(10)) {
			isProgramFinishedInExtraTimeLimit = true;
			break;
		}

		if (cancellation.isCanceled()) {
			stopRunner();

			return res;
		}
	}

	if (! isProgramFinishedInExtraTimeLimit) {
		WARN(config.executableFile, "Watcher exceeded its wall-clock deadline after", timer.elapsed(),
		     "ms; terminating watcher pid", runner.processId());
		stopRunner();
		res.score = 0;
		res.timeUsed = res.memoryUsed = -1;
		// Watcher usually needs to handle the situation of program timeout and kill it. Therefore, it is
		// abnormal for watcher to timeout itself, and report FAIL instead of TLE.
		res.result = CannotStartProgram;
		res.message = QString("Watcher time limit exceeded after %1 ms; exit status %2, exit code %3, "
		                      "process error %4: %5\n%6")
		                  .arg(timer.elapsed())
		                  .arg(int(runner.exitStatus()))
		                  .arg(runner.exitCode())
		                  .arg(int(runner.error()))
		                  .arg(runner.errorString(), QString::fromLocal8Bit(runner.readAllStandardError()));
		res.message += '\n' + childError();
		WARN(config.executableFile, res.message);
		return res;
	}

	{
		QString out = QString::fromLocal8Bit(runner.readAllStandardOutput().constData());
		QTextStream stream(&out, QIODevice::ReadOnly);
		stream >> res.timeUsed >> res.memoryUsed;
	}

	res.message = QString::fromLocal8Bit(runner.readAllStandardError().constData());

	enum : int {
		RS_AC = 0,
		RS_FAIL = 1,
		RS_RE = 2,
		RS_TLE = 3,
		RS_MLE = 4,
	};

	int code = runner.exitCode();

	switch (code) {
		case RS_RE:
			res.result = RunTimeError;
			res.score = 0;
			[[fallthrough]];
		case RS_AC: {
			QFile file(config.workingDirectory + "_tmperr");
			if (file.open(QFile::ReadOnly)) {
				res.message += file.readAll().right(1024);
				file.close();
			}
		} break;
		case RS_FAIL:
			res.result = CannotStartProgram;
			res.score = 0;
			break;
		case RS_TLE:
			res.result = TimeLimitExceeded;
			res.score = 0;
			break;
		case RS_MLE:
			res.result = MemoryLimitExceeded;
			res.score = 0;
			break;
		default:
			res.result = CannotStartProgram;
			res.score = 0;
			res.message = QString("Watcher reported an invalid result: %1\n%2").arg(code).arg(res.message);
			break;
	}
	if (res.result == CannotStartProgram || runner.exitStatus() == QProcess::CrashExit) {
		res.message = QString("Watcher failure: exit status %1, exit code %2, process error %3: %4\n%5")
		                  .arg(int(runner.exitStatus()))
		                  .arg(code)
		                  .arg(int(runner.error()))
		                  .arg(runner.errorString(), res.message);
		res.message += '\n' + childError();
		WARN(config.executableFile, res.message);
	}

	return res;
}

#endif
