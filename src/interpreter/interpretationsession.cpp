/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "interpretationsession.h"

#include "audiocapture.h"
#include "audionormalizer.h"
#include "provideroptions.h"
#include "speakerembedder.h"
#include "speechoutput.h"
#include "whisperserver.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QThread>

#include <algorithm>

namespace {
constexpr int sampleRate = 16000; // 発話の標本化周波数
constexpr int promptContextChars = 200; // 手がかりに入れる直前の発言の文字数
constexpr int speakerUnavailable = -2; // 話者を聞き分けられなかった（部品の読み込み失敗など）
}

InterpretationSession::InterpretationSession(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<InterpretationSession::Speaker>();
    qRegisterMetaType<WhisperServer::Transcript>();
}

InterpretationSession::~InterpretationSession()
{
    stop();
}

bool InterpretationSession::isRunning() const
{
    return m_running;
}

// 原文を置く列を決める。判定言語が A/B のどちらでもなければ話者の既定の列
int InterpretationSession::columnFor(Speaker speaker, const QString &detectedLanguage, const Language &languageA, const Language &languageB)
{
    const QString detected = detectedLanguage.left(2).toLower(); // 判定言語の 2 文字コード
    if (!detected.isEmpty()) {
        if (detected == languageA.toIso639_1())
            return 0;
        if (detected == languageB.toIso639_1())
            return 1;
    }
    return speaker == Speaker::Self ? 0 : 1;
}

// whisper の幻聴を取り除く
QString InterpretationSession::cleanTranscript(const QString &text)
{
    static const QRegularExpression bracketed(QStringLiteral(R"(\[[^\]]*\]|\([^)]*\)|（[^）]*）|【[^】]*】|♪+)")); // 括弧で囲まれた注記
    QString cleaned = text; // 整えた文
    cleaned.remove(bracketed);
    cleaned = cleaned.simplified();
    // 句読点・記号だけなら捨てる
    bool hasLetter = false; // 文字（記号以外）を含むか
    for (const QChar character : std::as_const(cleaned)) {
        if (character.isLetterOrNumber()) {
            hasLetter = true;
            break;
        }
    }
    return hasLetter ? cleaned : QString();
}

// 判定言語が A・B のどちらでもなければ、言語を指定して認識し直す
bool InterpretationSession::needsLanguageRetry(const QString &detectedLanguage, const Language &languageA, const Language &languageB)
{
    const QString detected = detectedLanguage.left(2).toLower(); // 判定言語の 2 文字コード
    const QString codeA = languageA.toIso639_1(); // 言語 A のコード
    const QString codeB = languageB.toIso639_1(); // 言語 B のコード
    if (detected.isEmpty() || codeA.isEmpty() || codeB.isEmpty())
        return false;
    return detected != codeA && detected != codeB;
}

// 用語と直前の発言から whisper の手がかりを作る
QString InterpretationSession::buildPrompt(const QString &glossary, const QString &previousText)
{
    QStringList parts; // 手がかりの部品
    const QString terms = glossary.simplified(); // 用語
    if (!terms.isEmpty())
        parts.append(terms);
    const QString previous = previousText.simplified(); // 直前の発言
    if (!previous.isEmpty())
        parts.append(previous.right(promptContextChars));
    return parts.join(QLatin1Char(' '));
}

