/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "optionalcomponentsdialog.h"
#include "optionalcomponents.h"
#include "translator/localmodelstranslationprovider.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QTreeWidget>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <tlhelp32.h>

static bool attachWorker(void *job, qint64 pid)
{
    HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(pid));
    const bool assigned = process && AssignProcessToJobObject(job, process);
    if (process)
        CloseHandle(process);
    if (!assigned)
        return false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    bool resumed = false;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != DWORD(pid))
                continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
            if (thread) {
                resumed = ResumeThread(thread) != DWORD(-1);
                CloseHandle(thread);
            }
            break;
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return resumed;
}
#endif

OptionalComponentsDialog::OptionalComponentsDialog(const QString &modelRoot, QWidget *parent, const QString &bundle)
    : QDialog(parent)
    , m_bundle(bundle.isEmpty() ? QCoreApplication::applicationDirPath() + QStringLiteral("/optional") : bundle)
{
    setObjectName(QStringLiteral("optionalComponentsDialog"));
    setWindowTitle(tr("Manage local AI"));
    resize(830, 730);
    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(tr("Install or repair local OCR and translation models. Downloads are verified and can be resumed. Installing files does not change the active model; choose Use to apply it."));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *rootRow = new QHBoxLayout;
    rootRow->addWidget(new QLabel(tr("Component directory:")));
    m_root = new QLineEdit(OptionalComponents::root());
    m_root->setObjectName(QStringLiteral("componentRootEdit"));
    const auto models = LocalModelsTranslationProvider::modelIds();
    bool activeInstalled = false;
    for (const auto &id : models)
        activeInstalled |= LocalModelsTranslationProvider::isInstalled(modelRoot, id);
    if (activeInstalled && QDir::isAbsolutePath(modelRoot) && QFileInfo(modelRoot).fileName() == QStringLiteral("local-translation"))
        m_root->setText(QFileInfo(modelRoot).absolutePath());
    rootRow->addWidget(m_root, 1);
    m_browse = new QPushButton(tr("Browse..."));
    rootRow->addWidget(m_browse);
    layout->addLayout(rootRow);
    const QString managedRoot = OptionalComponents::root();
    if (QDir::cleanPath(m_root->text()) != QDir::cleanPath(managedRoot)) {
        bool found = QFileInfo::exists(managedRoot + QStringLiteral("/indra-models/meta.json"));
        for (const auto &id : models)
            found |= LocalModelsTranslationProvider::isInstalled(managedRoot + QStringLiteral("/local-translation"), id);
        if (found) {
            auto *detected = new QPushButton(tr("Show detected installation: %1").arg(QDir::toNativeSeparators(managedRoot)));
            detected->setObjectName(QStringLiteral("detectedComponentRootButton"));
            detected->setAutoDefault(false);
            layout->addWidget(detected);
            connect(detected, &QPushButton::clicked, this, [this, managedRoot]() { m_root->setText(managedRoot); });
        }
    }
    m_choices = new QTreeWidget;
    m_choices->setObjectName(QStringLiteral("componentChoices"));
    m_choices->setRootIsDecorated(false);
    m_choices->setHeaderLabels({tr("Component"), tr("Status"), tr("Model download"), tr("Free space required")});
    m_choices->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(m_choices);
    auto *common = new QLabel(tr("The shared environment needs up to about 2 GB in addition to the model. PLaMo conversion requires at least 80 GB free. NVIDIA translation also downloads about 0.65 GB of CUDA runtime."));
    common->setWordWrap(true);
    layout->addWidget(common);
    m_licenses = new QPlainTextEdit;
    m_licenses->setObjectName(QStringLiteral("componentLicenses"));
    m_licenses->setReadOnly(true);
    layout->addWidget(m_licenses, 2);
    m_consent = new QCheckBox(tr("I have read and agree to all licenses for the selected components."));
    m_consent->setObjectName(QStringLiteral("componentConsent"));
    m_eligibility = new QCheckBox(tr("For Indra / PLaMo, I meet the stated usage and commercial conditions,\nor have obtained the required registration / permission."));
    m_eligibility->setObjectName(QStringLiteral("componentEligibility"));
    layout->addWidget(m_consent);
    layout->addWidget(m_eligibility);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("componentStatus"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);
    m_progress = new QProgressBar;
    layout->addWidget(m_progress);
    auto *buttons = new QHBoxLayout;
    m_use = new QPushButton(tr("Use this directory / selected model"));
    m_use->setObjectName(QStringLiteral("useComponentButton"));
    m_install = new QPushButton(tr("Install selected components"));
    m_install->setObjectName(QStringLiteral("installComponentsButton"));
    m_cancel = new QPushButton(tr("Cancel installation"));
    m_cancel->setObjectName(QStringLiteral("cancelComponentsButton"));
    m_close = new QPushButton(tr("Close"));
    for (auto *button : {m_use, m_install, m_cancel, m_close}) {
        button->setAutoDefault(false);
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);

    QFile catalog(m_bundle + QStringLiteral("/optional-components.json"));
    if (catalog.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = catalog.readAll();
        m_catalogHash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        m_components = QJsonDocument::fromJson(bytes).object().value(QStringLiteral("components")).toArray();
    }
    m_bundleValid = m_components.size() == 6 && QFileInfo::exists(m_bundle + QStringLiteral("/Install-OptionalComponents.ps1"))
        && QFileInfo::exists(m_bundle + QStringLiteral("/runtime-manifest.json"));
#ifndef Q_OS_WIN
    m_bundleValid = false;
#endif
    // The catalog is also embedded for browsing when a development build does
    // not have the prepared Windows installer payload beside its executable.
    if (m_components.isEmpty()) {
        QFile fallback(QStringLiteral(":/local-models/optional-components.json"));
        if (fallback.open(QIODevice::ReadOnly))
            m_components = QJsonDocument::fromJson(fallback.readAll()).object().value(QStringLiteral("components")).toArray();
    }
    for (const auto &value : m_components) {
        const auto component = value.toObject();
        auto *item = new QTreeWidgetItem(m_choices);
        const QString name = component.value(QStringLiteral("name")).toString();
        item->setText(0, component.value(QStringLiteral("id")).toString() == QStringLiteral("cat-translate-3.3b")
                             ? tr("%1 (recommended)").arg(name) : name);
        item->setData(0, Qt::UserRole, component.value(QStringLiteral("id")).toString());
        item->setCheckState(0, Qt::Unchecked);
        item->setText(2, QStringLiteral("%1 GB").arg(component.value(QStringLiteral("download_bytes")).toDouble() / 1e9, 0, 'f', 2));
        item->setText(3, QStringLiteral("%1 GB").arg(component.value(QStringLiteral("free_bytes")).toDouble() / 1e9, 0, 'f', 1));
    }
    connect(m_browse, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getExistingDirectory(this, tr("Component directory:"), m_root->text());
        if (!path.isEmpty())
            m_root->setText(path);
    });
    connect(m_root, &QLineEdit::textChanged, this, [this]() { refresh(); selectionChanged(); });
    connect(m_choices, &QTreeWidget::itemChanged, this, &OptionalComponentsDialog::selectionChanged);
    connect(m_choices, &QTreeWidget::currentItemChanged, this, [this]() { selectionChanged(); });
    connect(m_consent, &QCheckBox::toggled, this, &OptionalComponentsDialog::updateButtons);
    connect(m_eligibility, &QCheckBox::toggled, this, &OptionalComponentsDialog::updateButtons);
    connect(m_install, &QPushButton::clicked, this, &OptionalComponentsDialog::startInstallation);
    connect(m_cancel, &QPushButton::clicked, this, &OptionalComponentsDialog::cancelInstallation);
    connect(m_close, &QPushButton::clicked, this, &OptionalComponentsDialog::reject);
    connect(m_use, &QPushButton::clicked, this, [this]() {
        const auto *item = m_choices->currentItem();
        if (item && !m_running && installed(item->data(0, Qt::UserRole).toString())) {
            emit useRequested(QDir::cleanPath(m_root->text()), item->data(0, Qt::UserRole).toString());
            accept();
        }
    });
    m_poll.setInterval(300);
    connect(&m_poll, &QTimer::timeout, this, &OptionalComponentsDialog::readProgress);
    refresh();
    if (m_choices->topLevelItemCount())
        m_choices->setCurrentItem(m_choices->topLevelItem(0));
    selectionChanged();
    if (!m_bundleValid)
        m_status->setText(tr("Installation is available in the Windows installer build. The prepared optional component bundle is missing or unsupported."));
}

