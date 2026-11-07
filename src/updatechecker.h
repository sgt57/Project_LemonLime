/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "updaterelease.h"

#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

class Settings;
class QWidget;
class UpdateProgressDialog;
class QNetworkReply;
class UpdateInstaller;

class UpdateChecker : public QObject {
	Q_OBJECT

  public:
	UpdateChecker(Settings *settings, QWidget *window);
	~UpdateChecker() override;
	void check(bool automatic = false);
	bool launchInstaller();

  signals:
	void readyToRestart();

  private:
	Settings *settings;
	QWidget *window;
	QNetworkAccessManager network;
	QPointer<QNetworkReply> reply;
	QPointer<UpdateProgressDialog> progress;
	QTimer promptTimer;
	UpdateInstaller *installer;
	UpdateRelease release;
	std::unique_ptr<QTemporaryDir> directory;
	QFile download;
	QCryptographicHash hash{QCryptographicHash::Sha256};
	QByteArray response;
	QString downloadError;
	bool automatic = false;
	bool busy = false;
	bool canceled = false;

	void finishCheck();
	void readRelease();
	void noUpdate();
	void showCheckProgress();
	void showRelease();
	void startDownload();
	void readDownload();
	void finishDownload();
	bool verifyDownload(qint64 size);
	void finishProgress();
	void fail(const QString &message);
};
