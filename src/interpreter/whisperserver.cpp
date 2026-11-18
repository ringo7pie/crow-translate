/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "whisperserver.h"

#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTcpServer>
#include <QThread>
#include <QUrl>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
constexpr int healthIntervalMs = 500; // 起動完了を調べる間隔（ミリ秒）
constexpr int healthMaxTries = 360; // 起動完了を待つ最大回数（約 3 分。モデルの読み込みを含む）
constexpr int requestTimeoutMs = 60000; // 文字起こし 1 件の打ち切り時間（ミリ秒）
constexpr int stderrTailChars = 2000; // 失敗時に残す whisper-server の出力の文字数
constexpr int beamSize = 5; // ビームサーチの幅

// loopback で空いているポートを 1 つ返す。取れなければ 0
int findFreePort()
{
    QTcpServer probe; // 空きポートを調べるための一時的な待ち受け
    if (!probe.listen(QHostAddress::LocalHost, 0))
        return 0;
    const int port = probe.serverPort(); // OS が割り当てたポート
    probe.close();
    return port;
}
}

WhisperServer::WhisperServer(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    qRegisterMetaType<WhisperServer::Transcript>();
    m_healthTimer.setInterval(healthIntervalMs);
    connect(&m_healthTimer, &QTimer::timeout, this, &WhisperServer::pollHealth);
}

WhisperServer::~WhisperServer()
{
    stop();
}

// whisper-server を起動する
void WhisperServer::start(const QString &executable, const QString &model)
{
    stop();
    if (!QFileInfo::exists(executable)) {
        fail(tr("The speech recognition engine (whisper-server) is not installed. Install it from Local AI management."));
        return;
    }
    if (!QFileInfo::exists(model)) {
        fail(tr("The speech recognition model is not installed. Install it from Local AI management."));
        return;
    }
    m_port = findFreePort();
    if (m_port == 0) {
        fail(tr("Could not find a free local port for speech recognition."));
        return;
    }

    // windows.h の max マクロと衝突しないよう (std::max) と括弧で囲む
    const int threads = (std::max)(2,QThread::idealThreadCount() / 2); // CPU で動くときのスレッド数
    const QStringList arguments{
        QStringLiteral("-m"), QDir::toNativeSeparators(model),
        QStringLiteral("--host"), QStringLiteral("127.0.0.1"),
        QStringLiteral("--port"), QString::number(m_port),
        QStringLiteral("-l"), QStringLiteral("auto"),
        QStringLiteral("-t"), QString::number(threads),
        QStringLiteral("-bs"), QString::number(beamSize), // ビームサーチの幅（精度を上げる）
        QStringLiteral("-bo"), QString::number(beamSize), // 温度を上げてやり直すときの候補数
        QStringLiteral("-sns"), // 非音声の記号（音楽など）を出さない
    }; // whisper-server の起動引数

    auto *process = new QProcess(this); // 常駐させる whisper-server
    m_process = process;
    process->setProgram(executable);
    process->setArguments(arguments);
    process->setWorkingDirectory(QFileInfo(executable).absolutePath());
    process->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    m_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; // 親が閉じたら子も止める設定
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!m_job || !SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        fail(tr("Could not create the speech recognition process group."));
        return;
    }
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    connect(process, &QProcess::started, this, [this, process]() {
#ifdef Q_OS_WIN
        HANDLE handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(process->processId()));
        const bool assigned = handle && AssignProcessToJobObject(m_job, handle); // ジョブに入れられたか
        if (handle)
            CloseHandle(handle);
        if (!assigned) {
            fail(tr("Could not create the speech recognition process group."));
            return;
        }
#endif
        m_healthTries = 0;
        m_healthTimer.start();
    });
    connect(process, &QProcess::readyRead, this, [this, process]() {
        m_stderrTail += QString::fromLocal8Bit(process->readAll());
        if (m_stderrTail.size() > stderrTailChars)
            m_stderrTail = m_stderrTail.right(stderrTailChars);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && process == m_process)
            fail(tr("Could not start the speech recognition engine: %1").arg(process->errorString()));
    });
    connect(process, &QProcess::finished, this, [this, process](int exitCode) {
        if (process != m_process)
            return;
        fail(tr("The speech recognition engine stopped (exit code %1).\n%2").arg(exitCode).arg(m_stderrTail.trimmed()));
    });
    process->start();
}

// whisper-server を止める
void WhisperServer::stop()
{
    m_healthTimer.stop();
    m_ready = false;
    if (m_process) {
        QProcess *process = m_process; // 止める対象
        m_process = nullptr; // finished で fail() が出ないよう先に外す
        process->disconnect(this);
        process->kill();
        process->waitForFinished(3000);
        process->deleteLater();
    }
    closeJob();
}

bool WhisperServer::isReady() const
{
    return m_ready;
}

// 起動完了を調べる。HTTP の応答が返れば準備完了とみなす
void WhisperServer::pollHealth()
{
    if (!m_process)
        return;
    if (++m_healthTries > healthMaxTries) {
        fail(tr("The speech recognition engine did not become ready in time.\n%1").arg(m_stderrTail.trimmed()));
        return;
    }
    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(m_port))); // 応答確認の要求
    request.setTransferTimeout(healthIntervalMs * 2);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (m_ready || !m_process)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); // HTTP の状態コード
        if (status > 0) {
            m_healthTimer.stop();
            m_ready = true;
            emit ready();
        }
    });
}

