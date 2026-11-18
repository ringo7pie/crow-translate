/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "interpreterwindow.h"

#include "instancepinger.h"
#include "onlinetranslator.h"
#include "optionalcomponents.h"
#include "settings/appsettings.h"
#include "transcriptview.h"
#include "translator/localmodelstranslationprovider.h"

#include <QAudioDevice>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMediaDevices>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
// コンボの中から data が一致する項目を選ぶ
void selectData(QComboBox *combo, const QVariant &data)
{
    const int index = combo->findData(data); // 一致した位置
    if (index >= 0)
        combo->setCurrentIndex(index);
}

// 言語のコンボを作る
void fillLanguages(QComboBox *combo)
{
    const auto languages = Language::allLanguages(); // 使える言語
    for (const Language &language : languages) {
        if (language.isValid() && language != Language::autoLanguage())
            combo->addItem(language.displayName(), language.toCode());
    }
}

// 部品の置き場を決める: 部品の既定の場所 → ローカル AI の保存先（管理画面で場所を変えた場合）
// 引数: defaultRoot = 既定の置き場、subFolder = 保存先の中のフォルダ、probe = 置き場の中にあるはずのファイル（相対）
// 戻り値: 使う置き場
QString componentRoot(const QString &defaultRoot, const QString &subFolder, const QString &probe)
{
    if (QFile::exists(defaultRoot + QLatin1Char('/') + probe))
        return defaultRoot;
    const QString candidate = AppSettings().localModelsRoot() + QLatin1Char('/') + subFolder; // 保存先の中の置き場
    if (QFile::exists(candidate + QLatin1Char('/') + probe))
        return candidate;
    return defaultRoot;
}

// whisper の置き場（bin と models を持つフォルダ）を返す
QString whisperFolder()
{
    const QString executable = QFileInfo(OptionalComponents::whisperServer()).fileName(); // 実行ファイル名
    return componentRoot(OptionalComponents::whisperRoot(), QStringLiteral("speech/whisper"), QStringLiteral("bin/") + executable);
}

// 話者の聞き分けの部品の置き場を返す
QString speakerFolder()
{
    const QString root = OptionalComponents::speakerRoot(); // 既定の置き場
    const QString library = QDir(root).relativeFilePath(OptionalComponents::speakerLibrary()); // DLL の相対パス
    return componentRoot(root, QStringLiteral("speech/speaker"), library);
}

// 話者の聞き分けの DLL とモデルのパスを返す（置き場を変えた場合も含む）
QPair<QString, QString> speakerFiles()
{
    const QString root = OptionalComponents::speakerRoot(); // 既定の置き場
    const QString folder = speakerFolder(); // 実際の置き場
    const QString library = folder + QLatin1Char('/') + QDir(root).relativeFilePath(OptionalComponents::speakerLibrary()); // DLL
    const QString model = folder + QLatin1Char('/') + QDir(root).relativeFilePath(OptionalComponents::speakerModel()); // モデル
    return {library, model};
}
}