// 通訳を始める
void InterpretationSession::start(const Config &config)
{
    stop();
    m_config = config;
    m_sourceWarning.clear();
    if (!m_config.captureMicrophone && !m_config.captureRemote) {
        emit errorOccurred(tr("Select at least one audio source."));
        return;
    }
    m_running = true;
    m_clock.start();
    emit runningChanged(true);
    emit statusChanged(tr("Starting speech recognition…"));

    // 文字起こし
    startWhisper();
    if (!m_running)
        return; // 起動に失敗して止まった

    // 話者の聞き分け（相手の声だけ）
    if (m_config.captureRemote && m_config.speakerIdentification)
        startSpeakerIdentification();

    // 翻訳と読み上げ
    startTranslator();
    startSpeechOutputs();

    // 取り込みと発話区切り
    // マイクの失敗はまだ外さない。会議の音声を用意する前に外すと、取り込み元が残っていないと判断して止まるため
    const QString microphoneFailure = m_config.captureMicrophone ? startMicrophone() : QString(); // マイクを開けなかった理由
    if (m_config.captureRemote) {
        startLoopback();
        if (!m_running)
            return;
    }
    // マイクだけ外す。会議の音声も使わない設定なら、ここで止まる
    if (!microphoneFailure.isEmpty()) {
        dropSource(Speaker::Self, microphoneFailure);
        if (!m_running)
            return;
    }
    reportQueues();
}

// 動作中に設定を変える
void InterpretationSession::applyConfig(const Config &config)
{
    if (!m_running)
        return;
    const Config previous = m_config; // 変える前の設定
    m_config = config; // 言語・用語は次の発話から m_config を通して効く

    // 発話区切りと、同じ人とみなす類似度（そのまま反映）
    for (VoiceSegmenter *segmenter : {m_selfSegmenter, m_remoteSegmenter}) {
        if (segmenter)
            segmenter->setSettings(m_config.vad);
    }
    m_clusterer.setThreshold(m_config.speakerThreshold);

    // 翻訳
    if (previous.backend != m_config.backend || previous.translationOptions != m_config.translationOptions)
        restartTranslator();

    // 読み上げ（出力先や方式が変わったら作り直す。速さだけならそのまま反映）
    const bool speechChanged = previous.selfVoiceMode != m_config.selfVoiceMode || previous.virtualOutputId != m_config.virtualOutputId
        || previous.mixOriginalVoice != m_config.mixOriginalVoice || previous.speakRemoteTranslation != m_config.speakRemoteTranslation
        || previous.speakerOutputId != m_config.speakerOutputId; // 読み上げを作り直すか
    if (speechChanged) {
        releaseSpeechOutputs();
        startSpeechOutputs();
    } else if (previous.speechRate != m_config.speechRate) {
        for (SpeechOutput *speech : {m_selfSpeech, m_remoteSpeech}) {
            if (speech)
                speech->setRate(m_config.speechRate);
        }
    }

    // 取り込み元（会議の音声を先に用意する。マイクの失敗で、取り込み元が無いと誤って判断しないため）
    const bool remoteChanged = previous.captureRemote != m_config.captureRemote || previous.remoteOutputId != m_config.remoteOutputId; // 会議の音声を作り直すか
    const bool microphoneChanged = previous.captureMicrophone != m_config.captureMicrophone || previous.microphoneId != m_config.microphoneId; // マイクを作り直すか
    if (remoteChanged || microphoneChanged)
        m_sourceWarning.clear(); // 作り直した取り込み元の警告は、作り直した結果で出し直す
    if (remoteChanged) {
        releaseSource(Speaker::Remote);
        if (m_config.captureRemote)
            startLoopback();
        if (!m_running)
            return;
    }
    if (microphoneChanged) {
        releaseSource(Speaker::Self);
        const QString failure = m_config.captureMicrophone ? startMicrophone() : QString(); // マイクを開けなかった理由
        if (!failure.isEmpty())
            dropSource(Speaker::Self, failure);
        if (!m_running)
            return;
    }
    if (!m_microphone && !m_loopback) {
        fail(tr("No audio source is available."));
        return;
    }

    // 話者の聞き分け（会議の音声を取り込むときだけ）
    const bool identifyBefore = previous.captureRemote && previous.speakerIdentification; // 前は聞き分けていたか
    const bool identifyNow = m_config.captureRemote && m_config.speakerIdentification; // これから聞き分けるか
    if (identifyNow && !identifyBefore)
        startSpeakerIdentification();
    else if (!identifyNow && identifyBefore)
        disableSpeakerIdentification();

    // 認識モデル（whisper-server を起動し直す。準備ができると「聞き取り中」に戻る）
    if (previous.whisperModel != m_config.whisperModel || previous.whisperExecutable != m_config.whisperExecutable) {
        restartWhisper();
        if (!m_running)
            return;
    } else if (m_whisper && m_whisper->isReady()) {
        emit statusChanged(listeningStatus());
    }
    reportQueues();
}

