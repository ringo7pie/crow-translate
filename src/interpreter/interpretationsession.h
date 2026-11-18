/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "language.h"
#include "speakerclusterer.h"
#include "translator/atranslationprovider.h"
#include "voicesegmenter.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>

#include <memory>

class AudioCapture;
class AudioPassthrough;
class QThread;
class SpeakerEmbedder;
class SpeechOutput;
class WhisperServer;

// 同時通訳の全体の制御。話者ごとの音声 → 発話区切り → whisper の文字起こし → 翻訳 → 読み上げ をつなぐ
class InterpretationSession : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(InterpretationSession)
public:
    // 話者
    enum class Speaker {
        Self, // 自分（マイク）
        Remote, // 相手（PC の再生音）
    };
    Q_ENUM(Speaker)

    // 自分の声の扱い
    enum class SelfVoiceMode {
        SubtitlesOnly, // 字幕のみ
        SendTranslatedSpeech, // 訳の読み上げを仮想デバイスへ送る
    };
    Q_ENUM(SelfVoiceMode)

    // 開始時の設定
    struct Config {
        Language languageA; // 自分の言語（左の列）
        Language languageB; // 相手の言語（右の列）
        bool captureMicrophone = true; // マイクを取り込むか
        QByteArray microphoneId; // マイクの QAudioDevice::id()（空なら既定）
        bool captureRemote = true; // 相手の音（再生音）を取り込むか
        QByteArray remoteOutputId; // 再生音を取り込む出力デバイスの id（空なら既定）
        SelfVoiceMode selfVoiceMode = SelfVoiceMode::SubtitlesOnly; // 自分の声の扱い
        QByteArray virtualOutputId; // 訳の読み上げを送る仮想デバイスの id
        bool mixOriginalVoice = false; // 仮想デバイスへ元の声も混ぜるか
        bool speakRemoteTranslation = false; // 相手の訳を読み上げるか
        QByteArray speakerOutputId; // 相手の訳を読み上げる出力デバイスの id（空なら既定）
        double speechRate = 0.0; // 読み上げの速さ（-1.0〜1.0）
        VoiceSegmenter::Settings vad; // 発話区切りの調整値
        QString whisperExecutable; // whisper-server の実行ファイル
        QString whisperModel; // whisper のモデル
        QString glossary; // 用語（固有名詞など）。文字起こしの手がかりとして whisper に渡す
        bool speakerIdentification = false; // 相手の声を話者ごとに聞き分けるか
        QString speakerLibrary; // sherpa-onnx の C API の DLL
        QString speakerModel; // 話者埋め込みモデル（.onnx）
        float speakerThreshold = 0.5f; // 同じ人とみなすコサイン類似度
        ATranslationProvider::ProviderBackend backend = ATranslationProvider::ProviderBackend::Mozhi; // 翻訳に使う方式
        QVariantMap translationOptions; // 翻訳方式に渡す設定（通訳専用の翻訳設定から作る）
    };

    explicit InterpretationSession(QObject *parent = nullptr);
    ~InterpretationSession() override;

    // 通訳を始める。失敗は errorOccurred() で知らせる
    void start(const Config &config);
    // 通訳を止める（途中の発話は捨てる）
    void stop();
    bool isRunning() const;

    // 話者と判定言語から、原文を置く列（0 = 言語 A、1 = 言語 B）を決める
    static int columnFor(Speaker speaker, const QString &detectedLanguage, const Language &languageA, const Language &languageB);
    // whisper の出力から、無音時の幻聴（[BLANK_AUDIO] や (音楽) など）を取り除く。何も残らなければ空を返す
    static QString cleanTranscript(const QString &text);
    // 判定言語が言語 A・B のどちらでもなく、言語を指定して認識し直すべきかを返す（判定言語が空なら false）
    static bool needsLanguageRetry(const QString &detectedLanguage, const Language &languageA, const Language &languageB);
    // whisper に渡す手がかり（用語 + 直前の発言の末尾）を作る
    static QString buildPrompt(const QString &glossary, const QString &previousText);

signals:
    // 原文が確定した。entryId で後の訳と対応させる。column は原文の列（0 / 1）。
    // speakerLabel は表示する話者名（「自分」「Aさん」など。空なら「相手」）、speakerIndex は相手の話者の番号（-1 なら無し）
    void entryAdded(quint64 entryId, InterpretationSession::Speaker speaker, int column, const QString &text, const QDateTime &time,
                    const QString &speakerLabel, int speakerIndex);
    // 訳文が確定した（entryAdded と同じ entryId）
    void translationReady(quint64 entryId, const QString &text);
    void runningChanged(bool running);
    void statusChanged(const QString &status);
    void errorOccurred(const QString &error);
    // 待ち件数（文字起こし待ち・翻訳待ち・読み上げ待ち）
    void queueChanged(int recognition, int translation, int speech);

