// SPDX-FileCopyrightText: 2026 ringo7pie
// SPDX-License-Identifier: GPL-3.0-or-later
#include "interpreter/audiocapture.h"
#include "interpreter/audionormalizer.h"
#include "interpreter/interpretationsession.h"
#include "interpreter/speakerclusterer.h"
#include "interpreter/speakerembedder.h"
#include "optionalcomponents.h"
#include "interpreter/interpretertranslationsettings.h"
#include "interpreter/transcriptview.h"
#include "interpreter/voicesegmenter.h"
#include "interpreter/whisperserver.h"
#include "onlinetranslator.h"
#include "settings/appsettings.h"
#include "testisolation.h"
#include "translator/localmodelstranslationprovider.h"

#include <QApplication>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <cmath>

namespace {
constexpr int sampleRate = 16000; // 試験の音声の周波数

// 指定の長さの正弦波（発話の代わり）を作る。引数: milliseconds = 長さ、amplitude = 振幅
QVector<float> tone(int milliseconds, float amplitude = 0.3f)
{
    QVector<float> samples(qsizetype(milliseconds) * sampleRate / 1000); // 音声
    for (qsizetype index = 0; index < samples.size(); ++index)
        samples[index] = amplitude * float(std::sin(2.0 * M_PI * 220.0 * double(index) / sampleRate));
    return samples;
}

// 指定の長さの無音を作る
QVector<float> silence(int milliseconds)
{
    return QVector<float>(qsizetype(milliseconds) * sampleRate / 1000, 0.0f);
}
}

class InterpreterTest : public QObject
{
    Q_OBJECT
private slots:
    // 発話 2 つが無音で区切られて 2 つ出ること
    void segmenterSplitsOnSilence()
    {
        VoiceSegmenter segmenter; // 試験対象
        QSignalSpy spy(&segmenter, &VoiceSegmenter::utteranceReady);
        segmenter.push(silence(500));
        segmenter.push(tone(1000));
        segmenter.push(silence(800));
        segmenter.push(tone(700));
        segmenter.push(silence(800));
        QCOMPARE(spy.count(), 2);
        const auto first = spy.at(0).at(0).value<QVector<float>>(); // 1 つ目の発話
        // 発話 1 秒 + 前置き 0.2 秒 + 残した無音 0.2 秒 前後
        QVERIFY(first.size() >= sampleRate * 1);
        QVERIFY(first.size() <= sampleRate * 1.5);
    }

    // 短すぎる音は捨てること
    void segmenterDropsShortNoise()
    {
        VoiceSegmenter segmenter; // 試験対象
        QSignalSpy spy(&segmenter, &VoiceSegmenter::utteranceReady);
        segmenter.push(silence(300));
        segmenter.push(tone(100));
        segmenter.push(silence(900));
        QCOMPARE(spy.count(), 0);
    }

    // 長い発話は上限で強制的に区切ること
    void segmenterForcesMaximumLength()
    {
        VoiceSegmenter segmenter; // 試験対象
        VoiceSegmenter::Settings settings; // 上限を 2 秒にする
        settings.maxUtteranceMs = 2000;
        segmenter.setSettings(settings);
        QSignalSpy spy(&segmenter, &VoiceSegmenter::utteranceReady);
        segmenter.push(tone(5000));
        QCOMPARE(spy.count(), 2);
        QVERIFY(segmenter.inSpeech());
        segmenter.flush();
        QCOMPARE(spy.count(), 3);
    }

