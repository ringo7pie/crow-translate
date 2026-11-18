/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "interpretertranslationsettings.h"

#include "settings/appsettings.h"

namespace {
using Backend = ATranslationProvider::ProviderBackend;

// 通訳の翻訳設定の保存キー（Interpretation/ の下）を作る
QString translationKey(const char *name)
{
    return QStringLiteral("Translation/") + QLatin1String(name);
}

// 通訳で選べる方式か（Copy は訳さないので選ばせない）
bool isSelectableBackend(int value)
{
    switch (Backend(value)) {
    case Backend::Mozhi:
    case Backend::LocalAI:
    case Backend::Codex:
    case Backend::LocalModels:
    case Backend::ClaudeCode:
        return true;
    case Backend::Copy:
        break;
    }
    return false;
}
}

// メインの翻訳設定を写す
InterpreterTranslationSettings InterpreterTranslationSettings::fromMainSettings(const AppSettings &settings)
{
    InterpreterTranslationSettings result; // 写した設定
    const Backend mainBackend = settings.translationProviderBackend(); // メインの方式
    result.backend = isSelectableBackend(int(mainBackend)) ? mainBackend : Backend::Mozhi;
    result.mozhiInstance = settings.instance();
    result.mozhiEngine = int(settings.currentEngine());
    result.localModel = settings.localModelsModel();
    result.localAiProvider = settings.activeLocalProvider();
    result.localAiModel = settings.localProviderModel(result.localAiProvider);
    result.codexModel = settings.codexModel();
    result.codexEffort = settings.codexReasoningEffort();
    result.claudeCodeModel = settings.claudeCodeModel();
    result.claudeCodeEffort = settings.claudeCodeReasoningEffort();
    return result;
}

// 通訳の翻訳設定を読む
InterpreterTranslationSettings InterpreterTranslationSettings::load(const AppSettings &settings)
{
    const InterpreterTranslationSettings defaults = fromMainSettings(settings); // 未保存の項目の値
    InterpreterTranslationSettings result; // 読んだ設定
    bool backendOk = false; // 保存値が数値として読めたか
    const int backendValue = settings.interpretationValue(translationKey("Backend"), int(defaults.backend)).toInt(&backendOk); // 保存した方式
    result.backend = backendOk && isSelectableBackend(backendValue) ? Backend(backendValue) : defaults.backend;
    result.mozhiInstance = settings.interpretationValue(translationKey("MozhiInstance"), defaults.mozhiInstance).toString();
    result.mozhiEngine = settings.interpretationValue(translationKey("MozhiEngine"), defaults.mozhiEngine).toInt();
    result.localModel = settings.interpretationValue(translationKey("LocalModel"), defaults.localModel).toString();
    result.localAiProvider = settings.interpretationValue(translationKey("LocalAiProvider"), defaults.localAiProvider).toString();
    if (!AppSettings::localProviderIds().contains(result.localAiProvider))
        result.localAiProvider = defaults.localAiProvider;
    // 接続先を変えて保存していればその接続先のモデル、なければメインの値
    const QString providerModel = settings.localProviderModel(result.localAiProvider); // 接続先のメインのモデル
    result.localAiModel = settings.interpretationValue(translationKey("LocalAiModel"), providerModel).toString();
    result.codexModel = settings.interpretationValue(translationKey("CodexModel"), defaults.codexModel).toString();
    result.codexEffort = settings.interpretationValue(translationKey("CodexEffort"), defaults.codexEffort).toString();
    if (!AppSettings::codexReasoningEfforts().contains(result.codexEffort))
        result.codexEffort = defaults.codexEffort;
    result.claudeCodeModel = settings.interpretationValue(translationKey("ClaudeCodeModel"), defaults.claudeCodeModel).toString();
    result.claudeCodeEffort = settings.interpretationValue(translationKey("ClaudeCodeEffort"), defaults.claudeCodeEffort).toString();
    if (!AppSettings::claudeCodeReasoningEfforts().contains(result.claudeCodeEffort))
        result.claudeCodeEffort = defaults.claudeCodeEffort;
    return result;
}