// 通訳を止める
void InterpretationSession::stop()
{
    const bool wasRunning = m_running; // 止める前に動いていたか
    m_running = false;
    m_sourceWarning.clear();
    // 取り込みを先に止め、新しい発話を作らない
    releaseSource(Speaker::Self);
    releaseSource(Speaker::Remote);
    releaseSpeechOutputs();
    if (m_whisper) {
        m_whisper->disconnect(this);
        m_whisper->stop();
        m_whisper->deleteLater();
        m_whisper = nullptr;
    }
    stopSpeakerIdentification();
    if (m_translator) {
        m_translator->disconnect(this);
        m_translator->abort();
        m_translator->deleteLater();
        m_translator = nullptr;
    }
    m_utterances.clear();
    m_inFlight.clear();
    m_translations.clear();
    m_translating = false;
    m_previousText.clear();
    m_waiting = WaitingTranscript();
    m_speakerResults.clear();
    m_droppedSpeakerIds.clear();
    if (wasRunning) {
        emit statusChanged(tr("Stopped"));
        emit runningChanged(false);
        emit queueChanged(0, 0, 0);
    }
}

// 失敗を知らせて止める
void InterpretationSession::fail(const QString &error)
{
    // 先に止める。stop() が出す「停止しました」で理由が上書きされないよう、理由は後から知らせる
    stop();
    emit errorOccurred(error);
}

// 取り込み元を 1 つ片付ける
void InterpretationSession::releaseSource(Speaker source)
{
    const bool self = source == Speaker::Self; // マイク側か
    AudioCapture *&capture = self ? m_microphone : m_loopback; // 片付ける取り込み
    VoiceSegmenter *&segmenter = self ? m_selfSegmenter : m_remoteSegmenter; // 片付ける発話区切り
    if (capture) {
        capture->disconnect(this);
        capture->stop();
        capture->deleteLater(); // 自分の signal の中から呼ばれることがあるので、すぐには消さない
        capture = nullptr;
    }
    if (segmenter) {
        segmenter->disconnect(this);
        segmenter->deleteLater();
        segmenter = nullptr;
    }
}

// 失敗した取り込み元だけ外して続ける
void InterpretationSession::dropSource(Speaker source, const QString &reason)
{
    if (!m_running)
        return;
    releaseSource(source);
    if (!m_microphone && !m_loopback) {
        fail(reason); // 使える取り込み元が残っていない
        return;
    }
    // 話者の聞き分けは止めない（待っている文字起こしが聞き分けの結果を待ち続けないように）
    if (source == Speaker::Self)
        m_sourceWarning = tr("The microphone is not used (%1). Continuing with the meeting audio only.").arg(reason);
    else
        m_sourceWarning = tr("The meeting audio is not used (%1). Continuing with the microphone only.").arg(reason);
    emit errorOccurred(m_sourceWarning);
}

// whisper-server を起動する
void InterpretationSession::startWhisper()
{
    m_whisper = new WhisperServer(this);
    connect(m_whisper, &WhisperServer::ready, this, [this]() {
        // 外した取り込み元があれば、その警告を「聞き取り中」で消さずに添える
        emit statusChanged(listeningStatus());
        transcribeNext();
    });
    connect(m_whisper, &WhisperServer::failed, this, [this](const QString &error) {
        fail(tr("Speech recognition stopped: %1").arg(error));
    });
    connect(m_whisper, &WhisperServer::transcribed, this, [this](const WhisperServer::Transcript &transcript) {
        handleTranscript(transcript.id, transcript.text, transcript.language);
    });
    connect(m_whisper, &WhisperServer::transcriptionFailed, this, [this](quint64 id, const QString &error) {
        m_inFlight.remove(id);
        if (!m_speakerResults.remove(id))
            m_droppedSpeakerIds.insert(id);
        emit errorOccurred(tr("Speech recognition failed: %1").arg(error));
        transcribeNext();
    });
    m_whisper->start(m_config.whisperExecutable, m_config.whisperModel);
}

