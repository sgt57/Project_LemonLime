/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "updatechecker.h"

#include "base/settings.h"
#include "updateinstaller.h"
#include "updateprogressdialog.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkReply>
#include <QPushButton>
#include <QSysInfo>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {
	constexpr qint64 maximumResponseSize = 2LL * 1024 * 1024;

	QString platformName() {
#if defined(Q_OS_WIN)
		return "windows";
#elif defined(Q_OS_MACOS)
		return "macos";
#elif defined(Q_OS_LINUX)
		return "linux";
#else
		return {};
#endif
	}

	QString architectureName() {
		const QString architecture = QSysInfo::buildCpuArchitecture();
		return architecture == "aarch64" ? "arm64" : architecture;
	}

	QNetworkRequest updateRequest(const QUrl &url) {
		QNetworkRequest request(url);
		request.setHeader(QNetworkRequest::UserAgentHeader, QByteArray("LemonLime/") + LEMON_VERSION_STRING);
		request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		                     QNetworkRequest::NoLessSafeRedirectPolicy);
		request.setTransferTimeout(30000);
		return request;
	}
} // namespace

UpdateChecker::UpdateChecker(Settings *settings, QWidget *window)
    : QObject(window), settings(settings), window(window), installer(new UpdateInstaller(this)) {
	promptTimer.setInterval(250);
	connect(&promptTimer, &QTimer::timeout, this, [this]() {
		if (automatic && ! this->settings->getCheckUpdatesOnStartup()) {
			promptTimer.stop();
			busy = false;
			return;
		}
		if (! QApplication::activeModalWidget()) {
			promptTimer.stop();
			showRelease();
		}
	});
	connect(installer, &UpdateInstaller::failed, this, &UpdateChecker::fail);
	connect(installer, &UpdateInstaller::prepared, this, [this]() {
		finishProgress();
		emit readyToRestart();
		busy = false;
	});
}

UpdateChecker::~UpdateChecker() { delete installer; }

void UpdateChecker::check(bool checkAutomatically) {
	if (busy) {
		if (automatic && ! checkAutomatically) {
			automatic = false;
			if (reply)
				showCheckProgress();
		}
		return;
	}
	automatic = checkAutomatically;
	busy = true;
	canceled = false;
	response.clear();
	auto request = updateRequest(
	    QUrl("https://api.github.com/repos/Project-LemonLime/Project_LemonLime/releases/latest"));
	request.setRawHeader("Accept", "application/vnd.github+json");
	request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
	reply = network.get(request);
	reply->setReadBufferSize(maximumResponseSize);
	connect(reply, &QNetworkReply::readyRead, this, [this]() {
		response += reply->readAll();
		if (response.size() > maximumResponseSize)
			reply->abort();
	});
	connect(reply, &QNetworkReply::finished, this, &UpdateChecker::finishCheck, Qt::QueuedConnection);
	if (! automatic)
		showCheckProgress();
}

void UpdateChecker::showCheckProgress() {
	progress = new UpdateProgressDialog(tr("Checking for updates..."), tr("Cancel"), window);
	progress->setRange(0, 0);
	progress->setWindowTitle(tr("Check for Updates"));
	connect(progress, &QDialog::rejected, this, [this]() {
		canceled = true;
		if (reply)
			reply->abort();
	});
	progress->show();
}

void UpdateChecker::finishCheck() {
	response += reply->readAll();
	const auto networkError = reply->error();
	const QString errorString = reply->errorString();
	const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	reply->deleteLater();
	reply = nullptr;
	finishProgress();
	if (canceled) {
		busy = false;
		return;
	}
	if (status == 404) {
		noUpdate();
		return;
	}
	if (networkError != QNetworkReply::NoError || response.size() > maximumResponseSize) {
		fail(tr("Cannot check for updates: %1").arg(errorString));
		return;
	}
	readRelease();
}

void UpdateChecker::noUpdate() {
	busy = false;
	if (! automatic)
		QMessageBox::information(window, tr("Check for Updates"), tr("No new version is available."));
}

void UpdateChecker::readRelease() {
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);
	if (parseError.error != QJsonParseError::NoError || ! document.isObject()) {
		fail(tr("Invalid response from GitHub."));
		return;
	}
	QString error;
	release = UpdateRelease::fromJson(document.object(), platformName(), architectureName(), &error);
	if (! error.isEmpty()) {
		fail(error);
		return;
	}
	if (! release.isNewerThan(LEMON_VERSION_STRING)) {
		noUpdate();
		return;
	}
	if (automatic && ! settings->getCheckUpdatesOnStartup()) {
		busy = false;
		return;
	}
	promptTimer.start();
}

