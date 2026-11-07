/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;
class QTimer;

class UpdateInstaller : public QObject {
	Q_OBJECT

  public:
	explicit UpdateInstaller(QObject *parent = nullptr);
	~UpdateInstaller() override;
	void prepare(const QString &packagePath, const QString &workDirectory);
	bool launch(QString *error);
#ifdef Q_OS_WIN
	static void cleanupPreviousUpdate(const QStringList &arguments);
#endif

  signals:
	void prepared();
	void failed(const QString &message);

  private:
	QString directory;
	QString installationDirectory;
	QString helper;
	QString executable;
	bool preparing = false;
	bool ready = false;
	bool launched = false;
	bool timedOut = false;
	QProcess *process;
	QTimer *timeout;
	void finishPreparation(bool success);
};
