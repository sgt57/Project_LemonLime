/*
 * SPDX-FileCopyrightText: 2026 Project LemonLime
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "updateprogressdialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

UpdateProgressDialog::UpdateProgressDialog(const QString &text, const QString &cancelText, QWidget *parent)
    : QDialog(parent), label(new QLabel(text, this)), bar(new QProgressBar(this)),
      cancel(new QPushButton(cancelText, this)) {
	setWindowModality(Qt::WindowModal);
	setMinimumWidth(380);
	label->setTextFormat(Qt::PlainText);
	label->setWordWrap(true);
	auto *layout = new QVBoxLayout(this);
	layout->addWidget(label);
	layout->addWidget(bar);
	auto *buttons = new QHBoxLayout();
	buttons->addStretch();
	buttons->addWidget(cancel);
	layout->addLayout(buttons);
	connect(cancel, &QPushButton::clicked, this, &UpdateProgressDialog::reject);
}

void UpdateProgressDialog::setLabelText(const QString &text) { label->setText(text); }

void UpdateProgressDialog::setRange(int minimum, int maximum) { bar->setRange(minimum, maximum); }

void UpdateProgressDialog::setValue(int value) { bar->setValue(value); }

void UpdateProgressDialog::setCancelable(bool enabled) { cancel->setEnabled(enabled); }

void UpdateProgressDialog::reject() {
	if (cancel->isEnabled())
		QDialog::reject();
}
