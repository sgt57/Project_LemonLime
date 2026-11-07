/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

namespace {
	QByteArray load(const QString &path) {
		QFile file(path);
		if (! file.open(QIODevice::ReadOnly))
			return {};
		return file.readAll();
	}

	bool save(const QString &path, const QByteArray &data) {
		if (! QDir().mkpath(QFileInfo(path).absolutePath()))
			return false;
		QFile file(path);
		return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
	}

	bool copyExecutable(const QString &source, const QString &destination) {
		return QDir().mkpath(QFileInfo(destination).absolutePath()) && QFile::copy(source, destination) &&
		       QFile::setPermissions(destination, QFile::permissions(source));
	}

	// Also release detached fixtures on assertion failure; no process may outlive its temporary tree.
	struct Processes {
		QString root;
		QProcess parent;
		QProcess installer;
		explicit Processes(const QString &root) : root(root) {}
		~Processes() {
			save(root + "/exit-parent", "exit");
			save(root + "/stop-new", "stop");
			for (auto *process : {&parent, &installer}) {
				if (process->state() != QProcess::NotRunning) {
					process->kill();
					process->waitForFinished(5000);
				}
			}
			if (QFile::exists(root + "/restarted")) {
				QElapsedTimer timer;
				timer.start();
				while (! QFile::exists(root + "/new-exited") && timer.elapsed() < 5000)
					QTest::qWait(20);
			}
		}
	};
} // namespace

class UpdaterLifecycleTest : public QObject {
	Q_OBJECT