OptionalComponentsDialog::~OptionalComponentsDialog()
{
    cancelInstallation();
    if (m_process && m_process->state() != QProcess::NotRunning)
        m_process->waitForFinished(3000);
    closeJob();
}

bool OptionalComponentsDialog::installed(const QString &id) const
{
    const QString root = QDir::cleanPath(m_root->text());
    if (id != QStringLiteral("indra"))
        return LocalModelsTranslationProvider::isInstalled(root + QStringLiteral("/local-translation"), id);
    if (!QFileInfo::exists(root + QStringLiteral("/local-translation/venv/Scripts/indra-ocr.exe")))
        return false;
    for (const auto &value : m_components) {
        const auto component = value.toObject();
        if (component.value(QStringLiteral("id")).toString() != id)
            continue;
        for (const auto &file : component.value(QStringLiteral("files")).toArray()) {
            const auto record = file.toObject();
            const QFileInfo info(root + QLatin1Char('/') + record.value(QStringLiteral("file")).toString());
            if (!info.isFile() || info.size() != record.value(QStringLiteral("size")).toInteger())
                return false;
        }
        return true;
    }
    return false;
}

QStringList OptionalComponentsDialog::selected() const
{
    QStringList ids;
    for (int i = 0; i < m_choices->topLevelItemCount(); ++i) {
        const auto *item = m_choices->topLevelItem(i);
        if (item->checkState(0) == Qt::Checked)
            ids << item->data(0, Qt::UserRole).toString();
    }
    return ids;
}