    // 原文の行に訳文を 1 回だけ入れ、既存の文を書き換えないこと
    void transcriptNeverRewrites()
    {
        TranscriptView view; // 試験対象
        view.setColumnTitles(QStringLiteral("Japanese"), QStringLiteral("English"));
        const QDateTime now = QDateTime::currentDateTime(); // 時刻
        view.addEntry(1, true, 0, QStringLiteral("こんにちは"), now);
        view.addEntry(2, false, 1, QStringLiteral("Hello there"), now);
        QCOMPARE(view.rowCount(), 2);
        QVERIFY(view.setTranslation(2, QStringLiteral("こんにちは、どうも")));
        QVERIFY(view.setTranslation(1, QStringLiteral("Hello")));
        QVERIFY(!view.setTranslation(1, QStringLiteral("Changed")));
        QVERIFY(!view.setTranslation(99, QStringLiteral("Unknown")));
        QCOMPARE(view.item(0, 0)->text(), QStringLiteral("こんにちは"));
        QCOMPARE(view.item(0, 1)->text(), QStringLiteral("Hello"));
        QCOMPARE(view.item(1, 0)->text(), QStringLiteral("こんにちは、どうも"));
        QCOMPARE(view.item(1, 1)->text(), QStringLiteral("Hello there"));
        QVERIFY(view.toPlainText().contains(QStringLiteral("English: Hello there")));
    }

    // 一番下にいれば行が増えても一番下へ追従し、上へ戻したら追従しないこと
    void transcriptFollowsBottom()
    {
        TranscriptView view; // 試験対象
        view.resize(400, 200);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        const QDateTime now = QDateTime::currentDateTime(); // 時刻
        const QString longText = QStringLiteral("long line of transcript text that wraps several times ").repeated(4); // 折り返す長い原文
        QScrollBar *bar = view.verticalScrollBar(); // 縦のスクロールバー
        quint64 entryId = 1; // 次の行の番号
        for (int count = 0; count < 20; ++count)
            view.addEntry(entryId++, count % 2 == 0, count % 2, longText, now);
        QTRY_VERIFY(bar->maximum() > 0);
        QTRY_COMPARE(bar->value(), bar->maximum());
        QVERIFY(view.isFollowingBottom());

        // 上へ戻すと、行が増えても位置を保つ
        bar->setValue(0);
        QVERIFY(!view.isFollowingBottom());
        view.addEntry(entryId++, false, 1, longText, now);
        QTest::qWait(50);
        QCOMPARE(bar->value(), 0);

        // 一番下へ戻すと、また追従する
        bar->setValue(bar->maximum());
        QVERIFY(view.isFollowingBottom());
        const int before = bar->maximum(); // 行を足す前の一番下
        view.addEntry(entryId++, true, 0, longText, now);
        QTRY_VERIFY(bar->maximum() > before);
        QTRY_COMPARE(bar->value(), bar->maximum());
    }

    // 判定言語と話者から列を決めること
    void columnFollowsDetectedLanguage()
    {
        const Language japanese(QStringLiteral("ja")); // 言語 A
        const Language english(QStringLiteral("en")); // 言語 B
        using Speaker = InterpretationSession::Speaker;
        QCOMPARE(InterpretationSession::columnFor(Speaker::Self, QStringLiteral("ja"), japanese, english), 0);
        QCOMPARE(InterpretationSession::columnFor(Speaker::Self, QStringLiteral("en"), japanese, english), 1);
        QCOMPARE(InterpretationSession::columnFor(Speaker::Remote, QStringLiteral("ja"), japanese, english), 0);
        QCOMPARE(InterpretationSession::columnFor(Speaker::Remote, QStringLiteral("de"), japanese, english), 1);
        QCOMPARE(InterpretationSession::columnFor(Speaker::Self, QString(), japanese, english), 0);
    }

    // whisper の幻聴を捨て、普通の文は残すこと
    void cleansHallucinations()
    {
        QCOMPARE(InterpretationSession::cleanTranscript(QStringLiteral(" [BLANK_AUDIO] ")), QString());
        QCOMPARE(InterpretationSession::cleanTranscript(QStringLiteral("(music)")), QString());
        QCOMPARE(InterpretationSession::cleanTranscript(QStringLiteral("♪ ...")), QString());
        QCOMPARE(InterpretationSession::cleanTranscript(QStringLiteral(" Hello (laughs) world. ")), QStringLiteral("Hello world."));
    }

