/*
 * SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TRANSLATIONACTIVITYWIDGET_H
#define TRANSLATIONACTIVITYWIDGET_H

#include "modulestatus.h"

#include <QFrame>
#include <QPointer>
#include <QTimer>

class QLabel;
class QProgressBar;
class QPushButton;

// Visible beside the result, independent of the optional status bar. Both
// windows observe the same model; neither window owns a translation request.
class TranslationActivityWidget : public QFrame
{
    Q_OBJECT
public:
    explicit TranslationActivityWidget(QWidget *parent = nullptr);
    void setModel(ModuleStatus *model);

signals:
    void cancelRequested();

protected:
    void changeEvent(QEvent *event) override;

private:
    void refresh();
    void updateText();

    QPointer<ModuleStatus> m_model;
    QTimer m_timer;
    QLabel *m_message;
    QLabel *m_elapsed;
    QPushButton *m_cancel;
    QProgressBar *m_progress;
};

#endif // TRANSLATIONACTIVITYWIDGET_H
