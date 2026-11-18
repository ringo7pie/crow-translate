/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVector>

class QNetworkAccessManager;
class QProcess;

// whisper.cpp の whisper-server を loopback に常駐させ、発話の音声を文字起こしするクラス
class WhisperServer : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(WhisperServer)
public:
    // 文字起こし 1 件の結果
    struct Transcript {
        quint64 id = 0; // 要求の番号（呼び出し側が付ける）
        QString text; // 文字起こしの本文
        QString language; // 判定した言語の ISO 639-1 コード（分からなければ空）
    };

    explicit WhisperServer(QObject *parent = nullptr);
    ~WhisperServer() override;

    // whisper-server を起動する。準備ができたら ready()、失敗したら failed() を出す
    // 引数: executable = whisper-server のパス、model = ggml モデルのパス
    void start(const QString &executable, const QString &model);
    // whisper-server を止める
    void stop();
    // 文字起こしを受け付けられる状態かを返す
    bool isReady() const;

    // 16 kHz mono float の音声を文字起こしに出す。結果は transcribed() / transcriptionFailed() で返る
    // 引数: id = 要求の番号、samples = 音声、languageHint = 言語の指定（空なら自動判定）、
    //       prompt = 文脈と用語の手がかり（空なら送らない）
    void transcribe(quint64 id, const QVector<float> &samples, const QString &languageHint = QString(), const QString &prompt = QString());

    // 16 kHz mono float の音声を 16 bit PCM の WAV にする（試験からも使う）
    static QByteArray toWav(const QVector<float> &samples, int sampleRate = 16000);
    // whisper の言語表記（"ja" や "japanese"）を ISO 639-1 のコードにする。分からなければ空
    static QString normalizeLanguage(const QString &value);
    // 文字の種類から言語を推定する（日本語の仮名・漢字があれば "ja"、ラテン文字だけなら "en"）
    static QString guessLanguage(const QString &text);

signals:
    void ready();
    void failed(const QString &error);
    void transcribed(const WhisperServer::Transcript &transcript);
    void transcriptionFailed(quint64 id, const QString &error);

private:
    void pollHealth();
    void fail(const QString &error);
    void closeJob();

    QNetworkAccessManager *m_network; // HTTP の送受信
    QPointer<QProcess> m_process; // 常駐させた whisper-server
    QTimer m_healthTimer; // 起動完了を調べる間隔タイマー
    int m_port = 0; // whisper-server が待ち受ける loopback のポート
    int m_healthTries = 0; // 起動完了を調べた回数
    bool m_ready = false; // 文字起こしを受け付けられるか
    QString m_stderrTail; // 失敗時に見せる whisper-server の出力の末尾
#ifdef Q_OS_WIN
    void *m_job = nullptr; // アプリ終了時に whisper-server を必ず止めるジョブオブジェクト
#endif
};

Q_DECLARE_METATYPE(WhisperServer::Transcript)