    // whisper の言語名の正規化と、文字種からの推定
    void whisperLanguageHelpers()
    {
        QCOMPARE(WhisperServer::normalizeLanguage(QStringLiteral("japanese")), QStringLiteral("ja"));
        QCOMPARE(WhisperServer::normalizeLanguage(QStringLiteral("English")), QStringLiteral("en"));
        QCOMPARE(WhisperServer::normalizeLanguage(QStringLiteral("ja")), QStringLiteral("ja"));
        QCOMPARE(WhisperServer::guessLanguage(QStringLiteral("これはペンです")), QStringLiteral("ja"));
        QCOMPARE(WhisperServer::guessLanguage(QStringLiteral("This is a pen")), QStringLiteral("en"));
    }

    // WAV の大きさ（ヘッダー 44 バイト + 16 bit × サンプル数）
    void wavHasExpectedSize()
    {
        const QByteArray wav = WhisperServer::toWav(tone(100)); // 0.1 秒の WAV
        QCOMPARE(wav.size(), 44 + 1600 * 2);
        QVERIFY(wav.startsWith("RIFF"));
    }

    // 48 kHz ステレオを 16 kHz mono にしたとき長さが 1/3 になること
    void resamplerKeepsDuration()
    {
        QVector<float> stereo(48000 * 2, 0.5f); // 1 秒分の 48 kHz ステレオ
        const QVector<float> mono = MonoResampler::toMono(reinterpret_cast<const char *>(stereo.constData()),
                                                          stereo.size() * qsizetype(sizeof(float)),
                                                          QAudioFormat::Float,
                                                          2); // mono にした音声
        QCOMPARE(mono.size(), 48000);
        MonoResampler resampler; // 周波数の変換
        resampler.setRates(48000, 16000);
        QVector<float> output; // 16 kHz にした音声
        resampler.process(mono.constData(), mono.size() / 2, output);
        resampler.process(mono.constData() + mono.size() / 2, mono.size() / 2, output);
        QVERIFY(std::abs(output.size() - 16000) <= 1);
        QVERIFY(std::abs(output.first() - 0.5f) < 1e-6f);
    }

    // 通訳の翻訳設定が未保存なら、メインの翻訳設定を写すこと
    void translationSettingsCopyMainWhenUnsaved()
    {
        QSettings().remove(QStringLiteral("Interpretation"));
        AppSettings settings; // メインの設定
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Codex);
        settings.setCodexModel(QStringLiteral("main-codex-model"));
        settings.setCodexReasoningEffort(QStringLiteral("high"));
        const auto loaded = InterpreterTranslationSettings::load(settings); // 読んだ通訳の設定
        QCOMPARE(loaded.backend, ATranslationProvider::ProviderBackend::Codex);
        QCOMPARE(loaded.codexModel, QStringLiteral("main-codex-model"));
        QCOMPARE(loaded.codexEffort, QStringLiteral("high"));