// whisper-server を起動し直す
void InterpretationSession::restartWhisper()
{
    if (m_whisper) {
        m_whisper->disconnect(this);
        m_whisper->stop();
        m_whisper->deleteLater();
        m_whisper = nullptr;
    }
    // 文字起こし中だった発話を、発話の順を保って待ちの先頭へ戻す
    QList<Utterance> unfinished = m_inFlight.values(); // 文字起こし中だった発話
    m_inFlight.clear();
    std::sort(unfinished.begin(), unfinished.end(), [](const Utterance &left, const Utterance &right) {
        return left.id < right.id;
    });
    for (auto utterance = unfinished.crbegin(); utterance != unfinished.crend(); ++utterance)
        m_utterances.prepend(*utterance);
    emit statusChanged(tr("Starting speech recognition…"));
    startWhisper();
}

// 翻訳の方式を作る
void InterpretationSession::startTranslator()
{
    m_translator = ATranslationProvider::createTranslationProvider(this, m_config.backend);
    ProviderOptions options; // 通訳専用の翻訳設定（メインの翻訳設定とは別）
    for (auto option = m_config.translationOptions.cbegin(); option != m_config.translationOptions.cend(); ++option)
        options.setOption(option.key(), option.value());
    m_translator->applyOptions(options);
    connect(m_translator, &ATranslationProvider::stateChanged, this, &InterpretationSession::handleTranslationState);
}

// 翻訳の方式を作り直す
void InterpretationSession::restartTranslator()
{
    if (m_translator) {
        m_translator->disconnect(this);
        m_translator->abort();
        m_translator->deleteLater();
        m_translator = nullptr;
    }
    if (m_translating) {
        m_translations.prepend(m_currentJob); // 訳しかけの文を新しい方式で訳し直す
        m_translating = false;
    }
    startTranslator();
    translateNext();
}

// 読み上げと元の声の中継を作る
void InterpretationSession::startSpeechOutputs()
{
    if (m_config.selfVoiceMode == SelfVoiceMode::SendTranslatedSpeech) {
        m_selfSpeech = new SpeechOutput(this);
        m_selfSpeech->setDevice(m_config.virtualOutputId);
        m_selfSpeech->setRate(m_config.speechRate);
        connect(m_selfSpeech, &SpeechOutput::errorOccurred, this, &InterpretationSession::errorOccurred);
        if (!m_selfSpeech->canRouteToDevice())
            emit errorOccurred(tr("This speech engine cannot send speech to a chosen device; the default output is used."));
        if (m_config.mixOriginalVoice) {
            m_passthrough = new AudioPassthrough(this);
            if (!m_passthrough->start(m_config.virtualOutputId))
                emit errorOccurred(tr("Could not open the virtual audio device for the original voice."));
        }
    }
    if (m_config.speakRemoteTranslation) {
        m_remoteSpeech = new SpeechOutput(this);
        m_remoteSpeech->setDevice(m_config.speakerOutputId);
        m_remoteSpeech->setRate(m_config.speechRate);
        connect(m_remoteSpeech, &SpeechOutput::errorOccurred, this, &InterpretationSession::errorOccurred);
        // 読み上げ中は再生音の取り込みを止め、読み上げを相手の声として拾わない
        connect(m_remoteSpeech, &SpeechOutput::speakingChanged, this, [this](bool speaking) {
            if (m_loopback)
                m_loopback->setMuted(speaking);
            if (speaking && m_remoteSegmenter)
                m_remoteSegmenter->reset();
        });
    }
}