// 通訳の翻訳設定を保存する
void InterpreterTranslationSettings::save(AppSettings &settings) const
{
    settings.setInterpretationValue(translationKey("Backend"), int(backend));
    settings.setInterpretationValue(translationKey("MozhiInstance"), mozhiInstance);
    settings.setInterpretationValue(translationKey("MozhiEngine"), mozhiEngine);
    settings.setInterpretationValue(translationKey("LocalModel"), localModel);
    settings.setInterpretationValue(translationKey("LocalAiProvider"), localAiProvider);
    settings.setInterpretationValue(translationKey("LocalAiModel"), localAiModel);
    settings.setInterpretationValue(translationKey("CodexModel"), codexModel);
    settings.setInterpretationValue(translationKey("CodexEffort"), codexEffort);
    settings.setInterpretationValue(translationKey("ClaudeCodeModel"), claudeCodeModel);
    settings.setInterpretationValue(translationKey("ClaudeCodeEffort"), claudeCodeEffort);
}

// 翻訳方式に渡す値を作る
QVariantMap InterpreterTranslationSettings::buildOptions(const AppSettings &settings) const
{
    QVariantMap options; // applyOptions() に渡す値
    switch (backend) {
    case Backend::Mozhi:
        options.insert(QStringLiteral("instance"), mozhiInstance.isEmpty() ? settings.instance() : mozhiInstance);
        options.insert(QStringLiteral("engine"), mozhiEngine);
        options.insert(QStringLiteral("apikey"), settings.libreTranslateApiKey());
        options.insert(QStringLiteral("direct"), settings.libreTranslateDirect());
        break;
    case Backend::LocalModels: {
        const QString model = localModel.isEmpty() ? settings.localModelsModel() : localModel; // 使うモデル
        options.insert(QStringLiteral("root"), settings.localModelsRoot());
        options.insert(QStringLiteral("model"), model);
        options.insert(QStringLiteral("timeout"), settings.localModelsTimeout());
        options.insert(QStringLiteral("vramReserve"), settings.localModelsVramReserve());
        // モデルを変えると prompt / instructions が空に戻るので、必ず一緒に渡す
        options.insert(QStringLiteral("prompt"), settings.localModelsPrompt(model));
        options.insert(QStringLiteral("instructions"), settings.localModelsInstructions(model));
        break;
    }
    case Backend::LocalAI: {
        const QString provider = localAiProvider.isEmpty() ? settings.activeLocalProvider() : localAiProvider; // 接続先
        const QString model = localAiModel.isEmpty() ? settings.localProviderModel(provider) : localAiModel; // 使うモデル
        options.insert(QStringLiteral("url"), settings.localProviderUrl(provider));
        options.insert(QStringLiteral("model"), model);
        options.insert(QStringLiteral("prompt"), settings.localAiPrompt(model));
        options.insert(QStringLiteral("disable_thinking"), settings.localAiDisableThinking(provider));
        options.insert(QStringLiteral("timeout"), settings.localAiTimeout(provider));
        options.insert(QStringLiteral("api_key"), settings.localProviderApiKey(provider));
        options.insert(QStringLiteral("is_anthropic"), AppSettings::localProviderIsAnthropic(provider));
        // 言語の判定に使うモデルはメインの設定のまま
        const QString detectProvider = settings.detectProvider(); // 言語判定の接続先
        options.insert(QStringLiteral("detect_url"), settings.localProviderUrl(detectProvider));
        options.insert(QStringLiteral("detect_model"), settings.detectModel());
        options.insert(QStringLiteral("detect_api_key"), settings.localProviderApiKey(detectProvider));
        options.insert(QStringLiteral("detect_is_anthropic"), AppSettings::localProviderIsAnthropic(detectProvider));
        break;
    }
    case Backend::Codex:
        options.insert(QStringLiteral("executable"), settings.codexExecutable());
        options.insert(QStringLiteral("model"), codexModel.isEmpty() ? settings.codexModel() : codexModel);
        options.insert(QStringLiteral("reasoningEffort"), codexEffort);
        options.insert(QStringLiteral("timeout"), settings.codexTimeout());
        options.insert(QStringLiteral("instructions"), settings.codexInstructions());
        options.insert(QStringLiteral("prompt"), settings.codexPrompt());
        break;
    case Backend::ClaudeCode:
        options.insert(QStringLiteral("executable"), settings.claudeCodeExecutable());
        options.insert(QStringLiteral("model"), claudeCodeModel.isEmpty() ? settings.claudeCodeModel() : claudeCodeModel);
        options.insert(QStringLiteral("reasoningEffort"), claudeCodeEffort);
        options.insert(QStringLiteral("timeout"), settings.claudeCodeTimeout());
        options.insert(QStringLiteral("instructions"), settings.claudeCodeInstructions());
        options.insert(QStringLiteral("prompt"), settings.claudeCodePrompt());
        break;
    case Backend::Copy:
        break;
    }
    return options;
}
