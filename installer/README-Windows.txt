Crow Translate 非公式版 4.1.0-local.2 / Windows x64
================================================

このインストーラーには、Crow Translate本体、Qt、Tesseract、日本語・英語の
OCRデータ、Visual C++ランタイム、ライセンス文書を含みます。
インストール完了後、スタートメニューからCrow Translateを起動してください。
画面上の文字を認識するには「設定 → OCR」でエンジンを選択します。

Indra OCR、翻訳モデル、Python、Codex CLIは別途導入します。
このインストーラーを実行するだけでは、これらのモデルはダウンロードされません。

Indra OCR
---------
Python 3.10以降を導入してから、PowerShellで専用環境を作成します。

  $indraEnv = Join-Path $env:LOCALAPPDATA 'Crow Translate/indra-venv'
  python -m venv $indraEnv
  & (Join-Path $indraEnv 'Scripts/python.exe') -m pip install indra-ocr

「設定 → OCR → Indra OCR (local)」で、作成した環境の
Scripts/indra-ocr.exeを指定してください。初回は約76 MBのモデルを取得します。

ローカル翻訳モデル
------------------
PLaMo-2-translate / Index-Translate 2B / Hy-MT2 7B / Seed-X-PPO 7Bに対応します。
Python 3.14、Visual Studio C++ビルドツール、PowerShell 7が必要です。
Releaseのsource.zipをユーザーが書き込めるフォルダーに展開し、そのフォルダーで
READMEの「Managed local translation models (Windows)」の手順を実行します。
モデルの利用条件、ダウンロード容量、変換時の空き容量も確認してください。

準備ができたら「設定 → 翻訳 → ローカル翻訳モデル」で、展開先の
third_party/local-translationを指定し、モデルを選択します。
英日自動切り替えには第一翻訳言語・第二翻訳言語を英語と日本語に設定します。

配布元・ライセンス
------------------
これはKDE公式配布ではなく、ringo7pieによる非公式改変版です。
https://github.com/ringo7pie/crow-translate/releases/latest

アプリ本体: GPL-3.0-or-later
差し替えたアプリケーションアイコン: CC BY-SA 4.0 / Copyright 2026 ringo7pie
第三者ライセンス: インストール先のTHIRD-PARTY-NOTICES.txtとlicensesフォルダー
対応するソースコード・依存ライブラリのソース・SHA-256チェックサムはReleaseにあります。
