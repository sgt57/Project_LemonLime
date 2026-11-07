/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QJsonObject>
#include <QUrl>
#include <QVersionNumber>

struct UpdateRelease {
	QString tag;
	QString changelog;
	QString assetName;
	QUrl downloadUrl;
	QByteArray sha256;
	qint64 size = 0;

	static UpdateRelease fromJson(const QJsonObject &object, const QString &platform,
	                              const QString &architecture, QString *error);
	bool isNewerThan(const QString &currentVersion) const;
};