InterpreterWindow::InterpreterWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_session(new InterpretationSession(this))
{
    setWindowTitle(tr("Simultaneous interpretation"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    buildUi();
    fillDevices();
    loadSettings();
    updateModeControls();
    updateColumnTitles();
    watchOptionChanges();

    connect(m_session, &InterpretationSession::entryAdded, this,
            [this](quint64 entryId,
                   InterpretationSession::Speaker speaker,
                   int column,
                   const QString &text,
                   const QDateTime &time,
                   const QString &speakerLabel,
                   int speakerIndex) {
                m_transcript->addEntry(entryId, speaker == InterpretationSession::Speaker::Self, column, text, time, speakerLabel, speakerIndex);
            });
    connect(m_session, &InterpretationSession::translationReady, m_transcript, &TranscriptView::setTranslation);
    connect(m_session, &InterpretationSession::runningChanged, this, &InterpreterWindow::updateRunningState);
    connect(m_session, &InterpretationSession::statusChanged, m_statusLabel, &QLabel::setText);
    connect(m_session, &InterpretationSession::errorOccurred, this, [this](const QString &error) {
        m_statusLabel->setText(error);
        qWarning("Interpreter: %s", qPrintable(error));
        // 止まった失敗（開始できなかった・途中で止まった）は状態欄だけだと見落とすので、知らせる窓も出す
        // 続けられる警告は状態欄だけ。クリック処理の中で入れ子のイベントループを回さないよう、閉じるのを待たない
        if (!m_session->isRunning()) {
            auto *box = new QMessageBox(QMessageBox::Warning, windowTitle(), error, QMessageBox::Ok, this); // 失敗を知らせる窓
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->open();
        }
    });
    connect(m_session, &InterpretationSession::queueChanged, this, [this](int recognition, int translation, int speech) {
        m_queueLabel->setText(tr("Waiting: recognition %1, translation %2, speech %3").arg(recognition).arg(translation).arg(speech));
    });
}

InterpreterWindow::~InterpreterWindow()
{
    m_session->stop();
}

// 画面を組み立てる
void InterpreterWindow::buildUi()
{
    auto *layout = new QVBoxLayout(this); // 全体の並び

    // 開始/停止と状態
    auto *topRow = new QHBoxLayout; // 上の段
    m_startButton = new QPushButton(tr("Start"), this);
    m_startButton->setIcon(QIcon::fromTheme(QStringLiteral("media-record")));
    connect(m_startButton, &QPushButton::clicked, this, &InterpreterWindow::toggleRunning);
    m_statusLabel = new QLabel(tr("Stopped"), this);
    m_statusLabel->setWordWrap(true);
    m_queueLabel = new QLabel(this);
    auto *saveButton = new QPushButton(tr("Save…"), this); // 字幕の保存
    connect(saveButton, &QPushButton::clicked, this, &InterpreterWindow::saveTranscript);
    auto *clearButton = new QPushButton(tr("Clear"), this); // 字幕の消去
    connect(clearButton, &QPushButton::clicked, this, [this]() { m_transcript->clearEntries(); });
    topRow->addWidget(m_startButton);
    topRow->addWidget(m_statusLabel, 1);
    topRow->addWidget(m_queueLabel);
    topRow->addWidget(saveButton);
    topRow->addWidget(clearButton);
    layout->addLayout(topRow);

    // 設定（見出しのボタンで折りたたむ。聞き取り中も変えられる）
    m_optionsToggle = new QToolButton(this);
    m_optionsToggle->setText(tr("Settings"));
    m_optionsToggle->setToolTip(tr("Show or hide the settings. Changes are applied while listening."));
    m_optionsToggle->setCheckable(true);
    m_optionsToggle->setAutoRaise(true);
    m_optionsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(m_optionsToggle, &QToolButton::toggled, this, &InterpreterWindow::setOptionsExpanded);
    layout->addWidget(m_optionsToggle);
    m_optionsPanel = new QWidget(this);
    auto *optionsLayout = new QHBoxLayout(m_optionsPanel); // 設定の左右
    optionsLayout->setContentsMargins(0, 0, 0, 0);
    auto *leftForm = new QFormLayout; // 言語と取り込み
    auto *rightForm = new QFormLayout; // 読み上げと詳細

    m_languageA = new QComboBox(this);
    m_languageB = new QComboBox(this);
    fillLanguages(m_languageA);
    fillLanguages(m_languageB);
    connect(m_languageA, &QComboBox::currentIndexChanged, this, &InterpreterWindow::updateColumnTitles);
    connect(m_languageB, &QComboBox::currentIndexChanged, this, &InterpreterWindow::updateColumnTitles);
    leftForm->addRow(tr("My language:"), m_languageA);
    leftForm->addRow(tr("Remote language:"), m_languageB);

    m_captureMicrophone = new QCheckBox(tr("Microphone:"), this);
    m_microphone = new QComboBox(this);
    leftForm->addRow(m_captureMicrophone, m_microphone);
    m_captureRemote = new QCheckBox(tr("Meeting audio from:"), this);
    m_captureRemote->setToolTip(tr("The output device the meeting plays on (your headphones). Its sound is captured as the remote speaker."));
    m_remoteOutput = new QComboBox(this);
    leftForm->addRow(m_captureRemote, m_remoteOutput);

    m_selfMode = new QComboBox(this);
    m_selfMode->addItem(tr("Subtitles only"), int(InterpretationSession::SelfVoiceMode::SubtitlesOnly));
    m_selfMode->addItem(tr("Send translated speech to a virtual device"), int(InterpretationSession::SelfVoiceMode::SendTranslatedSpeech));
    m_selfMode->setToolTip(tr("Install a virtual audio cable (e.g. VB-CABLE), choose \"CABLE Input\" here and \"CABLE Output\" as the microphone in Teams."));
    connect(m_selfMode, &QComboBox::currentIndexChanged, this, &InterpreterWindow::updateModeControls);
    rightForm->addRow(tr("My voice:"), m_selfMode);
    m_virtualOutput = new QComboBox(this);
    rightForm->addRow(tr("Virtual device:"), m_virtualOutput);
    m_mixOriginal = new QCheckBox(tr("Also send my original voice"), this);
    rightForm->addRow(QString(), m_mixOriginal);
    m_speakRemote = new QCheckBox(tr("Read remote translation aloud on:"), this);
    connect(m_speakRemote, &QCheckBox::toggled, this, &InterpreterWindow::updateModeControls);
    m_speakerOutput = new QComboBox(this);
    rightForm->addRow(m_speakRemote, m_speakerOutput);
    m_speechRate = new QDoubleSpinBox(this);
    m_speechRate->setRange(-1.0, 1.0);
    m_speechRate->setSingleStep(0.1);
    rightForm->addRow(tr("Speech rate:"), m_speechRate);
    auto *vadRow = new QHBoxLayout; // 発話区切りの調整
    m_threshold = new QDoubleSpinBox(this);
    m_threshold->setRange(0.001, 0.2);
    m_threshold->setDecimals(3);
    m_threshold->setSingleStep(0.002);
    m_threshold->setToolTip(tr("Minimum loudness (RMS) treated as speech. Raise it if noise is transcribed."));
    m_silenceMs = new QSpinBox(this);
    m_silenceMs->setRange(200, 3000);
    m_silenceMs->setSingleStep(100);
    m_silenceMs->setSuffix(tr(" ms"));
    m_silenceMs->setToolTip(tr("Silence that ends a sentence. Shorter is faster but splits sentences more often."));
    vadRow->addWidget(m_threshold);
    vadRow->addWidget(m_silenceMs);
    rightForm->addRow(tr("Voice level / pause:"), vadRow);

    connect(m_captureRemote, &QCheckBox::toggled, this, &InterpreterWindow::updateRecognitionControls);

    optionsLayout->addLayout(leftForm, 1);
    optionsLayout->addLayout(rightForm, 1);
    optionsLayout->addWidget(buildRecognitionGroup(), 1);
    optionsLayout->addWidget(buildTranslationGroup(), 1);
    layout->addWidget(m_optionsPanel);

    // 字幕
    m_transcript = new TranscriptView(this);
    connect(m_transcript, &TranscriptView::fontPointSizeChanged, this, [](qreal pointSize) {
        AppSettings().setInterpretationValue(QStringLiteral("FontSize"), pointSize);
    });
    layout->addWidget(m_transcript, 1);
    resize(1280, 760);
}

// 音声認識の設定（認識モデル・用語・話者の聞き分け）のまとまりを作る
// 戻り値: 設定欄に置くグループ
QGroupBox *InterpreterWindow::buildRecognitionGroup()
{
    auto *group = new QGroupBox(tr("Speech recognition"), this); // 音声認識の設定のまとまり
    auto *form = new QFormLayout(group); // 項目の並び

    m_whisperModel = new QComboBox(group);
    m_whisperModel->setToolTip(tr("large-v3 is more accurate but about twice as slow. A GPU is recommended."));
    form->addRow(tr("Recognition model:"), m_whisperModel);

    m_glossary = new QLineEdit(group);
    m_glossary->setPlaceholderText(tr("e.g. Crow Translate, Kubernetes, Tanaka"));
    m_glossary->setToolTip(tr("Names and terms that appear in the meeting, separated by commas. They help the recognizer spell them correctly."));
    form->addRow(tr("Terms:"), m_glossary);

    m_speakerIdentification = new QCheckBox(tr("Identify remote speakers (A, B, …)"), group);
    m_speakerIdentification->setToolTip(tr("Tells the voices in the meeting audio apart and shows who is speaking. Your microphone is always shown as \"Me\"."));
    connect(m_speakerIdentification, &QCheckBox::toggled, this, &InterpreterWindow::updateRecognitionControls);
    form->addRow(m_speakerIdentification);

    m_speakerThreshold = new QDoubleSpinBox(group);
    m_speakerThreshold->setRange(0.20, 0.90);
    m_speakerThreshold->setDecimals(2);
    m_speakerThreshold->setSingleStep(0.05);
    m_speakerThreshold->setToolTip(tr("Similarity needed to treat two utterances as the same person. "
                                      "Raise it if different people get the same letter; lower it if one person gets several letters."));
    form->addRow(tr("Same-speaker similarity:"), m_speakerThreshold);

    m_speakerNote = new QLabel(tr("Install \"Speaker identification\" from \"Manage local AI\" to use this."), group);
    m_speakerNote->setWordWrap(true);
    m_speakerNote->setEnabled(false);
    form->addRow(m_speakerNote);
    return group;
}

// 認識モデルのコンボを入れる（未導入のものは名前に印を付ける）
// 引数: selectedPath = 選ぶモデルのパス（空なら既定の large-v3-turbo）
void InterpreterWindow::fillWhisperModels(const QString &selectedPath)
{
    const QString models = whisperFolder() + QStringLiteral("/models/"); // モデルのフォルダ
    const QList<QPair<QString, QString>> choices{
        {tr("large-v3-turbo (fast)"), models + QFileInfo(OptionalComponents::whisperModel()).fileName()},
        {tr("large-v3 (accurate, slow)"), models + QFileInfo(OptionalComponents::whisperLargeModel()).fileName()},
    }; // 選べるモデル（表示名とパス）
    const QSignalBlocker blocker(m_whisperModel);
    m_whisperModel->clear();
    for (const auto &choice : choices) {
        QString name = choice.first; // 表示名
        if (!QFile::exists(choice.second))
            name += tr(" (not installed)");
        m_whisperModel->addItem(name, choice.second);
    }
    int index = 0; // 選ぶ位置
    if (!selectedPath.isEmpty()) {
        index = -1;
        for (int row = 0; row < m_whisperModel->count(); ++row) {
            // 保存先を変えた場合もファイル名で対応させる
            if (QFileInfo(m_whisperModel->itemData(row).toString()).fileName() == QFileInfo(selectedPath).fileName()) {
                index = row;
                break;
            }
        }
        if (index < 0) {
            // 手で指定したモデル
            m_whisperModel->addItem(QFileInfo(selectedPath).fileName(), selectedPath);
            index = m_whisperModel->count() - 1;
        }
    }
    m_whisperModel->setCurrentIndex(index);
}

// 話者の聞き分けの項目を、部品の有無と相手の音の取り込みに合わせて有効にする
void InterpreterWindow::updateRecognitionControls()
{
    const auto files = speakerFiles(); // DLL とモデル
    const bool installed = QFile::exists(files.first) && QFile::exists(files.second); // 部品が揃っているか
    m_speakerIdentification->setEnabled(installed && m_captureRemote->isChecked());
    m_speakerThreshold->setEnabled(m_speakerIdentification->isEnabled() && m_speakerIdentification->isChecked());
    m_speakerNote->setVisible(!installed);
}

// 通訳の翻訳設定（メインの翻訳設定とは別）のまとまりを作る
// 戻り値: 設定欄に置くグループ
QGroupBox *InterpreterWindow::buildTranslationGroup()
{
    using Backend = ATranslationProvider::ProviderBackend;
    auto *group = new QGroupBox(tr("Interpretation translation settings"), this); // 通訳の翻訳設定のまとまり
    group->setToolTip(tr("Used only for simultaneous interpretation. The translator of the main window is not changed."));
    m_translationForm = new QFormLayout(group);

    m_translationBackend = new QComboBox(group);
    m_translationBackend->addItem(tr("Local translation models"), QVariant::fromValue(Backend::LocalModels));
    m_translationBackend->addItem(QStringLiteral("Mozhi"), QVariant::fromValue(Backend::Mozhi));
    m_translationBackend->addItem(QStringLiteral("LocalAI"), QVariant::fromValue(Backend::LocalAI));
    m_translationBackend->addItem(QStringLiteral("Codex"), QVariant::fromValue(Backend::Codex));
    m_translationBackend->addItem(QStringLiteral("Claude Code"), QVariant::fromValue(Backend::ClaudeCode));
    connect(m_translationBackend, &QComboBox::currentIndexChanged, this, &InterpreterWindow::updateTranslationControls);
    m_translationForm->addRow(tr("Translator:"), m_translationBackend);

    // Mozhi
    m_mozhiEngine = new QComboBox(group);
    m_mozhiEngine->addItem(QStringLiteral("Google"), int(OnlineTranslator::Google));
    m_mozhiEngine->addItem(QStringLiteral("Yandex"), int(OnlineTranslator::Yandex));
    m_mozhiEngine->addItem(QStringLiteral("DeepL"), int(OnlineTranslator::Deepl));
    m_mozhiEngine->addItem(QStringLiteral("DuckDuckGo"), int(OnlineTranslator::Duckduckgo));
    m_mozhiEngine->addItem(QStringLiteral("LibreTranslate"), int(OnlineTranslator::LibreTranslate));
    m_mozhiEngine->addItem(QStringLiteral("MyMemory"), int(OnlineTranslator::Mymemory));
    m_mozhiEngine->addItem(QStringLiteral("Reverso"), int(OnlineTranslator::Reverso));
    m_translationForm->addRow(tr("Engine:"), m_mozhiEngine);
    m_mozhiInstance = new QComboBox(group);
    m_mozhiInstance->setEditable(true);
    m_mozhiInstance->setInsertPolicy(QComboBox::NoInsert);
    m_mozhiInstance->addItems(InstancePinger::instances());
    m_translationForm->addRow(tr("Instance:"), m_mozhiInstance);

    // ローカル翻訳モデル（未導入のものは名前に印を付ける）
    m_localModel = new QComboBox(group);
    const QString root = AppSettings().localModelsRoot(); // モデルの置き場
    const QStringList modelIds = LocalModelsTranslationProvider::modelIds(); // 選べるモデル
    for (const QString &id : modelIds) {
        QString name = LocalModelsTranslationProvider::modelName(id); // 表示名
        if (!LocalModelsTranslationProvider::isInstalled(root, id))
            name += tr(" (not installed)");
        m_localModel->addItem(name, id);
    }
    connect(m_localModel, &QComboBox::currentIndexChanged, this, &InterpreterWindow::updateTranslationControls);
    m_translationForm->addRow(tr("Model:"), m_localModel);

    // LocalAI
    m_localAiProvider = new QComboBox(group);
    const QStringList providerIds = AppSettings::localProviderIds(); // 接続先の一覧
    for (const QString &id : providerIds)
        m_localAiProvider->addItem(AppSettings::localProviderDisplayName(id), id);
    m_translationForm->addRow(tr("Provider:"), m_localAiProvider);
    m_localAiModel = new QComboBox(group);
    m_localAiModel->setEditable(true);
    m_localAiModel->setInsertPolicy(QComboBox::NoInsert);
    m_translationForm->addRow(tr("Model:"), m_localAiModel);
    connect(m_localAiProvider, &QComboBox::currentIndexChanged, this, [this]() {
        fillLocalAiModels();
        m_localAiModel->setCurrentText(AppSettings().localProviderModel(m_localAiProvider->currentData().toString()));
    });

    // Codex
    m_codexModel = new QComboBox(group);
    m_codexModel->setEditable(true);
    m_codexModel->setInsertPolicy(QComboBox::NoInsert);
    m_codexModel->addItems({AppSettings::defaultCodexModel(),
                            QStringLiteral("gpt-6.1-terra"),
                            QStringLiteral("gpt-6.1-sol"),
                            QStringLiteral("gpt-6-sol"),
                            QStringLiteral("gpt-6-astra")});
    m_translationForm->addRow(tr("Model:"), m_codexModel);
    m_codexEffort = new QComboBox(group);
    m_translationForm->addRow(tr("Reasoning effort:"), m_codexEffort);

    // Claude Code
    m_claudeCodeModel = new QComboBox(group);
    m_claudeCodeModel->setEditable(true);
    m_claudeCodeModel->setInsertPolicy(QComboBox::NoInsert);
    m_claudeCodeModel->addItems({AppSettings::defaultClaudeCodeModel(), QStringLiteral("sonnet"), QStringLiteral("opus")});
    m_translationForm->addRow(tr("Model:"), m_claudeCodeModel);
    m_claudeCodeEffort = new QComboBox(group);
    m_claudeCodeEffort->addItem(tr("CLI default"), QString());
    m_translationForm->addRow(tr("Reasoning effort:"), m_claudeCodeEffort);
    // 推論の強さの選択肢（Codex と Claude Code で共通）
    const QList<QPair<QString, QString>> efforts{{tr("Low"), QStringLiteral("low")},
                                                 {tr("Medium"), QStringLiteral("medium")},
                                                 {tr("High"), QStringLiteral("high")},
                                                 {tr("Extra high"), QStringLiteral("xhigh")},
                                                 {tr("Maximum"), QStringLiteral("max")}};
    for (const auto &effort : efforts) {
        m_codexEffort->addItem(effort.first, effort.second);
        m_claudeCodeEffort->addItem(effort.first, effort.second);
    }
    // Haiku 4.5 は推論の強さを受け付けない
    connect(m_claudeCodeModel, &QComboBox::currentTextChanged, this, [this](const QString &model) {
        m_claudeCodeEffort->setEnabled(!model.startsWith(QStringLiteral("claude-haiku-4-5")));
    });

    // 方式ごとの注意
    m_backendLabel = new QLabel(group);
    m_backendLabel->setWordWrap(true);
    m_translationForm->addRow(m_backendLabel);
    auto *noteLabel = new QLabel(tr("Prompts, timeouts, API keys and other details use the values in Settings → Translation."), group); // 細かい項目の説明
    noteLabel->setWordWrap(true);
    noteLabel->setEnabled(false);
    m_translationForm->addRow(noteLabel);
    return group;
}

// LocalAI の接続先が覚えているモデルの一覧を入れる（入力中の文字は残す）
void InterpreterWindow::fillLocalAiModels()
{
    const AppSettings settings; // 設定
    const QString provider = m_localAiProvider->currentData().toString(); // 選んだ接続先
    const QString text = m_localAiModel->currentText(); // 入力中のモデル名
    QStringList models = settings.localProviderModels(provider); // 接続先のモデルの一覧
    const QString mainModel = settings.localProviderModel(provider); // メインで選んでいるモデル
    if (!mainModel.isEmpty() && !models.contains(mainModel))
        models.prepend(mainModel);
    const QSignalBlocker blocker(m_localAiModel);
    m_localAiModel->clear();
    m_localAiModel->addItems(models);
    m_localAiModel->setCurrentText(text);
}

// 選んだ方式の項目だけを出し、注意を書き換える
void InterpreterWindow::updateTranslationControls()
{
    using Backend = ATranslationProvider::ProviderBackend;
    const auto backend = m_translationBackend->currentData().value<Backend>(); // 選んだ方式
    m_translationForm->setRowVisible(m_mozhiEngine, backend == Backend::Mozhi);
    m_translationForm->setRowVisible(m_mozhiInstance, backend == Backend::Mozhi);
    m_translationForm->setRowVisible(m_localModel, backend == Backend::LocalModels);
    m_translationForm->setRowVisible(m_localAiProvider, backend == Backend::LocalAI);
    m_translationForm->setRowVisible(m_localAiModel, backend == Backend::LocalAI);
    m_translationForm->setRowVisible(m_codexModel, backend == Backend::Codex);
    m_translationForm->setRowVisible(m_codexEffort, backend == Backend::Codex);
    m_translationForm->setRowVisible(m_claudeCodeModel, backend == Backend::ClaudeCode);
    m_translationForm->setRowVisible(m_claudeCodeEffort, backend == Backend::ClaudeCode);

    QString note; // 方式ごとの注意
    if (backend == Backend::Codex || backend == Backend::ClaudeCode) {
        note = tr("<b>This translator starts a program for every sentence and is slow for interpretation.</b> Local models, Mozhi or LocalAI are recommended.");
    } else if (backend == Backend::LocalModels) {
        const QString id = m_localModel->currentData().toString(); // 選んだモデル
        if (!id.isEmpty() && !LocalModelsTranslationProvider::isInstalled(AppSettings().localModelsRoot(), id))
            note = tr("<b>This model is not installed.</b> Install it from \"Manage local AI\".");
    }
    m_backendLabel->setText(note);
    m_backendLabel->setVisible(!note.isEmpty());
}

// 通訳の翻訳設定を画面に入れる
void InterpreterWindow::loadTranslationSettings()
{
    const AppSettings settings; // 設定
    const InterpreterTranslationSettings translation = InterpreterTranslationSettings::load(settings); // 通訳の翻訳設定
    selectData(m_mozhiEngine, translation.mozhiEngine);
    m_mozhiInstance->setCurrentText(translation.mozhiInstance);
    selectData(m_localModel, translation.localModel);
    {
        const QSignalBlocker blocker(m_localAiProvider); // 接続先を入れる間はモデルを上書きしない
        selectData(m_localAiProvider, translation.localAiProvider);
    }
    fillLocalAiModels();
    m_localAiModel->setCurrentText(translation.localAiModel);
    m_codexModel->setCurrentText(translation.codexModel);
    selectData(m_codexEffort, translation.codexEffort);
    m_claudeCodeModel->setCurrentText(translation.claudeCodeModel);
    selectData(m_claudeCodeEffort, translation.claudeCodeEffort);
    m_claudeCodeEffort->setEnabled(!translation.claudeCodeModel.startsWith(QStringLiteral("claude-haiku-4-5")));
    selectData(m_translationBackend, QVariant::fromValue(translation.backend));
    updateTranslationControls();
}

// 画面の値から通訳の翻訳設定を作る
InterpreterTranslationSettings InterpreterWindow::currentTranslationSettings() const
{
    InterpreterTranslationSettings translation; // 画面の値
    translation.backend = m_translationBackend->currentData().value<ATranslationProvider::ProviderBackend>();
    translation.mozhiEngine = m_mozhiEngine->currentData().toInt();
    translation.mozhiInstance = m_mozhiInstance->currentText().trimmed();
    translation.localModel = m_localModel->currentData().toString();
    translation.localAiProvider = m_localAiProvider->currentData().toString();
    translation.localAiModel = m_localAiModel->currentText().trimmed();
    translation.codexModel = m_codexModel->currentText().trimmed();
    translation.codexEffort = m_codexEffort->currentData().toString();
    translation.claudeCodeModel = m_claudeCodeModel->currentText().trimmed();
    translation.claudeCodeEffort = m_claudeCodeEffort->currentData().toString();
    return translation;
}

// 音声デバイスの一覧を入れる
void InterpreterWindow::fillDevices()
{
    m_microphone->addItem(tr("Default microphone"), QByteArray());
    const auto inputs = QMediaDevices::audioInputs(); // 入力デバイス
    for (const QAudioDevice &device : inputs)
        m_microphone->addItem(device.description(), device.id());
    const auto outputs = QMediaDevices::audioOutputs(); // 出力デバイス
    for (QComboBox *combo : {m_remoteOutput, m_virtualOutput, m_speakerOutput}) {
        combo->addItem(tr("Default output"), QByteArray());
        for (const QAudioDevice &device : outputs)
            combo->addItem(device.description(), device.id());
    }
    // 仮想ケーブルがあれば既定で選ぶ
    for (int index = 0; index < m_virtualOutput->count(); ++index) {
        if (m_virtualOutput->itemText(index).contains(QStringLiteral("CABLE Input"), Qt::CaseInsensitive)) {
            m_virtualOutput->setCurrentIndex(index);
            break;
        }
    }
}

// 保存した設定を読む
void InterpreterWindow::loadSettings()
{
    const AppSettings settings; // 設定
    selectData(m_languageA, settings.interpretationValue(QStringLiteral("LanguageA"), QStringLiteral("ja")));
    selectData(m_languageB, settings.interpretationValue(QStringLiteral("LanguageB"), QStringLiteral("en")));
    m_captureMicrophone->setChecked(settings.interpretationValue(QStringLiteral("CaptureMicrophone"), true).toBool());
    selectData(m_microphone, settings.interpretationValue(QStringLiteral("Microphone"), QByteArray()).toByteArray());
    m_captureRemote->setChecked(settings.interpretationValue(QStringLiteral("CaptureRemote"), true).toBool());
    selectData(m_remoteOutput, settings.interpretationValue(QStringLiteral("RemoteOutput"), QByteArray()).toByteArray());
    selectData(m_selfMode, settings.interpretationValue(QStringLiteral("SelfVoiceMode"), 0).toInt());
    const QByteArray virtualOutput = settings.interpretationValue(QStringLiteral("VirtualOutput"), QByteArray()).toByteArray(); // 保存した仮想デバイス
    if (!virtualOutput.isEmpty())
        selectData(m_virtualOutput, virtualOutput);
    m_mixOriginal->setChecked(settings.interpretationValue(QStringLiteral("MixOriginalVoice"), false).toBool());
    m_speakRemote->setChecked(settings.interpretationValue(QStringLiteral("SpeakRemote"), false).toBool());
    selectData(m_speakerOutput, settings.interpretationValue(QStringLiteral("SpeakerOutput"), QByteArray()).toByteArray());
    m_speechRate->setValue(settings.interpretationValue(QStringLiteral("SpeechRate"), 0.0).toDouble());
    const VoiceSegmenter::Settings defaults; // 発話区切りの既定値
    m_threshold->setValue(settings.interpretationValue(QStringLiteral("VadThreshold"), double(defaults.threshold)).toDouble());
    m_silenceMs->setValue(settings.interpretationValue(QStringLiteral("VadSilenceMs"), defaults.silenceMs).toInt());
    const qreal fontSize = settings.interpretationValue(QStringLiteral("FontSize"), 0.0).toDouble(); // 字幕の文字の大きさ
    m_transcript->setFontPointSize(fontSize > 0.0 ? fontSize : font().pointSizeF() + 3.0);
    restoreGeometry(settings.interpretationValue(QStringLiteral("Geometry")).toByteArray());
    fillWhisperModels(settings.interpretationValue(QStringLiteral("WhisperModel")).toString());
    m_glossary->setText(settings.interpretationValue(QStringLiteral("Glossary")).toString());
    m_speakerIdentification->setChecked(settings.interpretationValue(QStringLiteral("SpeakerIdentification"), true).toBool());
    m_speakerThreshold->setValue(settings.interpretationValue(QStringLiteral("SpeakerThreshold"), double(SpeakerClusterer::defaultThreshold)).toDouble());
    updateRecognitionControls();
    loadTranslationSettings();
    setOptionsExpanded(settings.interpretationValue(QStringLiteral("SettingsExpanded"), true).toBool());
}

// 設定を保存する
void InterpreterWindow::saveSettings() const
{
    AppSettings settings; // 設定
    settings.setInterpretationValue(QStringLiteral("LanguageA"), m_languageA->currentData());
    settings.setInterpretationValue(QStringLiteral("LanguageB"), m_languageB->currentData());
    settings.setInterpretationValue(QStringLiteral("CaptureMicrophone"), m_captureMicrophone->isChecked());
    settings.setInterpretationValue(QStringLiteral("Microphone"), m_microphone->currentData());
    settings.setInterpretationValue(QStringLiteral("CaptureRemote"), m_captureRemote->isChecked());
    settings.setInterpretationValue(QStringLiteral("RemoteOutput"), m_remoteOutput->currentData());
    settings.setInterpretationValue(QStringLiteral("SelfVoiceMode"), m_selfMode->currentData());
    settings.setInterpretationValue(QStringLiteral("VirtualOutput"), m_virtualOutput->currentData());
    settings.setInterpretationValue(QStringLiteral("MixOriginalVoice"), m_mixOriginal->isChecked());
    settings.setInterpretationValue(QStringLiteral("SpeakRemote"), m_speakRemote->isChecked());
    settings.setInterpretationValue(QStringLiteral("SpeakerOutput"), m_speakerOutput->currentData());
    settings.setInterpretationValue(QStringLiteral("SpeechRate"), m_speechRate->value());
    settings.setInterpretationValue(QStringLiteral("VadThreshold"), m_threshold->value());
    settings.setInterpretationValue(QStringLiteral("VadSilenceMs"), m_silenceMs->value());
    settings.setInterpretationValue(QStringLiteral("Geometry"), saveGeometry());
    settings.setInterpretationValue(QStringLiteral("WhisperModel"), m_whisperModel->currentData());
    settings.setInterpretationValue(QStringLiteral("Glossary"), m_glossary->text().trimmed());
    settings.setInterpretationValue(QStringLiteral("SpeakerIdentification"), m_speakerIdentification->isChecked());
    settings.setInterpretationValue(QStringLiteral("SpeakerThreshold"), m_speakerThreshold->value());
    settings.setInterpretationValue(QStringLiteral("SettingsExpanded"), m_optionsToggle->isChecked());
    currentTranslationSettings().save(settings);
}

// 画面の設定から開始時の設定を作る
InterpretationSession::Config InterpreterWindow::makeConfig() const
{
    const AppSettings settings; // 設定
    InterpretationSession::Config config; // 開始時の設定
    config.languageA = Language(m_languageA->currentData().toString());
    config.languageB = Language(m_languageB->currentData().toString());
    config.captureMicrophone = m_captureMicrophone->isChecked();
    config.microphoneId = m_microphone->currentData().toByteArray();
    config.captureRemote = m_captureRemote->isChecked();
    config.remoteOutputId = m_remoteOutput->currentData().toByteArray();
    config.selfVoiceMode = InterpretationSession::SelfVoiceMode(m_selfMode->currentData().toInt());
    config.virtualOutputId = m_virtualOutput->currentData().toByteArray();
    config.mixOriginalVoice = m_mixOriginal->isChecked();
    config.speakRemoteTranslation = m_speakRemote->isChecked();
    config.speakerOutputId = m_speakerOutput->currentData().toByteArray();
    config.speechRate = m_speechRate->value();
    config.vad.threshold = float(m_threshold->value());
    config.vad.silenceMs = m_silenceMs->value();
    // whisper の場所: 設定の指定 → 部品の既定の場所 → ローカル AI の保存先（管理画面で場所を変えた場合）
    const QString executable = whisperFolder() + QStringLiteral("/bin/") + QFileInfo(OptionalComponents::whisperServer()).fileName(); // whisper-server の場所
    config.whisperExecutable = settings.interpretationValue(QStringLiteral("WhisperExecutable"), executable).toString();
    config.whisperModel = m_whisperModel->currentData().toString();
    config.glossary = m_glossary->text().trimmed();
    // 話者の聞き分け（相手の音を取り込み、部品があるときだけ）
    const auto speaker = speakerFiles(); // DLL とモデル
    config.speakerLibrary = speaker.first;
    config.speakerModel = speaker.second;
    config.speakerIdentification = m_speakerIdentification->isEnabled() && m_speakerIdentification->isChecked();
    config.speakerThreshold = float(m_speakerThreshold->value());
    // 翻訳は通訳専用の設定を使う（メインの翻訳設定は使わない・変えない）
    const InterpreterTranslationSettings translation = currentTranslationSettings(); // 通訳の翻訳設定
    config.backend = translation.backend;
    config.translationOptions = translation.buildOptions(settings);
    return config;
}

// 開始/停止を切り替える
void InterpreterWindow::toggleRunning()
{
    if (m_session->isRunning()) {
        m_session->stop();
        return;
    }
    const InterpretationSession::Config config = makeConfig(); // 開始時の設定
    if (!QFile::exists(config.whisperExecutable) || !QFile::exists(config.whisperModel)) {
        QMessageBox::warning(this,
                             tr("Speech recognition is not installed"),
                             tr("whisper.cpp was not found. Install Whisper from \"Manage local AI\", "
                                "or place the files here:\n%1\n%2")
                                 .arg(QDir::toNativeSeparators(config.whisperExecutable), QDir::toNativeSeparators(config.whisperModel)));
        return;
    }
    if (config.languageA == config.languageB) {
        QMessageBox::warning(this, windowTitle(), tr("Choose two different languages."));
        return;
    }
    saveSettings();
    m_session->start(config);
}

// 開始/停止のボタンを動作の状態に合わせる（設定は動作中も変えられる）
void InterpreterWindow::updateRunningState(bool running)
{
    m_startButton->setText(running ? tr("Stop") : tr("Start"));
    m_startButton->setIcon(QIcon::fromTheme(running ? QStringLiteral("media-playback-stop") : QStringLiteral("media-record")));
    if (!running && m_applyTimer)
        m_applyTimer->stop(); // 止まった後に古い変更を反映しない
}

// 設定の折りたたみを切り替える
void InterpreterWindow::setOptionsExpanded(bool expanded)
{
    if (m_optionsToggle->isChecked() != expanded) {
        const QSignalBlocker blocker(m_optionsToggle); // ここから再び呼ばれないように
        m_optionsToggle->setChecked(expanded);
    }
    m_optionsToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    m_optionsPanel->setVisible(expanded);
}

// 設定欄の項目の変更を見張る
void InterpreterWindow::watchOptionChanges()
{
    m_applyTimer = new QTimer(this);
    m_applyTimer->setSingleShot(true);
    m_applyTimer->setInterval(600); // 入力中の連続した変更をまとめる待ち（ミリ秒）
    connect(m_applyTimer, &QTimer::timeout, this, &InterpreterWindow::applyRunningSettings);
    const auto schedule = [this]() {
        if (m_session->isRunning())
            m_applyTimer->start();
    }; // 動作中だけ反映を予約する
    for (QComboBox *combo : m_optionsPanel->findChildren<QComboBox *>()) {
        connect(combo, &QComboBox::currentIndexChanged, this, schedule);
        if (combo->isEditable())
            connect(combo, &QComboBox::currentTextChanged, this, schedule);
    }
    for (QCheckBox *check : m_optionsPanel->findChildren<QCheckBox *>())
        connect(check, &QCheckBox::toggled, this, schedule);
    for (QDoubleSpinBox *spin : m_optionsPanel->findChildren<QDoubleSpinBox *>())
        connect(spin, &QDoubleSpinBox::valueChanged, this, schedule);
    for (QSpinBox *spin : m_optionsPanel->findChildren<QSpinBox *>())
        connect(spin, &QSpinBox::valueChanged, this, schedule);
    for (QLineEdit *edit : m_optionsPanel->findChildren<QLineEdit *>()) {
        if (qobject_cast<QComboBox *>(edit->parent()) == nullptr) // コンボの入力欄は上で見ている
            connect(edit, &QLineEdit::textChanged, this, schedule);
    }
}

// 動作中の通訳へ画面の設定を反映する
void InterpreterWindow::applyRunningSettings()
{
    if (!m_session->isRunning())
        return;
    const InterpretationSession::Config config = makeConfig(); // 新しい設定
    QString problem; // 反映できない理由
    if (config.languageA == config.languageB)
        problem = tr("Choose two different languages.");
    else if (!config.captureMicrophone && !config.captureRemote)
        problem = tr("Select at least one audio source.");
    else if (!QFile::exists(config.whisperModel))
        problem = tr("The selected recognition model is not installed.");
    if (!problem.isEmpty()) {
        // 直すまで前の設定のまま続ける
        m_statusLabel->setText(tr("Settings were not applied: %1").arg(problem));
        return;
    }
    saveSettings();
    m_session->applyConfig(config);
}

// モードに合わせて関係する項目だけを有効にする
void InterpreterWindow::updateModeControls()
{
    const bool sendSpeech = InterpretationSession::SelfVoiceMode(m_selfMode->currentData().toInt()) == InterpretationSession::SelfVoiceMode::SendTranslatedSpeech; // 訳の音声を送るか
    m_virtualOutput->setEnabled(sendSpeech);
    m_mixOriginal->setEnabled(sendSpeech);
    m_speakerOutput->setEnabled(m_speakRemote->isChecked());
}

// 列の見出しを言語名にする
void InterpreterWindow::updateColumnTitles()
{
    m_transcript->setColumnTitles(m_languageA->currentText(), m_languageB->currentText());
}

// 字幕をテキストファイルに保存する
void InterpreterWindow::saveTranscript()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Save transcript"), QStringLiteral("interpretation.txt"), tr("Text files (*.txt)")); // 保存先
    if (path.isEmpty())
        return;
    QFile file(path); // 保存するファイル
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, windowTitle(), file.errorString());
        return;
    }
    file.write(m_transcript->toPlainText().toUtf8());
}

// 閉じるときは止めて設定を保存する（窓は隠すだけで、字幕は残す）
void InterpreterWindow::closeEvent(QCloseEvent *event)
{
    m_session->stop();
    saveSettings();
    QWidget::closeEvent(event);
}