// 文字起こしを送る
void WhisperServer::transcribe(quint64 id, const QVector<float> &samples, const QString &languageHint, const QString &prompt)
{
    if (!m_ready) {
        emit transcriptionFailed(id, tr("Speech recognition is not ready."));
        return;
    }
    auto *multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType); // 送信する form-data
    const auto addField = [multipart](const QString &name, const QByteArray &value) {
        QHttpPart part; // form-data の 1 項目
        part.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"%1\"").arg(name));
        part.setBody(value);
        multipart->append(part);
    };
    QHttpPart filePart; // 音声ファイルの項目
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("audio/wav"));
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"file\"; filename=\"speech.wav\""));
    filePart.setBody(toWav(samples));
    multipart->append(filePart);
    addField(QStringLiteral("temperature"), "0.0");
    addField(QStringLiteral("temperature_inc"), "0.2"); // 失敗時に温度を上げてやり直す幅
    addField(QStringLiteral("beam_size"), QByteArray::number(beamSize));
    addField(QStringLiteral("suppress_non_speech"), "true");
    addField(QStringLiteral("response_format"), "verbose_json");
    addField(QStringLiteral("language"), languageHint.isEmpty() ? QByteArray("auto") : languageHint.toUtf8());
    if (!prompt.isEmpty())
        addField(QStringLiteral("prompt"), prompt.toUtf8()); // 文脈と用語の手がかり

    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/inference").arg(m_port))); // 文字起こしの要求
    request.setTransferTimeout(requestTimeoutMs);
    QNetworkReply *reply = m_network->post(request, multipart);
    multipart->setParent(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, id]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit transcriptionFailed(id, reply->errorString());
            return;
        }
        const QByteArray body = reply->readAll(); // 応答の本文
        const QJsonDocument document = QJsonDocument::fromJson(body); // 応答の JSON
        if (!document.isObject()) {
            emit transcriptionFailed(id, tr("Unexpected speech recognition response."));
            return;
        }
        const QJsonObject object = document.object();
        if (object.contains(QStringLiteral("error"))) {
            emit transcriptionFailed(id, object.value(QStringLiteral("error")).toVariant().toString());
            return;
        }
        Transcript transcript; // 返す結果
        transcript.id = id;
        transcript.text = object.value(QStringLiteral("text")).toString().trimmed();
        transcript.language = normalizeLanguage(object.value(QStringLiteral("language")).toString());
        if (transcript.language.isEmpty())
            transcript.language = guessLanguage(transcript.text);
        emit transcribed(transcript);
    });
}

// 16 kHz mono float の音声を 16 bit PCM の WAV にする
QByteArray WhisperServer::toWav(const QVector<float> &samples, int sampleRate)
{
    QByteArray wav; // 出力する WAV
    const quint32 dataBytes = static_cast<quint32>(samples.size()) * 2; // PCM 部分のバイト数
    wav.reserve(44 + static_cast<int>(dataBytes));
    QDataStream stream(&wav, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + dataBytes);
    stream.writeRawData("WAVE", 4);
    stream.writeRawData("fmt ", 4);
    stream << quint32(16) << quint16(1) << quint16(1) << quint32(sampleRate) << quint32(sampleRate * 2) << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << dataBytes;
    for (const float sample : samples) {
        const float clamped = std::clamp(sample, -1.0f, 1.0f); // 範囲外を切った値
        stream << qint16(clamped * 32767.0f);
    }
    return wav;
}

// whisper の言語表記を ISO 639-1 にする
QString WhisperServer::normalizeLanguage(const QString &value)
{
    static const QHash<QString, QString> names{
        {QStringLiteral("english"), QStringLiteral("en")},
        {QStringLiteral("japanese"), QStringLiteral("ja")},
        {QStringLiteral("chinese"), QStringLiteral("zh")},
        {QStringLiteral("korean"), QStringLiteral("ko")},
        {QStringLiteral("german"), QStringLiteral("de")},
        {QStringLiteral("french"), QStringLiteral("fr")},
        {QStringLiteral("spanish"), QStringLiteral("es")},
        {QStringLiteral("italian"), QStringLiteral("it")},
        {QStringLiteral("portuguese"), QStringLiteral("pt")},
        {QStringLiteral("russian"), QStringLiteral("ru")},
        {QStringLiteral("vietnamese"), QStringLiteral("vi")},
        {QStringLiteral("thai"), QStringLiteral("th")},
        {QStringLiteral("indonesian"), QStringLiteral("id")},
    }; // whisper の英語名 → ISO 639-1
    const QString lower = value.trimmed().toLower(); // 比べるための小文字
    if (lower.size() == 2)
        return lower;
    return names.value(lower);
}

// 文字の種類から言語を推定する
QString WhisperServer::guessLanguage(const QString &text)
{
    int kana = 0; // 仮名の数
    int han = 0; // 漢字の数
    int hangul = 0; // ハングルの数
    int latin = 0; // ラテン文字の数
    for (const QChar character : text) {
        const ushort code = character.unicode(); // 文字コード
        if ((code >= 0x3040 && code <= 0x30FF) || (code >= 0xFF66 && code <= 0xFF9F))
            ++kana;
        else if (code >= 0x4E00 && code <= 0x9FFF)
            ++han;
        else if (code >= 0xAC00 && code <= 0xD7AF)
            ++hangul;
        else if ((code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z'))
            ++latin;
    }
    if (kana > 0)
        return QStringLiteral("ja");
    if (hangul > 0)
        return QStringLiteral("ko");
    if (han > 0)
        return QStringLiteral("zh");
    if (latin > 0)
        return QStringLiteral("en");
    return {};
}

// 失敗を知らせて止める
void WhisperServer::fail(const QString &error)
{
    stop();
    emit failed(error);
}

// ジョブオブジェクトを閉じる（中の whisper-server も止まる）
void WhisperServer::closeJob()
{
#ifdef Q_OS_WIN
    if (m_job) {
        CloseHandle(m_job);
        m_job = nullptr;
    }
#endif
}