        // Copy（訳さない）は通訳では Mozhi にする
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Copy);
        QCOMPARE(InterpreterTranslationSettings::load(settings).backend, ATranslationProvider::ProviderBackend::Mozhi);
    }

    // 保存した通訳の設定は、後でメインの設定を変えても変わらないこと
    void translationSettingsAreIndependent()
    {
        QSettings().remove(QStringLiteral("Interpretation"));
        AppSettings settings; // メインの設定
        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::LocalAI);
        InterpreterTranslationSettings saved = InterpreterTranslationSettings::load(settings); // 通訳の設定
        saved.backend = ATranslationProvider::ProviderBackend::Mozhi;
        saved.mozhiInstance = QStringLiteral("https://mozhi.example.org");
        saved.mozhiEngine = int(OnlineTranslator::Deepl);
        saved.save(settings);

        settings.setTranslationProviderBackend(ATranslationProvider::ProviderBackend::Codex);
        settings.setInstance(QStringLiteral("https://main.example.org"));
        const auto loaded = InterpreterTranslationSettings::load(settings); // 読み直した通訳の設定
        QCOMPARE(loaded.backend, ATranslationProvider::ProviderBackend::Mozhi);
        QCOMPARE(loaded.mozhiInstance, QStringLiteral("https://mozhi.example.org"));
        QCOMPARE(loaded.mozhiEngine, int(OnlineTranslator::Deepl));
        QCOMPARE(settings.translationProviderBackend(), ATranslationProvider::ProviderBackend::Codex);

        const QVariantMap options = loaded.buildOptions(settings); // 翻訳方式に渡す値
        QCOMPARE(options.value(QStringLiteral("instance")).toString(), QStringLiteral("https://mozhi.example.org"));
        QCOMPARE(options.value(QStringLiteral("engine")).toInt(), int(OnlineTranslator::Deepl));
    }

    // ローカル翻訳モデルでは、通訳側のモデルとそのモデルの prompt / instructions を必ず渡すこと
    void translationOptionsForLocalModels()
    {
        const QStringList models = LocalModelsTranslationProvider::modelIds(); // モデルの一覧
        QVERIFY(models.size() >= 2);
        AppSettings settings; // メインの設定
        settings.setLocalModelsModel(models.at(0));
        settings.setLocalModelsPrompt(models.at(1), QStringLiteral("interpreter prompt {text}"));
        settings.setLocalModelsInstructions(models.at(1), QStringLiteral("interpreter instructions"));

        InterpreterTranslationSettings translation; // 通訳の設定
        translation.backend = ATranslationProvider::ProviderBackend::LocalModels;
        translation.localModel = models.at(1);
        const QVariantMap options = translation.buildOptions(settings); // 翻訳方式に渡す値
        QCOMPARE(options.value(QStringLiteral("model")).toString(), models.at(1));
        QCOMPARE(options.value(QStringLiteral("prompt")).toString(), QStringLiteral("interpreter prompt {text}"));
        QCOMPARE(options.value(QStringLiteral("instructions")).toString(), QStringLiteral("interpreter instructions"));
        QVERIFY(options.contains(QStringLiteral("root")));
        QCOMPARE(settings.localModelsModel(), models.at(0));
    }

    // Codex では通訳側のモデルと推論の強さ、メインのプロンプトを渡すこと
    void translationOptionsForCodex()
    {
        AppSettings settings; // メインの設定
        settings.setCodexModel(QStringLiteral("main-model"));
        settings.setCodexPrompt(QStringLiteral("main prompt {text}"));
        InterpreterTranslationSettings translation; // 通訳の設定
        translation.backend = ATranslationProvider::ProviderBackend::Codex;
        translation.codexModel = QStringLiteral("interpreter-model");
        translation.codexEffort = QStringLiteral("low");
        const QVariantMap options = translation.buildOptions(settings); // 翻訳方式に渡す値
        QCOMPARE(options.value(QStringLiteral("model")).toString(), QStringLiteral("interpreter-model"));
        QCOMPARE(options.value(QStringLiteral("reasoningEffort")).toString(), QStringLiteral("low"));
        QCOMPARE(options.value(QStringLiteral("prompt")).toString(), QStringLiteral("main prompt {text}"));
        QVERIFY(!options.contains(QStringLiteral("instance")));
    }

    // 話者名と色が行見出しとセルに付き、保存用の文字にも出ること
    void transcriptShowsSpeakerLabel()
    {
        TranscriptView view; // 試験対象
        view.setColumnTitles(QStringLiteral("Japanese"), QStringLiteral("English"));
        const QDateTime now = QDateTime::currentDateTime(); // 時刻
        view.addEntry(1, false, 1, QStringLiteral("Hello"), now, QStringLiteral("Speaker A"), 0);
        view.addEntry(2, false, 1, QStringLiteral("Hi"), now, QStringLiteral("Speaker B"), 1);
        view.addEntry(3, false, 1, QStringLiteral("Hey"), now);
        QVERIFY(view.verticalHeaderItem(0)->text().endsWith(QStringLiteral("Speaker A")));
        QVERIFY(view.verticalHeaderItem(1)->text().endsWith(QStringLiteral("Speaker B")));
        QVERIFY(!view.verticalHeaderItem(2)->text().contains(QStringLiteral("Speaker")));
        QCOMPARE(view.item(0, 1)->foreground().color(), TranscriptView::speakerColor(0));
        QVERIFY(TranscriptView::speakerColor(0) != TranscriptView::speakerColor(1));
        QCOMPARE(TranscriptView::speakerColor(8), TranscriptView::speakerColor(0));
        QVERIFY(view.toPlainText().contains(QStringLiteral("Speaker B]")));
    }

    // 声の違う 2 人を分け、同じ人には同じ番号を付けること
    void clustererSeparatesSpeakers()
    {
        SpeakerClusterer clusterer; // 試験対象
        const QVector<float> personA{1.0f, 0.1f, 0.0f, 0.0f}; // A さんの声（合成）
        const QVector<float> personAAgain{0.9f, 0.2f, 0.05f, 0.0f}; // 少し揺れた A さんの声
        const QVector<float> personB{0.0f, 0.1f, 1.0f, 0.2f}; // B さんの声（合成）
        QCOMPARE(clusterer.assign(personA, 2.0), 0);
        QCOMPARE(clusterer.assign(personB, 2.0), 1);
        QCOMPARE(clusterer.assign(personAAgain, 2.0), 0);
        QCOMPARE(clusterer.assign(personB, 2.0), 1);
        QCOMPARE(clusterer.speakerCount(), 2);
        QCOMPARE(SpeakerClusterer::letterFor(0), QStringLiteral("A"));
        QCOMPARE(SpeakerClusterer::letterFor(1), QStringLiteral("B"));
        QCOMPARE(SpeakerClusterer::letterFor(26), QStringLiteral("A2"));
        QCOMPARE(SpeakerClusterer::letterFor(-1), QStringLiteral("?"));
    }

    // 短い発話は重心を動かさず、知らない声なら「?」（-1）にすること
    void clustererKeepsCentroidForShortUtterances()
    {
        SpeakerClusterer clusterer; // 試験対象
        const QVector<float> personA{1.0f, 0.0f, 0.0f}; // A さんの声
        QCOMPARE(clusterer.assign(personA, 2.0), 0);
        const QVector<float> before = clusterer.centroid(0); // 短い発話の前の重心
        QCOMPARE(clusterer.assign(QVector<float>{0.9f, 0.3f, 0.0f}, 0.5), 0);
        QCOMPARE(clusterer.centroid(0), before);
        QCOMPARE(clusterer.assign(QVector<float>{0.0f, 0.0f, 1.0f}, 0.5), -1);
        QCOMPARE(clusterer.speakerCount(), 1);
    }

    // 話者の数は上限を超えないこと
    void clustererRespectsMaximumSpeakers()
    {
        SpeakerClusterer clusterer; // 試験対象
        clusterer.setMaximumSpeakers(2);
        QCOMPARE(clusterer.assign(QVector<float>{1.0f, 0.0f, 0.0f}, 2.0), 0);
        QCOMPARE(clusterer.assign(QVector<float>{0.0f, 1.0f, 0.0f}, 2.0), 1);
        const int third = clusterer.assign(QVector<float>{0.0f, 0.0f, 1.0f}, 2.0); // 3 人目の声
        QVERIFY(third == 0 || third == 1);
        QCOMPARE(clusterer.speakerCount(), 2);
        QCOMPARE(clusterer.assign(QVector<float>(), 2.0), -1);
    }

    // 小さい音を大きくし、ピークを抑え、無音は増幅しないこと
    void normalizerLevelsLoudness()
    {
        const QVector<float> quiet = AudioNormalizer::normalize(tone(1000, 0.01f)); // 小さい音
        double squareSum = 0.0; // 二乗の合計
        float peak = 0.0f; // ピーク
        for (const float sample : quiet) {
            squareSum += double(sample) * sample;
            peak = (std::max)(peak, std::abs(sample));
        }
        const double rms = std::sqrt(squareSum / quiet.size()); // 実効値
        QVERIFY(rms > 0.05); // 0.007 → 約 0.07（上限 10 倍）
        QVERIFY(peak <= AudioNormalizer::peakLimit + 1e-6f);

        QVector<float> spiky = tone(1000, 0.05f); // 大きな一点を含む音
        spiky[100] = 0.9f;
        for (const float sample : AudioNormalizer::normalize(spiky))
            QVERIFY(std::abs(sample) <= AudioNormalizer::peakLimit + 1e-6f);

        QVector<float> offsetSilence(16000, 0.2f); // 直流分だけの無音
        for (const float sample : AudioNormalizer::normalize(offsetSilence))
            QVERIFY(std::abs(sample) < 1e-6f);
        QVERIFY(AudioNormalizer::normalize(QVector<float>()).isEmpty());
    }

    // 言語 A・B 以外と判定されたときだけ認識し直すこと
    void languageRetryOnlyForOtherLanguages()
    {
        const Language japanese(QStringLiteral("ja")); // 言語 A
        const Language english(QStringLiteral("en")); // 言語 B
        QVERIFY(!InterpretationSession::needsLanguageRetry(QStringLiteral("ja"), japanese, english));
        QVERIFY(!InterpretationSession::needsLanguageRetry(QStringLiteral("en"), japanese, english));
        QVERIFY(InterpretationSession::needsLanguageRetry(QStringLiteral("zh"), japanese, english));
        QVERIFY(InterpretationSession::needsLanguageRetry(QStringLiteral("nn"), japanese, english));
        QVERIFY(!InterpretationSession::needsLanguageRetry(QString(), japanese, english));
    }

    // 手がかりは用語と直前の発言の末尾から作ること
    void promptCombinesGlossaryAndContext()
    {
        QCOMPARE(InterpretationSession::buildPrompt(QString(), QString()), QString());
        QCOMPARE(InterpretationSession::buildPrompt(QStringLiteral(" Crow,  Kubernetes "), QString()), QStringLiteral("Crow, Kubernetes"));
        QCOMPARE(InterpretationSession::buildPrompt(QString(), QStringLiteral("Hello.")), QStringLiteral("Hello."));
        const QString longText = QString(500, QLatin1Char('x')) + QStringLiteral("END"); // 長い直前の発言
        const QString prompt = InterpretationSession::buildPrompt(QStringLiteral("Term"), longText); // 手がかり
        QVERIFY(prompt.startsWith(QStringLiteral("Term ")));
        QVERIFY(prompt.endsWith(QStringLiteral("END")));
        QVERIFY(prompt.size() <= 5 + 200);
    }

    // 導入済みの部品で埋め込みを求められること（部品が無ければ飛ばす）
    void speakerEmbedderLive()
    {
        if (!OptionalComponents::speakerInstalled())
            QSKIP("The speaker identification component is not installed.");
        SpeakerEmbedder embedder; // 試験対象
        QVERIFY2(embedder.load(OptionalComponents::speakerLibrary(), OptionalComponents::speakerModel()), qPrintable(embedder.lastError()));
        QVERIFY(embedder.dimension() > 0);
        const QVector<float> voice = AudioNormalizer::normalize(tone(2000)); // 2 秒の音
        const QVector<float> first = embedder.compute(voice); // 1 回目の埋め込み
        const QVector<float> second = embedder.compute(voice); // 2 回目の埋め込み
        QCOMPARE(first.size(), embedder.dimension());
        QVERIFY(SpeakerClusterer::cosine(first, second) > 0.99f);
    }
};

int main(int argc, char *argv[])
{
    isolateTestSettings();
    QApplication app(argc, argv);
    InterpreterTest test; // 試験
    return QTest::qExec(&test, argc, argv);
}
#include "test_interpreter.moc"