private:
    struct Utterance {
        quint64 id = 0; // 発話の番号
        Speaker speaker = Speaker::Self; // 話者
        QVector<float> samples; // 音声（音量をそろえたもの。言語を指定した認識し直しにも使う）
        qint64 endedAt = 0; // 発話が終わった時刻（m_clock のミリ秒）
        bool identifySpeaker = false; // 話者の聞き分けの結果を待つか
        bool retried = false; // 言語を指定して認識し直したか
    };
    // 話者の聞き分けの結果を待っている文字起こし
    struct WaitingTranscript {
        bool active = false; // 待っているか
        Utterance utterance; // 発話
        QString text; // 幻聴を除いた原文
        QString language; // 判定言語
    };
    struct TranslationJob {
        quint64 id = 0; // 発話の番号
        Speaker speaker = Speaker::Self; // 話者
        QString text; // 原文
        Language source; // 原文の言語
        Language destination; // 訳す先の言語
        qint64 endedAt = 0; // 発話が終わった時刻
    };

    void addUtterance(Speaker speaker, const QVector<float> &samples);
    void transcribeNext();
    void handleTranscript(quint64 id, const QString &text, const QString &language);
    void emitEntry(const Utterance &utterance, const QString &text, const QString &language);
    void startSpeakerIdentification();
    void stopSpeakerIdentification();
    void requestEmbedding(quint64 id, const QVector<float> &samples);
    void handleEmbedding(quint64 generation, quint64 id, const QVector<float> &embedding, double durationSeconds, const QString &error);
    void translateNext();
    void handleTranslationState(ATranslationProvider::State state);
    void speak(const TranslationJob &job, const QString &translation);
    void reportQueues();
    void fail(const QString &error);

    Config m_config; // 開始時の設定
    bool m_running = false; // 動作中か
    QElapsedTimer m_clock; // 遅延の計測用
    quint64 m_nextId = 1; // 次の発話の番号

    WhisperServer *m_whisper = nullptr; // 文字起こし
    AudioCapture *m_microphone = nullptr; // マイクの取り込み
    AudioCapture *m_loopback = nullptr; // 再生音の取り込み
    VoiceSegmenter *m_selfSegmenter = nullptr; // 自分の発話区切り
    VoiceSegmenter *m_remoteSegmenter = nullptr; // 相手の発話区切り
    AudioPassthrough *m_passthrough = nullptr; // 元の声を仮想デバイスへ流す
    SpeechOutput *m_selfSpeech = nullptr; // 自分の訳の読み上げ（仮想デバイス）
    SpeechOutput *m_remoteSpeech = nullptr; // 相手の訳の読み上げ

    QQueue<Utterance> m_utterances; // 文字起こし待ちの発話
    QHash<quint64, Utterance> m_inFlight; // 文字起こし中の発話（番号 → 話者・時刻）
    QQueue<TranslationJob> m_translations; // 翻訳待ち
    QPointer<ATranslationProvider> m_translator; // 翻訳
    TranslationJob m_currentJob; // 翻訳中の文
    bool m_translating = false; // 翻訳中か

    QHash<int, QString> m_previousText; // 話者（Speaker の値）ごとの直前の発言（文脈の手がかり）
    WaitingTranscript m_waiting; // 話者の聞き分けの結果を待っている文字起こし

    // 話者の聞き分け（相手の声だけ）
    bool m_identifySpeakers = false; // 聞き分けを行うか（読み込みに失敗したら false に戻す）
    QThread *m_speakerThread = nullptr; // 埋め込みを計算するスレッド
    QObject *m_speakerWorker = nullptr; // m_speakerThread で動く、処理を受ける器
    std::shared_ptr<SpeakerEmbedder> m_embedder; // 埋め込みの計算（m_speakerThread の中だけで使う）
    SpeakerClusterer m_clusterer; // 埋め込みを話者に振り分ける
    QHash<quint64, int> m_speakerResults; // 発話の番号 → 話者の番号（-1 = 不明、-2 = 聞き分けできない）
    QSet<quint64> m_droppedSpeakerIds; // 表示しなかった発話（後から届く結果を捨てる）
    quint64 m_speakerGeneration = 0; // 開始ごとに増やし、止めた後に届く古い結果を捨てる
};

Q_DECLARE_METATYPE(InterpretationSession::Speaker)