  private slots:
	void prepareThroughCleanup() {
		QTemporaryDir temporary(QDir::tempPath() + "/lemon updater-XXXXXX");
		QVERIFY(temporary.isValid());
		// macOS /var is a symlink; production deliberately rejects symlinked update ancestors.
		const auto root = QFileInfo(temporary.path()).canonicalFilePath();
		const auto work = root + "/update work";
#ifdef Q_OS_MACOS
		const auto installation = root + "/installed app/Lemon.app";
		const QString relativeExecutable = "Contents/MacOS/lemon";
		const QString relativeData = "Contents/Resources/version.txt";
		const QString relativeAdded = "Contents/Resources/new directory/added.txt";
#else
		const auto installation = root + "/installed app";
#ifdef Q_OS_WIN
		const QString relativeExecutable = "lemon.exe";
#else
		const QString relativeExecutable = "lemon";
#endif
		const QString relativeData = "resources/version.txt";
		const QString relativeAdded = "resources/new directory/added.txt";
#endif
		const auto executable = installation + '/' + relativeExecutable;
		QVERIFY(QDir().mkpath(work));
		QVERIFY(copyExecutable(QString::fromUtf8(OLD_FIXTURE_PATH), executable));
		QVERIFY(save(installation + '/' + relativeData, "old resource\n"));
		QVERIFY(save(installation + "/user-settings.ini", "keep user settings\n"));
		const auto oldBinary = load(executable);
		const auto newBinary = load(QString::fromUtf8(NEW_FIXTURE_PATH));
		QVERIFY(! oldBinary.isEmpty());
		QVERIFY(! newBinary.isEmpty());
		QVERIFY(oldBinary != newBinary);

		QString package;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
		const auto packageRoot = root + "/package source";
#ifdef Q_OS_MACOS
		const auto packageTree = packageRoot + "/Lemon.app";
#else
		const auto packageTree = packageRoot + "/Lemon";
#endif
		QVERIFY(copyExecutable(QString::fromUtf8(NEW_FIXTURE_PATH), packageTree + '/' + relativeExecutable));
		QVERIFY(save(packageTree + '/' + relativeData, "new resource\n"));
		QVERIFY(save(packageTree + '/' + relativeAdded, "added resource\n"));
		QProcess packager;
		packager.setWorkingDirectory(packageRoot);
#ifdef Q_OS_WIN
		package = work + "/release.zip";
		packager.start(QString::fromUtf8(CMAKE_PATH),
		               {"-E", "tar", "cf", package, "--format=zip", "--", "Lemon"});
#else
		package = work + "/release.dmg";
		packager.start("/usr/bin/hdiutil", {"create", "-srcfolder", packageRoot, "-format", "UDZO",
		                                    "-volname", "Updater fixture", "-ov", package});
#endif
		QVERIFY(packager.waitForStarted());
		QVERIFY2(packager.waitForFinished(30000), qPrintable(packager.errorString()));
		QCOMPARE(packager.exitStatus(), QProcess::NormalExit);
		QVERIFY2(packager.exitCode() == 0, packager.readAllStandardError().constData());
#else
		package = work + "/release";
		QVERIFY(QFile::copy(QString::fromUtf8(NEW_FIXTURE_PATH), package));
#endif
		// Run the shipped binary from the disposable work directory, including Windows self-cleanup.
#ifdef Q_OS_WIN
		const auto updater = work + "/updater.exe";
#else
		const auto updater = work + "/updater";
#endif
		QVERIFY(copyExecutable(QString::fromUtf8(UPDATER_PATH), updater));
		Processes processes{root};
		auto environment = QProcessEnvironment::systemEnvironment();
		environment.insert("LEMON_UPDATER_TEST_ROOT", root);
		processes.parent.setProcessEnvironment(environment);
		processes.parent.start(executable, {"--parent"});
		QVERIFY(processes.parent.waitForStarted());
		QTRY_COMPARE_WITH_TIMEOUT(load(root + "/parent-ready"), QByteArray("old"), 5000);

		QProcess prepare;
		prepare.start(updater, {"prepare", work, package, installation, executable});
		QVERIFY(prepare.waitForStarted());
		QVERIFY2(prepare.waitForFinished(30000),
		         qPrintable(prepare.errorString() + '\n' + QString::fromUtf8(load(work + "/error.log"))));
		QCOMPARE(prepare.exitStatus(), QProcess::NormalExit);
		QVERIFY2(prepare.exitCode() == 0, load(work + "/error.log").constData());
		QCOMPARE(load(work + "/payload/" + relativeExecutable), newBinary);
		QCOMPARE(load(executable), oldBinary);
		QVERIFY(! QFile::exists(work + "/backup"));
		qInfo("prepare: native package staged; running installation unchanged");

		processes.installer.setProcessEnvironment(environment);
		processes.installer.start(updater, {"install", work, installation, executable,
		                                    QString::number(processes.parent.processId())});
		QVERIFY(processes.installer.waitForStarted());
		QTRY_VERIFY2_WITH_TIMEOUT(QFile::exists(work + "/ready"), load(work + "/error.log").constData(),
		                          5000);
		// Exercise several real waitForParent polling intervals, not just the ready-file handshake.
		QTest::qWait(400);
		QCOMPARE(processes.parent.state(), QProcess::Running);
		QCOMPARE(processes.installer.state(), QProcess::Running);
		QVERIFY(! QFile::exists(work + "/backup"));
		QVERIFY(! QFile::exists(root + "/restarted"));
		QCOMPARE(load(executable), oldBinary);
		QCOMPARE(load(installation + '/' + relativeData), QByteArray("old resource\n"));
		qInfo("parent exit: installer waits without backing up or replacing live files");

		QVERIFY(save(root + "/exit-parent", "exit"));
		QVERIFY(processes.parent.waitForFinished(5000));
		QCOMPARE(processes.parent.exitStatus(), QProcess::NormalExit);
		QCOMPARE(processes.parent.exitCode(), 0);
		QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(root + "/restarted"), 10000);
		QCOMPARE(load(root + "/observed-backup/" + relativeExecutable), oldBinary);
		QCOMPARE(load(executable), newBinary);
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
		QCOMPARE(load(root + "/observed-backup/" + relativeData), QByteArray("old resource\n"));
		QVERIFY(! QFile::exists(root + "/observed-backup/" + relativeAdded));
		QCOMPARE(load(installation + '/' + relativeData), QByteArray("new resource\n"));
		QCOMPARE(load(installation + '/' + relativeAdded), QByteArray("added resource\n"));
#else
		QCOMPARE(load(installation + '/' + relativeData), QByteArray("old resource\n"));
#endif
		QCOMPARE(load(installation + "/user-settings.ini"), QByteArray("keep user settings\n"));
		qInfo("backup -> replace: original bytes preserved; new payload installed; user files retained");
		const auto restart = load(root + "/restarted");
		QVERIFY(restart.startsWith("new\n"));
		QCOMPARE(QFileInfo(QString::fromUtf8(restart.mid(4))).canonicalFilePath(),
		         QFileInfo(executable).absoluteDir().canonicalPath());
		qInfo("restart: new executable running in the application directory");

		QTRY_COMPARE_WITH_TIMEOUT(processes.installer.state(), QProcess::NotRunning, 15000);
		QCOMPARE(processes.installer.exitStatus(), QProcess::NormalExit);
		QVERIFY2(processes.installer.exitCode() == 0, load(work + "/error.log").constData());
#ifdef Q_OS_WIN
		QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(root + "/cleanup-done"), 5000);
#endif
		QTRY_VERIFY_WITH_TIMEOUT(! QFile::exists(work), 5000);
		QDirIterator entries(installation, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot,
		                     QDirIterator::Subdirectories);
		while (entries.hasNext()) {
			entries.next();
			QVERIFY(! entries.fileName().startsWith(".lemon-update-"));
		}
		QVERIFY(! QFile::exists(root + "/new-exited"));
		QVERIFY(save(root + "/stop-new", "stop"));
		QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(root + "/new-exited"), 5000);
		qInfo("cleanup: updater, package, payload, backups and temporary replacements removed");
	}
};

QTEST_GUILESS_MAIN(UpdaterLifecycleTest)
#include "main.moc"
