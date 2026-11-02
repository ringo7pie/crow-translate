/* SPDX-FileCopyrightText: 2026 Crow Translate contributors
 * SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "localmodelstranslationprovider.h"
#include "settings/appsettings.h"
#include "optionalcomponents.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <tlhelp32.h>
#elif defined(Q_OS_UNIX)
#include <signal.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
static bool resumeWorker(DWORD processId)
{
    // Assign the process to its job before the Python venv redirector can
    // spawn its interpreter. Already-running children do not join a new job.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    DWORD threadId = 0;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == processId) {
                threadId = entry.th32ThreadID;
                break;
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, threadId);
    if (!thread)
        return false;
    const bool resumed = ResumeThread(thread) != static_cast<DWORD>(-1);
    CloseHandle(thread);
    return resumed;
}
#endif

static void initLocalModelsResource()
{
    Q_INIT_RESOURCE(localmodels);
}

static QJsonArray catalog()
{
    initLocalModelsResource();
    QFile file(QStringLiteral(":/local-models/catalog.json"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("models")).toArray();
}

static QJsonObject profile(const QString &id)
{
    for (const auto &entry : catalog()) {
        if (entry.toObject().value(QStringLiteral("id")).toString() == id)
            return entry.toObject();
    }
    return {};
}

QStringList LocalModelsTranslationProvider::modelIds()
{
    QStringList ids;
    for (const auto &entry : catalog())
        ids.append(entry.toObject().value(QStringLiteral("id")).toString());
    return ids;
}

QString LocalModelsTranslationProvider::modelName(const QString &id)
{
    return profile(id).value(QStringLiteral("name")).toString();
}

QString LocalModelsTranslationProvider::modelFile(const QString &id)
{
    return profile(id).value(QStringLiteral("file")).toString();
}

QString LocalModelsTranslationProvider::defaultPrompt(const QString &id)
{
    return profile(id).value(QStringLiteral("prompt")).toString();
}

QString LocalModelsTranslationProvider::defaultRoot()
{
    const QString development = QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../third_party/local-translation"));
    if (QFileInfo::exists(development + QStringLiteral("/venv")))
        return development;
#ifdef Q_OS_WIN
    const QString managed = OptionalComponents::root() + QStringLiteral("/local-translation");
    if (QFileInfo::exists(OptionalComponents::python(managed)))
        return managed;
#endif
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/local-translation");
}

bool LocalModelsTranslationProvider::isInstalled(const QString &root, const QString &id)
{
#ifdef Q_OS_WIN
    const QString server = QStringLiteral("/runtime/llama-server.exe");
#else
    const QString server = QStringLiteral("/runtime/llama-server");
#endif
    const QString file = modelFile(id);
    return !file.isEmpty() && QFileInfo::exists(OptionalComponents::python(root)) && QFileInfo::exists(root + server)
        && QFileInfo::exists(root + QStringLiteral("/models/") + file)
        && (id != QLatin1String("seed-x-ppo-7b") || QFileInfo::exists(root + QStringLiteral("/tokenizers/seed-x-ppo-7b/tokenizer.json")));
}

LocalModelsTranslationProvider::LocalModelsTranslationProvider(QObject *parent)
    : ATranslationProvider(parent), m_root(defaultRoot())
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this]() { fail(tr("Local translation timed out. Try a shorter passage or increase the timeout.")); });
}

LocalModelsTranslationProvider::~LocalModelsTranslationProvider()
{
    stopWorker();
}

QString LocalModelsTranslationProvider::getProviderType() const { return QStringLiteral("LocalModelsTranslationProvider"); }
QVector<Language> LocalModelsTranslationProvider::supportedSourceLanguages()
{
    return {Language(QStringLiteral("en")), Language(QStringLiteral("ja"))};
}
QVector<Language> LocalModelsTranslationProvider::supportedDestinationLanguages() { return supportedSourceLanguages(); }
bool LocalModelsTranslationProvider::supportsAutodetection() const { return true; }
ProviderCapabilities LocalModelsTranslationProvider::capabilities() const
{
    return ProviderCapability::LanguageDetection | ProviderCapability::ModelSelection;
}

Language LocalModelsTranslationProvider::detectLanguage(const QString &text)
{
    if (state == State::Ready)
        startRequest(text, true);
    return Language::autoLanguage();
}

void LocalModelsTranslationProvider::translate(const QString &text, const Language &destination, const Language &source)
{
    if (state == State::Processing)
        return;
    if (m_active)
        stopWorker();
    sourceLanguage = source;
    translationLanguage = destination;
    error = TranslationError::NoError;
    errorString.clear();
    result.clear();
    state = State::Processing;
    emit stateChanged(state);
    if (state != State::Processing)
        return;
    // Auto destinations can resolve to the system QLocale, whose territory
    // and number options need not equal the preset locale for the same language.
    // Preserve the requested locale, but validate the language sent to the worker.
    if (!supportedDestinationLanguages().contains(Language(destination.toCode()))) {
        fail(tr("Select English or Japanese as the destination language."));
        return;
    }
    if (source != Language::autoLanguage() && !supportedSourceLanguages().contains(Language(source.toCode()))) {
        fail(tr("Select English or Japanese as the source language."));
        return;
    }
    startRequest(text, false);
}

void LocalModelsTranslationProvider::startRequest(const QString &text, bool detectOnly)
{
    if (m_active)
        stopWorker();
    m_detectOnly = detectOnly;
    m_sourceWasAuto = !detectOnly && sourceLanguage == Language::autoLanguage();
    if (text.trimmed().isEmpty()) {
        fail(tr("There is no text to translate or detect."));
        return;
    }
    if (!modelIds().contains(m_model)) {
        fail(tr("Unknown local translation model: %1").arg(m_model));
        return;
    }
    ++m_requestId;
    m_active = true;
    m_command = {{QStringLiteral("id"), m_requestId}, {QStringLiteral("text"), text},
                 {QStringLiteral("source"), (detectOnly || m_sourceWasAuto) ? QStringLiteral("auto") : sourceLanguage.toCode()},
                 {QStringLiteral("target"), translationLanguage.toCode()}, {QStringLiteral("detect_only"), detectOnly},
                 {QStringLiteral("timeout"), m_timeout}, {QStringLiteral("prompt"), m_prompt},
                 {QStringLiteral("instructions"), m_instructions}};
    m_timer.start((m_timeout + (detectOnly ? 0 : 120)) * 1000);
    if (detectOnly)
        emit detectionStarted();
    if (!m_active)
        return;
    if (m_process && m_process->state() == QProcess::Running) {
        m_process->write(QJsonDocument(m_command).toJson(QJsonDocument::Compact) + '\n');
        return;
    }
    const QString python = OptionalComponents::python(m_root);
    if (!QFileInfo(python).isExecutable()) {
        fail(tr("The local translation environment is not installed. Check the model directory in Translation settings."));
        return;
    }
    m_workerDirectory = std::make_unique<QTemporaryDir>();
    if (!m_workerDirectory->isValid()) {
        fail(tr("Could not create a temporary directory for local translation."));
        return;
    }
    for (const QString &name : {QStringLiteral("worker.py"), QStringLiteral("catalog.json")}) {
        if (!QFile::copy(QStringLiteral(":/local-models/") + name, m_workerDirectory->filePath(name))) {
            fail(tr("Could not prepare the local translation worker."));
            return;
        }
    }
    auto *process = new QProcess(this);
    m_process = process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    process->setProcessEnvironment(environment);
    process->setWorkingDirectory(m_root);
#ifdef Q_OS_WIN
    m_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!m_job || !SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        fail(tr("Could not create the local translation process group."));
        return;
    }
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW | CREATE_SUSPENDED; });
#elif defined(Q_OS_UNIX)
    process->setChildProcessModifier([]() { ::setsid(); });
#endif
    connect(process, &QProcess::started, this, [this, process]() {
#ifdef Q_OS_WIN
        HANDLE handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(process->processId()));
        const bool assigned = handle && AssignProcessToJobObject(m_job, handle);
        if (handle)
            CloseHandle(handle);
        if (!assigned || !resumeWorker(static_cast<DWORD>(process->processId()))) {
            fail(tr("Could not create the local translation process group."));
            return;
        }
#endif
        process->write(QJsonDocument(m_command).toJson(QJsonDocument::Compact) + '\n');
    });
    connect(process, &QProcess::readyReadStandardOutput, this, &LocalModelsTranslationProvider::readEvents);
    connect(process, &QProcess::readyReadStandardError, this, [this, process]() { m_diagnostics = (m_diagnostics + process->readAllStandardError()).right(8192); });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(tr("Could not start local translation: %1").arg(process->errorString()));
    });
    connect(process, &QProcess::finished, this, [this, process]() {
        readEvents();
        if (m_process != process)
            return;
        m_diagnostics = (m_diagnostics + process->readAllStandardError()).right(8192);
        if (m_active)
            fail(tr("The local translation worker stopped: %1").arg(QString::fromUtf8(m_diagnostics).right(2000)));
        else
            stopWorker();
    });
    process->start(python, {QStringLiteral("-u"), m_workerDirectory->filePath(QStringLiteral("worker.py")),
                           QStringLiteral("--root"), m_root, QStringLiteral("--model"), m_model});
}

void LocalModelsTranslationProvider::readEvents()
{
    if (!m_process)
        return;
    m_buffer += m_process->readAllStandardOutput();
    if (m_buffer.size() > 16 * 1024 * 1024) {
        fail(tr("The local translation response is too large."));
        return;
    }
    while (m_process && m_buffer.contains('\n')) {
        const int end = m_buffer.indexOf('\n');
        const QByteArray line = m_buffer.left(end);
        m_buffer.remove(0, end + 1);
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) {
            fail(tr("Invalid local translation response."));
            return;
        }
        handleEvent(document.object());
    }
}

void LocalModelsTranslationProvider::handleEvent(const QJsonObject &event)
{
    if (!m_active || event.value(QStringLiteral("id")).toInteger(-1) != m_requestId)
        return;
    const QString type = event.value(QStringLiteral("event")).toString();
    if (type == QLatin1String("stage")) {
        const QString stage = event.value(QStringLiteral("stage")).toString();
        if (stage == QLatin1String("loading"))
            emit modelLoading();
        else if (stage == QLatin1String("translating")) {
            m_timer.start(m_timeout * 1000);
            emit modelReady();
        }
        return;
    }
    if (type == QLatin1String("error")) {
        fail(event.value(QStringLiteral("message")).toString());
        return;
    }
    const Language detected(event.value(QStringLiteral("source")).toString());
    const QString translated = event.value(QStringLiteral("text")).toString();
    if (type != QLatin1String("result") || !supportedSourceLanguages().contains(detected)
        || (!m_detectOnly && (translated.trimmed().isEmpty() || (!m_sourceWasAuto && detected.toCode() != sourceLanguage.toCode())))) {
        fail(tr("Invalid local translation response."));
        return;
    }
    m_timer.stop();
    m_active = false;
    sourceLanguage = detected;
    if (m_detectOnly) {
        emit languageDetected(detected, false);
        return;
    }
    result.translation = translated;
    error = TranslationError::NoError;
    state = State::Processed;
    if (m_sourceWasAuto)
        emit languageDetected(detected, true);
    if (state == State::Processed && !m_active)
        emit stateChanged(state);
}

void LocalModelsTranslationProvider::stopWorker()
{
    m_timer.stop();
    m_active = false;
    QProcess *process = m_process;
    m_process = nullptr;
    if (process)
        process->disconnect(this);
#ifdef Q_OS_WIN
    if (m_job) {
        CloseHandle(m_job);
        m_job = nullptr;
    }
#elif defined(Q_OS_UNIX)
    if (process && process->processId() > 0)
        ::kill(-static_cast<pid_t>(process->processId()), SIGKILL);
#endif
    if (process) {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
        process->deleteLater();
    }
    m_workerDirectory.reset();
    m_buffer.clear();
    m_diagnostics.clear();
}

void LocalModelsTranslationProvider::fail(const QString &message)
{
    stopWorker();
    result.clear();
    error = TranslationError::Custom;
    errorString = message;
    state = State::Finished;
    emit stateChanged(state);
}

void LocalModelsTranslationProvider::abort()
{
    const bool active = m_active;
    stopWorker();
    if (active || state == State::Processing || state == State::Processed) {
        error = TranslationError::Aborted;
        state = State::Finished;
        emit stateChanged(state);
    }
}

void LocalModelsTranslationProvider::reset()
{
    const bool detecting = m_active && state == State::Ready;
    if (detecting)
        stopWorker();
    ATranslationProvider::reset();
    if (detecting)
        emit stateChanged(state);
}

void LocalModelsTranslationProvider::applyOptions(const ProviderOptions &options)
{
    const QString root = options.getOption(QStringLiteral("root"), m_root).toString();
    const QString model = options.getOption(QStringLiteral("model"), m_model).toString();
    if (root != m_root || model != m_model) {
        abort();
        if (state == State::Finished)
            reset();
    }
    m_root = root;
    if (model != m_model) {
        m_prompt.clear();
        m_instructions.clear();
    }
    m_model = model;
    m_timeout = qBound(1, options.getOption(QStringLiteral("timeout"), m_timeout).toInt(), 3600);
    m_prompt = options.getOption(QStringLiteral("prompt"), m_prompt).toString();
    m_instructions = options.getOption(QStringLiteral("instructions"), m_instructions).toString();
}

std::unique_ptr<ProviderOptions> LocalModelsTranslationProvider::getDefaultOptions() const
{
    auto options = std::make_unique<ProviderOptions>();
    options->setOption(QStringLiteral("root"), defaultRoot());
    options->setOption(QStringLiteral("model"), QStringLiteral("index-translate-2b"));
    options->setOption(QStringLiteral("timeout"), 300);
    options->setOption(QStringLiteral("prompt"), QString());
    options->setOption(QStringLiteral("instructions"), QString());
    return options;
}
QStringList LocalModelsTranslationProvider::getAvailableOptions() const
{
    return {QStringLiteral("root"), QStringLiteral("model"), QStringLiteral("timeout"), QStringLiteral("prompt"), QStringLiteral("instructions")};
}
void LocalModelsTranslationProvider::saveOptionToSettings(const QString &key, const QVariant &value)
{
    AppSettings settings;
    if (key == QLatin1String("root"))
        settings.setLocalModelsRoot(value.toString());
    else if (key == QLatin1String("model"))
        settings.setLocalModelsModel(value.toString());
    else if (key == QLatin1String("timeout"))
        settings.setLocalModelsTimeout(value.toInt());
    else if (key == QLatin1String("prompt"))
        settings.setLocalModelsPrompt(m_model, value.toString());
    else if (key == QLatin1String("instructions"))
        settings.setLocalModelsInstructions(m_model, value.toString());
}