// 読み上げと元の声の中継を片付ける
void InterpretationSession::releaseSpeechOutputs()
{
    for (SpeechOutput **speech : {&m_selfSpeech, &m_remoteSpeech}) {
        if (*speech) {
            (*speech)->disconnect(this);
            (*speech)->clear();
            (*speech)->deleteLater();
            *speech = nullptr;
        }
    }
    if (m_passthrough) {
        m_passthrough->stop();
        m_passthrough->deleteLater();
        m_passthrough = nullptr;
    }
    // 読み上げ中に片付けると speakingChanged(false) が届かないので、会議の音声の取り込みを戻す
    if (m_loopback)
        m_loopback->setMuted(false);
}

// マイクの取り込みを始める
QString InterpretationSession::startMicrophone()
{
    m_selfSegmenter = new VoiceSegmenter(this);
    m_selfSegmenter->setSettings(m_config.vad);
    connect(m_selfSegmenter, &VoiceSegmenter::utteranceReady, this, [this](const QVector<float> &samples) {
        addUtterance(Speaker::Self, samples);
    });
    m_microphone = new AudioCapture(AudioCapture::Kind::Microphone, m_config.microphoneId, this);
    connect(m_microphone, &AudioCapture::samplesReady, this, [this](const QVector<float> &samples) {
        if (m_selfSegmenter)
            m_selfSegmenter->push(samples);
        if (m_passthrough)
            m_passthrough->write(samples);
    });
    connect(m_microphone, &AudioCapture::errorOccurred, this, &InterpretationSession::errorOccurred);
    if (m_microphone->start())
        return QString();
    QString failure = tr("Could not start the microphone."); // 失敗の説明
    const QString detail = m_microphone->lastError(); // AudioCapture が知らせた理由
    if (!detail.isEmpty())
        failure += QLatin1Char(' ') + detail;
    return failure;
}

// 会議の音声の取り込みを始める
void InterpretationSession::startLoopback()
{
    m_remoteSegmenter = new VoiceSegmenter(this);
    m_remoteSegmenter->setSettings(m_config.vad);
    connect(m_remoteSegmenter, &VoiceSegmenter::utteranceReady, this, [this](const QVector<float> &samples) {
        addUtterance(Speaker::Remote, samples);
    });
    m_loopback = new AudioCapture(AudioCapture::Kind::Loopback, m_config.remoteOutputId, this);
    connect(m_loopback, &AudioCapture::samplesReady, this, [this](const QVector<float> &samples) {
        if (m_remoteSegmenter)
            m_remoteSegmenter->push(samples);
    });
    // ループバックの失敗は取り込み用のスレッドから後で届く。開始時・実行中を問わず、会議の音声だけ外す
    connect(m_loopback, &AudioCapture::errorOccurred, this, [this](const QString &error) {
        dropSource(Speaker::Remote, error);
    });
    // start() の中で失敗を知らせた場合は、上の接続で既に外れている（m_loopback が空になる）
    if (!m_loopback->start() && m_loopback)
        dropSource(Speaker::Remote, tr("Could not capture the meeting audio."));
}

// 話者の聞き分けを途中でやめる
void InterpretationSession::disableSpeakerIdentification()
{
    stopSpeakerIdentification(); // 世代が変わるので、後から届く結果は捨てられる
    for (Utterance &utterance : m_utterances)
        utterance.identifySpeaker = false;
    for (Utterance &utterance : m_inFlight)
        utterance.identifySpeaker = false;
    m_speakerResults.clear();
    m_droppedSpeakerIds.clear();
    if (m_waiting.active) {
        WaitingTranscript waiting = m_waiting; // 聞き分けの結果を待っていた文字起こし
        m_waiting = WaitingTranscript();
        waiting.utterance.identifySpeaker = false;
        emitEntry(waiting.utterance, waiting.text, waiting.language);
        transcribeNext();
    }
}

