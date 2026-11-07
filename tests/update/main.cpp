/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "updaterelease.h"

#include <QJsonArray>
#include <QtTest>

namespace {
	QJsonObject
	asset(const QString &name,
	      const QString &url =
	          "https://github.com/Project-LemonLime/Project_LemonLime/releases/download/0.3.8/package") {
		return {{"name", name}, {"browser_download_url", url}, {"size", 12345}};
	}

	QJsonObject release(const QJsonArray &assets = {}) {
		return {{"tag_name", "v0.3.8"}, {"body", "## Changes\n修复评测"}, {"assets", assets}};
	}
} // namespace

class UpdateTest : public QObject {
	Q_OBJECT

  private slots:
	void versionOrdering_data() {
		QTest::addColumn<QString>("latest");
		QTest::addColumn<QString>("current");
		QTest::addColumn<bool>("newer");
		QTest::newRow("numeric-not-lexical") << "0.3.10" << "0.3.9" << true;
		QTest::newRow("optional-v") << "v0.4.0" << "0.3.9" << true;
		QTest::newRow("equal") << "0.3.8" << "0.3.8" << false;
		QTest::newRow("trailing-zero") << "0.3.8.0" << "0.3.8" << false;
		QTest::newRow("older-release") << "0.3.7" << "0.3.8" << false;
		QTest::newRow("stable-after-prerelease") << "0.3.8" << "0.3.8-beta.1" << true;
		QTest::newRow("build-metadata") << "0.3.8" << "0.3.8+build.1" << false;
	}

	void versionOrdering() {
		QFETCH(QString, latest);
		QFETCH(QString, current);
		QFETCH(bool, newer);
		UpdateRelease info;
		info.tag = latest;
		QCOMPARE(info.isNewerThan(current), newer);
	}

	void rejectsNonStableRelease_data() {
		QTest::addColumn<QString>("tag");
		QTest::addColumn<QString>("flag");
		QTest::newRow("malformed-version") << "latest" << "";
		QTest::newRow("prerelease-tag") << "0.3.8-rc.1" << "";
		QTest::newRow("draft") << "0.3.8" << "draft";
		QTest::newRow("prerelease") << "0.3.8" << "prerelease";
	}

	void rejectsNonStableRelease() {
		QFETCH(QString, tag);
		QFETCH(QString, flag);
		auto object = release();
		object.insert("tag_name", tag);
		if (! flag.isEmpty())
			object.insert(flag, true);
		QString error;
		const auto info = UpdateRelease::fromJson(object, "windows", "x86_64", &error);
		QVERIFY(! error.isEmpty());
		QVERIFY(info.downloadUrl.isEmpty());
	}

	void selectsCompatibleRelease_data() {
		QTest::addColumn<QString>("platform");
		QTest::addColumn<QString>("architecture");
		QTest::addColumn<QString>("expected");
		QTest::newRow("windows-x64") << "windows" << "x86_64" << "lemon-win-qt6-x64-Release.zip";
		QTest::newRow("windows-arm64")
		    << "windows" << "arm64" << "lemonlime-0.3.8-win-arm64-release-qt6.12.0-abcdef.zip";
		QTest::newRow("linux-x64") << "linux" << "x86_64" << "LemonLime-linux-qt6-x86_64-Release";
		QTest::newRow("mac-arm64") << "macos" << "arm64" << "lemon-Qt6.12.0-Release-arm64.dmg";
		QTest::newRow("mac-x64") << "macos" << "x86_64"
		                         << "lemonlime-0.3.8-macos-x64-release-qt6.12.0-abcdef.dmg";
	}

	void selectsCompatibleRelease() {
		QFETCH(QString, platform);
		QFETCH(QString, architecture);
		QFETCH(QString, expected);
		const QJsonArray assets{asset("Lemon-0.3.8-source-all.7z"), asset("lemon-win-qt6-x64-Debug.zip"),
		                        asset("LemonLime-0.3.8-debian-sid-Release.deb"), asset(expected)};
		QString error;
		const auto info = UpdateRelease::fromJson(release(assets), platform, architecture, &error);
		QVERIFY2(error.isEmpty(), qPrintable(error));
		QCOMPARE(info.assetName, expected);
		QCOMPARE(info.changelog, QString("## Changes\n修复评测"));
	}

	void rejectsUnsafeAsset_data() {
		QTest::addColumn<QString>("url");
		QTest::newRow("plaintext")
		    << "http://github.com/Project-LemonLime/Project_LemonLime/releases/download/0.3.8/a.zip";
		QTest::newRow("other-host")
		    << "https://example.com/Project-LemonLime/Project_LemonLime/releases/download/0.3.8/a.zip";
		QTest::newRow("other-project") << "https://github.com/other/project/releases/download/0.3.8/a.zip";
		QTest::newRow("credentials") << "https://user:password@github.com/Project-LemonLime/"
		                                "Project_LemonLime/releases/download/0.3.8/a.zip";
	}

	void rejectsUnsafeAsset() {
		QFETCH(QString, url);
		QString error;
		const auto info = UpdateRelease::fromJson(release({asset("lemon-win-qt6-x64-Release.zip", url)}),
		                                          "windows", "x86_64", &error);
		QVERIFY(info.downloadUrl.isEmpty());
	}

	void checksumValidation() {
		auto package = asset("lemon-win-qt6-x64-Release.zip");
		package.insert("digest", "sha256:invalid");
		QString error;
		const auto invalid = UpdateRelease::fromJson(release({package}), "windows", "x86_64", &error);
		QVERIFY(! error.isEmpty());
		QVERIFY(invalid.downloadUrl.isEmpty());
		const QByteArray digest(32, '\x12');
		package.insert("digest", QString("sha256:") + QString::fromLatin1(digest.toHex()));
		error.clear();
		const auto valid = UpdateRelease::fromJson(release({package}), "windows", "x86_64", &error);
		QVERIFY(error.isEmpty());
		QCOMPARE(valid.sha256, digest);
	}
};

QTEST_GUILESS_MAIN(UpdateTest)
#include "main.moc"
