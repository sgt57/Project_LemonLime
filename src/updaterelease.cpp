/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "updaterelease.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QRegularExpression>

namespace {
	QVersionNumber releaseVersion(QString tag) {
		if (tag.startsWith('v'))
			tag.remove(0, 1);
		qsizetype suffix = 0;
		const auto version = QVersionNumber::fromString(tag, &suffix);
		return suffix == tag.size() ? version : QVersionNumber();
	}

	bool matchesAsset(const QString &name, const QString &platform, const QString &architecture) {
		const QString packagePlatform = platform == "windows" ? "win" : platform;
		const QString packageArch = architecture == "x86_64" ? "x64" : architecture;
		QString extension;
		if (platform == "windows")
			extension = "\\.zip";
		else if (platform == "macos")
			extension = "\\.dmg";
		const QRegularExpression currentPattern(
		    QString("^lemonlime-\\d+\\.\\d+\\.\\d+-%1-%2-release-qt6\\.\\d+\\.\\d+-[0-9a-f]+%3$")
		        .arg(QRegularExpression::escape(packagePlatform), QRegularExpression::escape(packageArch),
		             extension));
		if (currentPattern.match(name).hasMatch())
			return true;
		if (platform == "windows") {
			const QString arch = architecture == "x86_64" ? "x64" : architecture;
			return name == QString("lemon-win-qt6-%1-Release.zip").arg(arch);
		}
		if (platform == "linux")
			return name == QString("LemonLime-linux-qt6-%1-Release").arg(architecture);
		if (platform == "macos") {
			const QRegularExpression pattern(QString("^lemon-Qt6\\.\\d+\\.\\d+-Release-%1\\.dmg$")
			                                     .arg(QRegularExpression::escape(architecture)));
			return pattern.match(name).hasMatch();
		}
		return false;
	}

	bool isReleaseUrl(const QUrl &url) {
		const QString prefix = "/Project-LemonLime/Project_LemonLime/releases/download/";
		return url.isValid() && url.scheme() == "https" && url.host() == "github.com" &&
		       url.userInfo().isEmpty() && url.port(-1) == -1 && url.path().startsWith(prefix);
	}
} // namespace

UpdateRelease UpdateRelease::fromJson(const QJsonObject &object, const QString &platform,
                                      const QString &architecture, QString *error) {
	UpdateRelease release;
	release.tag = object.value("tag_name").toString();
	if (object.value("draft").toBool() || object.value("prerelease").toBool() ||
	    releaseVersion(release.tag).isNull()) {
		*error = QCoreApplication::translate("UpdateChecker", "Invalid release version.");
		return {};
	}
	release.changelog = object.value("body").toString();
	const QJsonArray assets = object.value("assets").toArray();
	for (const auto &value : assets) {
		const QJsonObject asset = value.toObject();
		const QString name = asset.value("name").toString();
		if (! matchesAsset(name, platform, architecture))
			continue;
		const QUrl url(asset.value("browser_download_url").toString());
		const QString digest = asset.value("digest").toString();
		const qint64 size = asset.value("size").toInteger();
		if (! isReleaseUrl(url) || size <= 0)
			continue;
		if (! digest.isEmpty()) {
			static const QRegularExpression sha256Pattern("^sha256:([0-9a-fA-F]{64})$");
			const auto match = sha256Pattern.match(digest);
			if (! match.hasMatch()) {
				*error = QCoreApplication::translate("UpdateChecker", "Invalid release checksum.");
				return {};
			}
			release.sha256 = QByteArray::fromHex(match.captured(1).toLatin1());
		}
		release.assetName = name;
		release.downloadUrl = url;
		release.size = size;
		break;
	}
	return release;
}

bool UpdateRelease::isNewerThan(const QString &currentVersion) const {
	QString current = currentVersion;
	if (current.startsWith('v'))
		current.remove(0, 1);
	qsizetype suffix = 0;
	const auto version = QVersionNumber::fromString(current, &suffix);
	if (version.isNull())
		return false;
	const int comparison = QVersionNumber::compare(releaseVersion(tag).normalized(), version.normalized());
	return comparison > 0 || (comparison == 0 && current.mid(suffix).startsWith('-'));
}