// 「聞き取り中」の表示
QString InterpretationSession::listeningStatus() const
{
    if (m_sourceWarning.isEmpty())
        return tr("Listening");
    return tr("Listening — %1").arg(m_sourceWarning);
}

// 発話を文字起こし待ちに入れる
void InterpretationSession::addUtterance(Speaker speaker, const QVector<float> &samples)
{
    if (!m_running)
        return;
    Utterance utterance; // 新しい発話
    utterance.id = m_nextId++;
    utterance.speaker = speaker;
    utterance.samples = AudioNormalizer::normalize(samples); // 音量をそろえてから認識する
    utterance.endedAt = m_clock.elapsed();
    utterance.identifySpeaker = speaker == Speaker::Remote && m_identifySpeakers;
    if (utterance.identifySpeaker)
        requestEmbedding(utterance.id, utterance.samples); // 文字起こしと並行して話者を聞き分ける
    m_utterances.enqueue(utterance);
    transcribeNext();
    reportQueues();
}

// 文字起こしを 1 つずつ順番に出す（表示の順を発話の順に保つ）
void InterpretationSession::transcribeNext()
{
    if (!m_running || !m_whisper || !m_whisper->isReady() || !m_inFlight.isEmpty() || m_waiting.active || m_utterances.isEmpty())
        return;
    const Utterance utterance = m_utterances.dequeue(); // 次に文字起こしする発話
    m_inFlight.insert(utterance.id, utterance);
    const QString prompt = buildPrompt(m_config.glossary, m_previousText.value(int(utterance.speaker))); // 用語と文脈の手がかり
    m_whisper->transcribe(utterance.id, utterance.samples, QString(), prompt);
    reportQueues();
}

// 文字起こしの結果を表に出し、翻訳待ちに入れる
void InterpretationSession::handleTranscript(quint64 id, const QString &text, const QString &language)
{
    const auto found = m_inFlight.find(id); // 対応する発話
    if (found == m_inFlight.end()) {
        transcribeNext();
        return;
    }
    const QString cleaned = cleanTranscript(text); // 幻聴を除いた文
    // 言語 A・B のどちらでもない言語と判定されたら、話者の既定の言語を指定して 1 回だけ認識し直す
    if (!cleaned.isEmpty() && !found->retried && needsLanguageRetry(language, m_config.languageA, m_config.languageB)) {
        found->retried = true;
        const Language &fallback = found->speaker == Speaker::Self ? m_config.languageA : m_config.languageB; // 指定する言語
        qInfo("Interpreter: utterance %llu detected as %s; retrying as %s", id, qPrintable(language), qPrintable(fallback.toIso639_1()));
        const QString prompt = buildPrompt(m_config.glossary, m_previousText.value(int(found->speaker))); // 手がかり
        m_whisper->transcribe(id, found->samples, fallback.toIso639_1(), prompt);
        return;
    }
    Utterance utterance = found.value(); // 文字起こしした発話
    m_inFlight.erase(found);
    utterance.samples.clear();
    if (cleaned.isEmpty()) {
        if (utterance.identifySpeaker && !m_speakerResults.remove(id))
            m_droppedSpeakerIds.insert(id); // 後から届く聞き分けの結果を捨てる
    } else if (utterance.identifySpeaker && !m_speakerResults.contains(id)) {
        // 話者の聞き分けがまだ終わっていない。終わるまで表示を待つ（表示の順を保つ）
        m_waiting.active = true;
        m_waiting.utterance = utterance;
        m_waiting.text = cleaned;
        m_waiting.language = language;
        reportQueues();
        return;
    } else {
        emitEntry(utterance, cleaned, language);
    }
    transcribeNext();
    reportQueues();
}

