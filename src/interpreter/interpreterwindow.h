/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "interpretationsession.h"
#include "interpretertranslationsettings.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class TranscriptView;

// 同時通訳の窓（モードレス）。上に設定と開始/停止、下に 2 列の字幕
class InterpreterWindow : public QWidget
{
    Q_OBJECT
    Q_DISABLE_COPY(InterpreterWindow)
public:
    explicit InterpreterWindow(QWidget *parent = nullptr);
    ~InterpreterWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    QGroupBox *buildTranslationGroup();
    void fillDevices();
    void fillLocalAiModels();
    void updateTranslationControls();
    void loadTranslationSettings();
    InterpreterTranslationSettings currentTranslationSettings() const;
    void loadSettings();
    void saveSettings() const;
    void toggleRunning();
    void updateRunningState(bool running);
    void updateModeControls();
    void updateColumnTitles();
    void saveTranscript();
    QGroupBox *buildRecognitionGroup();
    void fillWhisperModels(const QString &selectedPath);
    void updateRecognitionControls();
    InterpretationSession::Config makeConfig() const;

    InterpretationSession *m_session; // 通訳の制御
    TranscriptView *m_transcript = nullptr; // 字幕
    QPushButton *m_startButton = nullptr; // 開始/停止
    QLabel *m_statusLabel = nullptr; // 状態
    QLabel *m_queueLabel = nullptr; // 待ち件数
    QLabel *m_backendLabel = nullptr; // 翻訳方式の表示と注意
    QGroupBox *m_optionsGroup = nullptr; // 設定のまとまり（動作中は無効）
    QComboBox *m_languageA = nullptr; // 自分の言語
    QComboBox *m_languageB = nullptr; // 相手の言語
    QCheckBox *m_captureMicrophone = nullptr; // マイクを取り込むか
    QComboBox *m_microphone = nullptr; // マイク
    QCheckBox *m_captureRemote = nullptr; // 相手の音を取り込むか
    QComboBox *m_remoteOutput = nullptr; // 相手の音を取り込む出力デバイス
    QComboBox *m_selfMode = nullptr; // 自分の声の扱い
    QComboBox *m_virtualOutput = nullptr; // 訳の読み上げを送る仮想デバイス
    QCheckBox *m_mixOriginal = nullptr; // 元の声も混ぜるか
    QCheckBox *m_speakRemote = nullptr; // 相手の訳を読み上げるか
    QComboBox *m_speakerOutput = nullptr; // 相手の訳の読み上げ先
    QDoubleSpinBox *m_speechRate = nullptr; // 読み上げの速さ
    QDoubleSpinBox *m_threshold = nullptr; // 発話とみなす音量
    QSpinBox *m_silenceMs = nullptr; // 区切りの無音の長さ

    // 音声認識の設定
    QComboBox *m_whisperModel = nullptr; // 認識モデル（data はモデルのパス）
    QLineEdit *m_glossary = nullptr; // 用語（固有名詞など）
    QCheckBox *m_speakerIdentification = nullptr; // 相手の声を話者ごとに聞き分けるか
    QDoubleSpinBox *m_speakerThreshold = nullptr; // 同じ人とみなす類似度
    QLabel *m_speakerNote = nullptr; // 話者の聞き分けの部品が無いときの説明

    // 通訳の翻訳設定（メインの翻訳設定とは別）
    QFormLayout *m_translationForm = nullptr; // 通訳の翻訳設定の並び（方式ごとに行を出し分ける）
    QComboBox *m_translationBackend = nullptr; // 翻訳の方式
    QComboBox *m_mozhiEngine = nullptr; // Mozhi のエンジン
    QComboBox *m_mozhiInstance = nullptr; // Mozhi のインスタンス（編集可）
    QComboBox *m_localModel = nullptr; // ローカル翻訳モデル
    QComboBox *m_localAiProvider = nullptr; // LocalAI の接続先
    QComboBox *m_localAiModel = nullptr; // LocalAI のモデル（編集可）
    QComboBox *m_codexModel = nullptr; // Codex のモデル（編集可）
    QComboBox *m_codexEffort = nullptr; // Codex の推論の強さ
    QComboBox *m_claudeCodeModel = nullptr; // Claude Code のモデル（編集可）
    QComboBox *m_claudeCodeEffort = nullptr; // Claude Code の推論の強さ
};
