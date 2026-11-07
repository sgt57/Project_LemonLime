/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "base/LemonLog.hpp"
#include "core/processrunner.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include <QFile>
#include <QPromise>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtConcurrent/QtConcurrentRun>
#include <QtTest>

#ifdef Q_OS_MACOS
#include <cerrno>
#include <csignal>
#endif

class PosixRunnerTests : public QObject {
	Q_OBJECT

  private slots:
	void initTestCase() { Lemon::base::logger = spdlog::stdout_color_mt("posix-runner-tests"); }

	void ignoresTermination_data() {
		QTest::addColumn<bool>("cancel");
		QTest::newRow("watcher-timeout") << false;
		QTest::newRow("cancellation") << true;
	}

	void ignoresTermination() {
		QFETCH(bool, cancel);
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto prefix = directory.path() + "/control";
		QPromise<void> promise;
		promise.start();
		auto cancellation = promise.future();
		ProcessRunnerConfig config;
		config.executableFile = QString::fromUtf8(HELPER_PATH);
		config.arguments = prefix;
		config.workingDirectory = directory.path() + '/';
		config.environment = QProcessEnvironment::systemEnvironment();
		config.timeLimit = config.rawTimeLimit = 100;
		config.memoryLimit = config.rawMemoryLimit = -1;
		config.interpreterAsWatcher = true;
		// Cancellation must happen before the normal deadline, after the watcher ignores SIGTERM.
		if (cancel)
			config.timeLimit = config.rawTimeLimit = 10000;
		auto running = QtConcurrent::run(
		    [config, cancellation] { return ProcessRunner::create(config, cancellation)->run(); });
		const auto release = qScopeGuard([&] {
			QFile file(prefix + ".release");
			file.open(QIODevice::WriteOnly);
			QDeadlineTimer deadline(3000);
			while (! running.isFinished() && ! deadline.hasExpired())
				QTest::qWait(10);
			if (! running.isFinished())
				qFatal("Watcher did not exit after releasing the fixture");
		});
		QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(prefix + ".ready"), 3000);
#ifdef Q_OS_MACOS
		QFile ready(prefix + ".ready");
		QVERIFY(ready.open(QIODevice::ReadOnly));
		const auto pid = ready.readAll().toInt();
		QVERIFY(pid > 0);
#endif
		if (cancel)
			cancellation.cancel();
		QTRY_VERIFY_WITH_TIMEOUT(running.isFinished(), 7000);
		if (! cancel) {
			const auto result = running.result();
			QCOMPARE(result.result, CannotStartProgram);
		}
#ifdef Q_OS_MACOS
		// Unlike Linux's PID namespace, macOS exposes the actual child PID to this fixture.
		errno = 0;
		QCOMPARE(kill(pid, 0), -1);
		QCOMPARE(errno, ESRCH);
#endif
	}
};

QTEST_GUILESS_MAIN(PosixRunnerTests)
#include "main.moc"