QString OptionalComponentsDialog::licenseText(const QJsonObject &component, bool *valid) const
{
    QString text = component.value(QStringLiteral("name")).toString() + QStringLiteral("\n")
        + component.value(QStringLiteral("license")).toString() + QStringLiteral("\n")
        + component.value(QStringLiteral("license_summary")).toString() + QStringLiteral("\n");
    for (const auto &key : {QStringLiteral("licenses"), QStringLiteral("notices")}) {
        const auto records = component.value(key).toArray();
        if (key == QStringLiteral("licenses") && records.isEmpty())
            *valid = false;
        for (const auto &value : records) {
            const auto record = value.toObject();
            const QString name = record.value(QStringLiteral("file")).toString();
            QFile file(m_bundle + QStringLiteral("/notices/") + name);
            QByteArray bytes;
            if (name != QFileInfo(name).fileName() || !file.open(QIODevice::ReadOnly)
                || (bytes = file.readAll()).size() != record.value(QStringLiteral("size")).toInteger()
                || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != record.value(QStringLiteral("sha256")).toString().toLatin1()) {
                *valid = false;
                text += tr("The bundled license or notice is missing or has an invalid checksum: %1").arg(name) + QLatin1Char('\n');
            } else {
                text += QStringLiteral("\n%1\n%2\n\n%3\n").arg(name, record.value(QStringLiteral("url")).toString(), QString::fromUtf8(bytes));
            }
        }
    }
    return text;
}

void OptionalComponentsDialog::selectionChanged()
{
    if (m_running)
        return;
    m_consent->setChecked(false);
    m_eligibility->setChecked(false);
    const auto ids = selected();
    m_eligibility->setVisible(ids.contains(QStringLiteral("indra")) || ids.contains(QStringLiteral("plamo-2-translate")));
    QStringList texts;
    m_licensesValid = true;
    const auto *current = m_choices->currentItem();
    const QString currentId = current ? current->data(0, Qt::UserRole).toString() : QString();
    for (const auto &value : m_components) {
        const auto component = value.toObject();
        const auto id = component.value(QStringLiteral("id")).toString();
        if (ids.contains(id))
            texts << licenseText(component, &m_licensesValid);
        else if (ids.isEmpty() && id == currentId) {
            bool unused = true;
            texts << licenseText(component, &unused);
        }
    }
    m_licenses->setPlainText(texts.join(QStringLiteral("\n\n====================\n\n")));
    updateButtons();
}

void OptionalComponentsDialog::refresh()
{
    const QSignalBlocker blocker(m_choices);
    for (int i = 0; i < m_choices->topLevelItemCount(); ++i) {
        auto *item = m_choices->topLevelItem(i);
        item->setText(1, installed(item->data(0, Qt::UserRole).toString()) ? tr("Installed") : tr("Missing / incomplete"));
    }
    updateButtons();
}

void OptionalComponentsDialog::updateButtons()
{
    const QString root = m_root->text().trimmed();
    const bool validRoot = QDir::isAbsolutePath(root) && QDir::cleanPath(root) != QDir(root).rootPath();
    const bool restricted = selected().contains(QStringLiteral("indra")) || selected().contains(QStringLiteral("plamo-2-translate"));
    m_install->setEnabled(!m_running && m_bundleValid && m_licensesValid && validRoot && !selected().isEmpty()
                          && m_consent->isChecked() && (!restricted || m_eligibility->isChecked()));
    const auto *current = m_choices->currentItem();
    m_use->setEnabled(!m_running && validRoot && current && installed(current->data(0, Qt::UserRole).toString()));
    m_cancel->setEnabled(m_running);
    for (auto *widget : QList<QWidget *>{m_root, m_browse, m_choices, m_consent, m_eligibility})
        widget->setEnabled(!m_running);
}

