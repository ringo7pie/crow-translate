/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QCloseEvent;

// Uses the same pinned, resumable installer as the Windows setup. Installing
// files never changes active application settings; choosing Use is explicit.
class OptionalComponentsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit OptionalComponentsDialog(const QString &modelRoot, QWidget *parent = nullptr,
                                      const QString &bundle = QString());
    ~OptionalComponentsDialog() override;
signals:
    void useRequested(const QString &dataRoot, const QString &component);
protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;
private:
    void refresh();
    void selectionChanged();
    void updateButtons();
    void startInstallation();
    void cancelInstallation();
    void readProgress();
    void finishInstallation(int code);
    bool installed(const QString &id) const;
    QStringList selected() const;
    QString licenseText(const QJsonObject &component, bool *valid) const;
    void closeJob();

    QString m_bundle;
    QByteArray m_catalogHash;
    QJsonArray m_components;
    bool m_bundleValid = false;
    bool m_licensesValid = false;
    bool m_running = false;
    bool m_cancelled = false;
    bool m_progressDone = false;
    QString m_progressMessage;
    QTreeWidget *m_choices;
    QLineEdit *m_root;
    QPushButton *m_browse;
    QPlainTextEdit *m_licenses;
    QCheckBox *m_consent;
    QCheckBox *m_eligibility;
    QLabel *m_status;
    QProgressBar *m_progress;
    QPushButton *m_install;
    QPushButton *m_cancel;
    QPushButton *m_use;
    QPushButton *m_close;
    QProcess *m_process = nullptr;
    QTimer m_poll;
    std::unique_ptr<QTemporaryDir> m_temporary;
    QByteArray m_diagnostics;
    void *m_job = nullptr;
};