void UpdateChecker::showRelease() {
	QDialog dialog(window);
	dialog.setWindowTitle(tr("Update Available"));
	dialog.resize(620, 460);
	auto *layout = new QVBoxLayout(&dialog);
	auto *version = new QLabel(
	    tr("Current version: %1\nNew version: %2").arg(LEMON_VERSION_STRING, release.tag), &dialog);
	version->setTextFormat(Qt::PlainText);
	layout->addWidget(version);
	auto *notes = new QTextBrowser(&dialog);
	notes->setOpenExternalLinks(true);
	notes->setMarkdown(release.changelog.isEmpty() ? tr("No release notes provided.") : release.changelog);
	layout->addWidget(notes);
	if (release.downloadUrl.isEmpty()) {
		auto *unavailable =
		    new QLabel(tr("This release has no update package for this system or architecture."), &dialog);
		unavailable->setWordWrap(true);
		layout->addWidget(unavailable);
	}
	auto *buttons = new QDialogButtonBox(&dialog);
	auto *update = buttons->addButton(tr("Update"), QDialogButtonBox::AcceptRole);
	update->setEnabled(! release.downloadUrl.isEmpty());
	buttons->addButton(tr("Ignore"), QDialogButtonBox::RejectRole);
	if (automatic) {
		auto *disable = buttons->addButton(tr("Don't notify again"), QDialogButtonBox::ActionRole);
		connect(disable, &QPushButton::clicked, &dialog, [this, &dialog]() {
			settings->setCheckUpdatesOnStartup(false);
			settings->saveSettings();
			dialog.reject();
		});
	}
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	if (dialog.exec() == QDialog::Accepted)
		startDownload();
	else
		busy = false;
}

void UpdateChecker::startDownload() {
	// Download errors must be visible even when the check started automatically.
	automatic = false;
	canceled = false;
	downloadError.clear();
	directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/lemonlime-update-XXXXXX");
	if (! directory->isValid()) {
		fail(tr("Cannot create the update directory."));
		return;
	}
	download.setFileName(directory->filePath(release.assetName));
	if (! download.open(QIODevice::WriteOnly)) {
		fail(download.errorString());
		return;
	}
	hash.reset();
	progress = new UpdateProgressDialog(tr("Downloading %1...").arg(release.tag), tr("Cancel"), window);
	progress->setRange(0, 100);
	progress->setWindowTitle(tr("Update LemonLime"));
	progress->setValue(0);
	reply = network.get(updateRequest(release.downloadUrl));
	reply->setReadBufferSize(256LL * 1024);
	connect(reply, &QNetworkReply::readyRead, this, [this]() {
		readDownload();
		if (! downloadError.isEmpty() && reply)
			reply->abort();
	});
	connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
		if (progress) {
			const qint64 expected = total > 0 ? total : release.size;
			progress->setValue(static_cast<int>(
			    qBound(0.0, 100.0 * static_cast<double>(received) / static_cast<double>(expected), 100.0)));
		}
	});
	// Finish outside the network signal stack; abort() can emit finished synchronously.
	connect(reply, &QNetworkReply::finished, this, &UpdateChecker::finishDownload, Qt::QueuedConnection);
	connect(progress, &QDialog::rejected, this, [this]() {
		canceled = true;
		if (reply)
			reply->abort();
	});
	progress->show();
}

void UpdateChecker::readDownload() {
	const QByteArray data = reply->readAll();
	if (download.size() > release.size - data.size()) {
		downloadError = tr("The downloaded package has an unexpected size.");
		return;
	}
	if (download.write(data) != data.size()) {
		downloadError = download.errorString();
		return;
	}
	hash.addData(data);
}

void UpdateChecker::finishDownload() {
	if (! canceled && downloadError.isEmpty())
		readDownload();
	const auto networkError = reply->error();
	const QString errorString = reply->errorString();
	reply->deleteLater();
	reply = nullptr;
	const bool flushed = download.flush();
	if (! flushed && downloadError.isEmpty())
		downloadError = download.errorString();
	const qint64 size = download.size();
	download.close();
	if (canceled) {
		finishProgress();
		directory.reset();
		busy = false;
		return;
	}
	if (downloadError.isEmpty() && networkError != QNetworkReply::NoError)
		downloadError = errorString;
	if (! downloadError.isEmpty()) {
		fail(tr("Download failed: %1").arg(downloadError));
		return;
	}
	if (! verifyDownload(size))
		return;
	progress->setLabelText(tr("Preparing the update..."));
	progress->setRange(0, 0);
	progress->setCancelable(false);
	installer->prepare(download.fileName(), directory->path());
}

bool UpdateChecker::verifyDownload(qint64 size) {
	if (size != release.size || (! release.sha256.isEmpty() && hash.result() != release.sha256)) {
		fail(tr("The downloaded package is incomplete or its checksum does not match."));
		return false;
	}
	return true;
}

bool UpdateChecker::launchInstaller() {
	QString error;
	if (! installer->launch(&error)) {
		fail(error);
		return false;
	}
	directory->setAutoRemove(false);
	return true;
}

void UpdateChecker::finishProgress() {
	if (progress) {
		progress->hide();
		progress->deleteLater();
		progress = nullptr;
	}
}

void UpdateChecker::fail(const QString &message) {
	finishProgress();
	download.close();
	directory.reset();
	busy = false;
	if (! automatic)
		QMessageBox::warning(window, tr("Update Failed"), message);
}