void OptionalComponentsDialog::startInstallation()
{
    updateButtons();
    if (!m_install->isEnabled())
        return;
    m_temporary = std::make_unique<QTemporaryDir>();
    QFile receipt(m_temporary->filePath(QStringLiteral("consent.json")));
    if (!m_temporary->isValid() || !receipt.open(QIODevice::WriteOnly)) {
        m_status->setText(tr("Could not create the installation consent file."));
        return;
    }
    const QJsonObject acceptance{{QStringLiteral("accepted"), true}, {QStringLiteral("catalog_sha256"), QString::fromLatin1(m_catalogHash)},
        {QStringLiteral("components"), QJsonArray::fromStringList(selected())},
        {QStringLiteral("accepted_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("default_model"), QString()}, {QStringLiteral("new_installation"), false}};
    const QByteArray bytes = QJsonDocument(acceptance).toJson();
    if (receipt.write(bytes) != bytes.size()) {
        m_status->setText(tr("Could not create the installation consent file."));
        return;
    }
    receipt.close();
#ifdef Q_OS_WIN
    m_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!m_job || !SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        closeJob();
        m_status->setText(tr("Could not create the installation process group."));
        return;
    }
#endif
    m_running = true;
    m_cancelled = false;
    m_progressDone = false;
    m_progressMessage.clear();
    m_diagnostics.clear();
    m_progress->setRange(0, 0);
    m_status->setText(tr("Preparing selected components..."));
    updateButtons();
    auto *process = new QProcess(this);
    m_process = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW | CREATE_SUSPENDED; });
    connect(process, &QProcess::started, this, [this, process]() {
        if (!attachWorker(m_job, process->processId())) {
            m_status->setText(tr("Could not create the installation process group."));
            m_diagnostics = m_status->text().toUtf8();
            closeJob();
            process->kill();
        }
    });
#endif
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() {
        m_diagnostics = (m_diagnostics + process->readAllStandardOutput()).right(4096);
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            m_diagnostics = m_process->errorString().toUtf8();
            finishInstallation(-1);
        }
    });
    connect(process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        finishInstallation(status == QProcess::NormalExit ? code : -1);
    });
    const QString executable = qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe");
    process->start(executable, {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
        QStringLiteral("-File"), QDir::toNativeSeparators(m_bundle + QStringLiteral("/Install-OptionalComponents.ps1")), QStringLiteral("-Worker"),
        QStringLiteral("-DataRoot"), QDir::toNativeSeparators(QDir::cleanPath(m_root->text())), QStringLiteral("-Receipt"), receipt.fileName(),
        QStringLiteral("-ProgressFile"), m_temporary->filePath(QStringLiteral("progress.json")), QStringLiteral("-NoConfigure")});
    m_poll.start();
}

void OptionalComponentsDialog::readProgress()
{
    if (!m_temporary)
        return;
    QFile file(m_temporary->filePath(QStringLiteral("progress.json")));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const auto progress = QJsonDocument::fromJson(file.readAll()).object();
    if (progress.isEmpty())
        return; // Atomic replacement can briefly make the file unavailable.
    m_progressMessage = progress.value(QStringLiteral("message")).toString();
    m_progressDone = progress.value(QStringLiteral("done")).toBool();
    m_status->setText(m_progressMessage);
    const int percent = progress.value(QStringLiteral("percent")).toInt(-1);
    m_progress->setRange(0, percent < 0 ? 0 : 100);
    if (percent >= 0)
        m_progress->setValue(qBound(0, percent, 100));
}

void OptionalComponentsDialog::finishInstallation(int code)
{
    if (!m_running)
        return;
    readProgress();
    m_poll.stop();
    closeJob();
    m_running = false;
    m_progress->setRange(0, 100);
    if (m_cancelled)
        m_status->setText(tr("Installation cancelled. Verified files and resumable downloads have been retained."));
    else if (code == 0 && m_progressDone) {
        m_progress->setValue(100);
        m_status->setText(tr("Installation completed. Select an installed component and choose Use to apply it.") + QLatin1Char('\n') + m_progressMessage);
    } else {
        m_status->setText(tr("Installation failed. Check the destination and connection, then retry.\n%1\n%2")
                              .arg(m_progressMessage, QString::fromLocal8Bit(m_diagnostics)));
    }
    m_process->deleteLater();
    m_process = nullptr;
    refresh();
    selectionChanged();
}

void OptionalComponentsDialog::closeJob()
{
#ifdef Q_OS_WIN
    if (m_job) {
        TerminateJobObject(m_job, 1223);
        CloseHandle(m_job);
        m_job = nullptr;
    }
#endif
}

void OptionalComponentsDialog::cancelInstallation()
{
    if (!m_running)
        return;
    m_cancelled = true;
    closeJob();
    if (m_process)
        m_process->kill();
}

void OptionalComponentsDialog::reject()
{
    if (m_running) {
        cancelInstallation();
        return; // Remain alive until the entire owned worker has stopped.
    }
    QDialog::reject();
}

void OptionalComponentsDialog::closeEvent(QCloseEvent *event)
{
    if (m_running) {
        cancelInstallation();
        event->ignore();
    } else {
        QDialog::closeEvent(event);
    }
}