// 原文を表に出し、翻訳待ちに入れる
void InterpretationSession::emitEntry(const Utterance &utterance, const QString &text, const QString &language)
{
    const int column = columnFor(utterance.speaker, language, m_config.languageA, m_config.languageB); // 原文の列
    qInfo("Interpreter latency: utterance %llu recognized after %lld ms (%s)", utterance.id, m_clock.elapsed() - utterance.endedAt, qPrintable(language));
    QString label; // 表示する話者名
    int speakerIndex = -1; // 相手の話者の番号
    if (utterance.speaker == Speaker::Self) {
        label = tr("Me");
    } else if (utterance.identifySpeaker) {
        speakerIndex = m_speakerResults.take(utterance.id);
        if (speakerIndex != speakerUnavailable)
            label = tr("Speaker %1").arg(SpeakerClusterer::letterFor(speakerIndex));
        else
            speakerIndex = -1;
    }
    m_previousText.insert(int(utterance.speaker), text);
    emit entryAdded(utterance.id, utterance.speaker, column, text, QDateTime::currentDateTime(), label, speakerIndex);
    TranslationJob job; // 翻訳の依頼
    job.id = utterance.id;
    job.speaker = utterance.speaker;
    job.text = text;
    job.source = column == 0 ? m_config.languageA : m_config.languageB;
    job.destination = column == 0 ? m_config.languageB : m_config.languageA;
    job.endedAt = utterance.endedAt;
    m_translations.enqueue(job);
    translateNext();
}

// 話者の聞き分けを始める（埋め込みの計算用のスレッドを立てる。部品の読み込みは最初の発話のときにそのスレッドで行う）
void InterpretationSession::startSpeakerIdentification()
{
    stopSpeakerIdentification();
    m_clusterer.reset();
    m_clusterer.setThreshold(m_config.speakerThreshold);
    m_embedder = std::make_shared<SpeakerEmbedder>();
    m_speakerThread = new QThread(this);
    m_speakerThread->setObjectName(QStringLiteral("SpeakerEmbedding"));
    m_speakerWorker = new QObject;
    m_speakerWorker->moveToThread(m_speakerThread);
    connect(m_speakerThread, &QThread::finished, m_speakerWorker, &QObject::deleteLater);
    m_speakerThread->start(QThread::LowPriority);
    m_identifySpeakers = true;
    ++m_speakerGeneration;
}

// 話者の聞き分けを止める（計算中の 1 件が終わるのを待つ）
void InterpretationSession::stopSpeakerIdentification()
{
    m_identifySpeakers = false;
    ++m_speakerGeneration;
    if (m_speakerThread) {
        m_speakerThread->quit();
        m_speakerThread->wait();
        delete m_speakerThread;
        m_speakerThread = nullptr;
        m_speakerWorker = nullptr; // finished で deleteLater 済み
    }
    m_embedder.reset();
}

