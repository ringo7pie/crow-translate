/* SPDX-FileCopyrightText: 2026 ringo7pie
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "translator/atranslationprovider.h"

#include <QString>
#include <QVariantMap>

class AppSettings;

// 同時通訳だけで使う翻訳の設定。メインの翻訳設定（設定 → 翻訳）とは別に保存する。
// 方式とモデルなど主な項目だけを持ち、プロンプト・タイムアウト・API キーなど細かい項目はメインの設定から借りる
struct InterpreterTranslationSettings {
    ATranslationProvider::ProviderBackend backend = ATranslationProvider::ProviderBackend::Mozhi; // 翻訳の方式
    QString mozhiInstance; // Mozhi のインスタンスの URL
    int mozhiEngine = 0; // Mozhi のエンジン（OnlineTranslator::Engine の値）
    QString localModel; // ローカル翻訳モデルの ID
    QString localAiProvider; // LocalAI の接続先の ID（ollama など）
    QString localAiModel; // LocalAI のモデル名
    QString codexModel; // Codex のモデル
    QString codexEffort; // Codex の推論の強さ
    QString claudeCodeModel; // Claude Code のモデル
    QString claudeCodeEffort; // Claude Code の推論の強さ（空なら CLI の既定）

    // メインの翻訳設定を写した値を返す（初回の初期値）。メインが Copy なら Mozhi にする
    static InterpreterTranslationSettings fromMainSettings(const AppSettings &settings);
    // 通訳の設定（Interpretation/Translation/...）を読む。未保存の項目はメインの設定から写す
    static InterpreterTranslationSettings load(const AppSettings &settings);
    // 通訳の設定として保存する（メインの設定は変えない）
    void save(AppSettings &settings) const;
    // 翻訳方式の applyOptions() に渡す値を作る。主な項目はこの設定、細かい項目はメインの設定から取る
    QVariantMap buildOptions(const AppSettings &settings) const;
};
