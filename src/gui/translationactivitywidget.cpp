/*
 * SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "translationactivitywidget.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

TranslationActivityWidget::TranslationActivityWidget(QWidget *parent)
    : QFrame(parent)
    , m_timer(this)
    , m_message(new QLabel(this))
    , m_elapsed(new QLabel(this))
    , m_cancel(new QPushButton(this))
    , m_progress(new QProgressBar(this))
{
    setObjectName(QStringLiteral("translationActivityWidget"));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::AlternateBase);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    m_message->setObjectName(QStringLiteral("translationActivityMessage"));
    m_message->setWordWrap(true);
    QFont font = m_message->font();
    font.setBold(true);
    m_message->setFont(font);
    m_elapsed->setObjectName(QStringLiteral("translationElapsedLabel"));
    m_cancel->setObjectName(QStringLiteral("cancelTranslationButton"));
    m_cancel->setAutoDefault(false);
    m_progress->setObjectName(QStringLiteral("translationBusyIndicator"));
    m_progress->setRange(0, 0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    auto *row = new QHBoxLayout;
    row->addWidget(m_message, 1);
    row->addWidget(m_elapsed);
    row->addWidget(m_cancel);
    layout->addLayout(row);
    layout->addWidget(m_progress);

    m_timer.setInterval(1000);
    m_timer.setObjectName(QStringLiteral("translationActivityTimer"));
    connect(&m_timer, &QTimer::timeout, this, &TranslationActivityWidget::updateText);
    connect(m_cancel, &QPushButton::clicked, this, &TranslationActivityWidget::cancelRequested);
    refresh();
}

void TranslationActivityWidget::setModel(ModuleStatus *model)
{
    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);
    m_model = model;
    if (model) {
        connect(model, &ModuleStatus::changed, this, &TranslationActivityWidget::refresh);
        connect(model, &QObject::destroyed, this, [this]() {
            m_model = nullptr;
            refresh();
        });
    }
    refresh();
}

void TranslationActivityWidget::refresh()
{
    const bool busy = m_model && m_model->activity(ModuleStatus::Module::Translation) == ModuleStatus::Activity::Busy;
    m_cancel->setText(tr("Cancel"));
    setAccessibleName(tr("Translation progress"));
    if (busy) {
        updateText();
        if (!m_timer.isActive())
            m_timer.start();
    } else {
        m_timer.stop();
        m_message->clear();
        m_message->setAccessibleName({});
        m_elapsed->clear();
        m_progress->setAccessibleName({});
    }
    m_cancel->setEnabled(busy);
    setVisible(busy);
}

void TranslationActivityWidget::updateText()
{
    if (!m_model)
        return;
    const QString message = m_model->message(ModuleStatus::Module::Translation);
    m_message->setText(message + QStringLiteral("…"));
    m_message->setAccessibleName(message);
    m_progress->setAccessibleName(message);
    m_elapsed->setText(tr("%1 s").arg(m_model->elapsedMilliseconds(ModuleStatus::Module::Translation) / 1000));
}

void TranslationActivityWidget::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange)
        refresh();
    QFrame::changeEvent(event);
}