// 埋め込みの計算を頼む（発話の順に 1 つずつ計算される）
void InterpretationSession::requestEmbedding(quint64 id, const QVector<float> &samples)
{
    if (!m_speakerWorker || !m_embedder)
        return;
    const std::shared_ptr<SpeakerEmbedder> embedder = m_embedder; // 計算するスレッドへ渡す
    const QString library = m_config.speakerLibrary; // DLL の場所
    const QString model = m_config.speakerModel; // モデルの場所
    const quint64 generation = m_speakerGeneration; // 頼んだときの世代
    const double durationSeconds = double(samples.size()) / sampleRate; // 発話の長さ（秒）
    QPointer<InterpretationSession> session(this); // 結果を返す先（消えていたら返さない）
    QMetaObject::invokeMethod(
        m_speakerWorker,
        [embedder, library, model, generation, durationSeconds, session, id, samples]() {
            QString error; // 失敗の説明
            if (!embedder->isLoaded() && !embedder->load(library, model))
                error = embedder->lastError();
            QVector<float> embedding; // 求めた埋め込み（失敗なら空）
            if (error.isEmpty()) {
                embedding = embedder->compute(samples, sampleRate);
                if (embedding.isEmpty())
                    qInfo("Interpreter: speaker embedding of utterance %llu failed: %s", id, qPrintable(embedder->lastError()));
            }
            // session はこのスレッドから触らず、主スレッド（qApp）へ戻してから確かめる
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [session, generation, id, embedding, durationSeconds, error]() {
                    if (session)
                        session->handleEmbedding(generation, id, embedding, durationSeconds, error);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

// 埋め込みを話者に振り分け、待っている文字起こしがあれば表に出す
void InterpretationSession::handleEmbedding(quint64 generation, quint64 id, const QVector<float> &embedding, double durationSeconds, const QString &error)
{
    if (generation != m_speakerGeneration || !m_running)
        return;
    int speakerIndex = -1; // 振り分けた話者
    if (!error.isEmpty()) {
        // 部品を読み込めなかった。以後は聞き分けずに続ける
        if (m_identifySpeakers)
            emit errorOccurred(tr("Speaker identification is disabled: %1").arg(error));
        m_identifySpeakers = false;
        speakerIndex = speakerUnavailable;
    } else if (!embedding.isEmpty()) {
        speakerIndex = m_clusterer.assign(embedding, durationSeconds);
    }
    if (m_droppedSpeakerIds.remove(id))
        return; // 表示しなかった発話（重心の更新だけ使う）
    m_speakerResults.insert(id, speakerIndex);
    if (m_waiting.active && m_waiting.utterance.id == id) {
        const WaitingTranscript waiting = m_waiting; // 待っていた文字起こし
        m_waiting = WaitingTranscript();
        emitEntry(waiting.utterance, waiting.text, waiting.language);
        transcribeNext();
        reportQueues();
    }
}

// 翻訳を 1 つずつ順番に出す
void InterpretationSession::translateNext()
{
    if (!m_running || !m_translator || m_translating || m_translations.isEmpty())
        return;
    m_currentJob = m_translations.dequeue();
    m_translating = true;
    reportQueues();
    m_translator->translate(m_currentJob.text, m_currentJob.destination, m_currentJob.source);
}

// 翻訳の状態の変化を受ける
void InterpretationSession::handleTranslationState(ATranslationProvider::State state)
{
    if (!m_translating || !m_translator || state != ATranslationProvider::State::Processed)
        return;
    QString translation; // 訳文（失敗時はエラーの説明）
    const bool succeeded = m_translator->error == ATranslationProvider::TranslationError::NoError; // 翻訳できたか
    if (succeeded)
        translation = m_translator->result.translation.trimmed();
    else
        translation = tr("(translation failed: %1)").arg(m_translator->getErrorString());
    const TranslationJob job = m_currentJob; // 終わった依頼
    m_translating = false;
    m_translator->finish();
    qInfo("Interpreter latency: utterance %llu translated after %lld ms", job.id, m_clock.elapsed() - job.endedAt);
    emit translationReady(job.id, translation);
    if (succeeded)
        speak(job, translation);
    translateNext();
    reportQueues();
}

// 訳文を読み上げる（自分の声 → 相手の言語へ訳したもの、相手の声 → 自分の言語へ訳したもの）
void InterpretationSession::speak(const TranslationJob &job, const QString &translation)
{
    if (translation.isEmpty())
        return;
    if (job.speaker == Speaker::Self && m_selfSpeech && job.destination == m_config.languageB)
        m_selfSpeech->enqueue(translation, job.destination.toQLocale());
    else if (job.speaker == Speaker::Remote && m_remoteSpeech && job.destination == m_config.languageA)
        m_remoteSpeech->enqueue(translation, job.destination.toQLocale());
}

// 待ち件数を知らせる
void InterpretationSession::reportQueues()
{
    const int speech = (m_selfSpeech ? m_selfSpeech->pending() : 0) + (m_remoteSpeech ? m_remoteSpeech->pending() : 0); // 読み上げ待ち
    const int recognition = int(m_utterances.size() + m_inFlight.size()) + (m_waiting.active ? 1 : 0); // 文字起こし待ち
    emit queueChanged(recognition, int(m_translations.size()) + (m_translating ? 1 : 0), speech);
}
