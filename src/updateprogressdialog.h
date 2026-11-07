/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>

class QLabel;
class QProgressBar;
class QPushButton;

class UpdateProgressDialog : public QDialog {
  public:
	UpdateProgressDialog(const QString &text, const QString &cancelText, QWidget *parent);
	void setLabelText(const QString &text);
	void setRange(int minimum, int maximum);
	void setValue(int value);
	void setCancelable(bool enabled);
	void reject() override;

  private:
	QLabel *label;
	QProgressBar *bar;
	QPushButton *cancel;
};
