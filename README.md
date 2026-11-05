<!-- SPDX-FileCopyrightText: none -->
<!-- SPDX-License-Identifier: CC0-1.0 -->

# ![Crow Translate logo](data/icons/app/48-apps-org.kde.CrowTranslate.png) Crow Translate

## Unofficial fork / 非公式改変版

This is an unofficial modified version of [Crow Translate](https://invent.kde.org/office/crow-translate),
maintained by [ringo7pie](https://github.com/ringo7pie). It is not an official KDE release.
The upstream authors' copyright notices and license texts are retained.

Modified on **2026-11-01**: added local Indra OCR, PLaMo-2-translate,
Index-Translate 2B, Hy-MT2 7B and Seed-X-PPO 7B translation presets, Codex CLI
translation, translation progress and cancellation controls, Japanese localization,
Windows build/installer support, and replacement application icons.

Modified on **2026-11-05**: Windows installer **4.1.0-local.14** adds Claude
Code CLI translation with Haiku 4.5 as its default model, executable discovery,
editable instructions, language detection and cancellation. The source editor
now supports Ctrl+mouse-wheel font zoom, independently of the translated text.

Modified on **2026-11-05**: Windows installer **4.1.0-local.13** adds a fallback
that queries running Codex image paths for the same Windows user and session,
preferring the managed `current/bin` path across updates. Native executable
checks are independent of Qt's global NTFS permission-lookup setting.

Modified on **2026-11-04**: Windows installer **4.1.0-local.12** also finds the
desktop-managed Codex CLI under `CODEX_HOME` (by default `~/.codex`), including
the stable `packages/app-server-daemon/current/bin/codex.exe` path.

Modified on **2026-11-02**: added an optional installer for Indra OCR and each
translation model, with license consent, private Python, resumable verified
downloads, cancellation and automatic detection of the selected data directory.
The setup now offers a default translation model selector and removes verified
download archives and PLaMo conversion inputs after successful installation.
Indra and the recommended CAT model start selected; existing application settings stay unchanged
unless a default translation model is explicitly chosen.
Local translation presets now have separately saved basic and additional
instructions for each model. Codex translation also exposes its basic
instructions alongside the existing additional preferences. Both can be edited
in Settings > Translation; edits apply on OK and can be restored to defaults.
GPU loading now uses a smaller configurable VRAM reserve, accounts for other
NVIDIA workloads, and reduces resident RAM from model-file mappings. An optional
sentence-ending line-break setting formats both input and translation requests.
Codex discovery also supports installed launches with a stale or minimal PATH.
Added RTX 3070 support through an optional CUDA 12.4 runtime, with Vulkan/CPU
fallback. PLaMo uses smaller physical batches on CUDA to avoid invalid generation.
CAT-Translate-3.3B is available as a separately downloadable Q4_K_M model converted
from the official CyberAgent release, retaining its MIT notices and chat format.
Run the additional-components setup and select a translation model to add CUDA
to an existing installation; verified existing model files are reused.
Optional PLaMo integration: **Built with PLaMo**. Its Community License requires
PFN registration and other conditions for commercial use; the optional setup
shows the full model licenses and checks consent before downloading any model.

Modified on **2026-11-03**: added an in-app local AI manager, editable shortcuts
for common actions, and an option to hide Crow's windows during OCR capture.
Open **Manage local AI** from the settings-button menu, tray menu, or Translation
and OCR settings to install or repair any of the six existing components. Read
the full licenses and confirm eligibility where required before downloading.
Choose **Use this directory / selected model** to activate an installed component;
when opened from Settings this choice is provisional until OK. Cancel keeps
installed files while discarding setting changes. A detected installation in a
different directory can be selected without downloading its models again.

日本語: 「ローカルAIの管理」で未導入のOCR・翻訳モデルを選び、ライセンスに
同意してインストールできます。導入後は「この保存先・モデルを使用」で反映します。
**Ctrl+Shift+Z** は設定した遅延の後に画面の領域を選択し、OCRして翻訳します。
遅延は「設定 → OCR」で変更できます。**Ctrl+Alt+I** はOCRのみ、
**Ctrl+Alt+O** は遅延なしでキャプチャーから翻訳まで実行します。
インストール時の推奨モデルは **CAT-Translate-3.3B** です。IndraとCATを初期選択し、
新規導入時はCATを既定にします。更新時は既存モデルの設定を維持します。
「設定 → OCR」の「キャプチャー中はCrowのウィンドウを隠す」は初期状態でONです。
成功するとOCR結果を表示し、キャンセル・失敗時は元のウィンドウ状態に戻ります。
キーの割り当ては「設定 → ショートカット」で編集・解除・初期化できます。

The application code and these code additions are distributed under
[GPL-3.0-or-later](LICENSES/GPL-3.0-or-later.txt). The replacement application
icons and their JPEG sources are Copyright 2026 ringo7pie, licensed under
[CC BY-SA 4.0](LICENSES/CC-BY-SA-4.0.txt). Other files retain their individual
licenses, recorded in SPDX headers and [.reuse/dep5](.reuse/dep5).
Model weights, downloaded runtimes, personal settings and build outputs are not
included in this repository. Separately installed models and dependencies have
their own licenses; this fork's GPL does not replace those terms.

このリポジトリは非公式の改変版です。元の著作権表示とライセンスを維持し、
上記機能を追加しています。モデル本体や個人設定は含みません。

Application written in **C++ / Qt6** that allows you to translate and speak text using [Mozhi](https://codeberg.org/aryak/mozhi) with multiple TTS providers including high-quality neural text-to-speech.

## Content

- [Screenshots](#screenshots)
- [Validation](#validation)
- [Features](#features)
- [Default keyboard shortcuts](#default-keyboard-shortcuts)
- [CLI commands](#cli-commands)
- [D-Bus API](#d-bus-api)
- [Global shortcuts in Wayland](#global-shortcuts-in-wayland)
- [OCR](#ocr)
- [Dependencies](#dependencies)
- [Icons](#icons)
- [Installation](#installation)
- [Building](#building)
- [Localization](#localization)

## Validation

On 2026-11-01, Windows 11 with Qt 6.8.3, a Ryzen 9 3900X and an RTX 3070
(8 GB VRAM) was used to verify English/Japanese translation with all four local
models, local source-language detection, and automatic destination switching in
the GUI. An English test image was recognized by Indra OCR and translated by
Index-Translate 2B through Crow's CLI. Dates, numbers and paragraph samples were
also checked in both translation directions. These are smoke checks, not a
translation-quality benchmark.

On 2026-11-02, the unchanged PLaMo Q4_0-ssm-f16 model was compared on the same
RTX 3070 using Vulkan and CUDA with a physical batch of 16 for CUDA. Four short
and multi-paragraph English/Japanese samples took 2.40/2.09/21.57/15.99 seconds
with Vulkan and 0.66/0.55/5.30/4.07 seconds with CUDA, excluding initial loading.
Initial loading took 14.39 and 11.12 seconds respectively. These are measurements
of those samples with about 1.1 GiB already in use on the GPU; available VRAM,
other applications and input length affect results. CAT is checked with its
official fast tokenizer for both input and output, including numbers and times.

Regression tests cover provider state and cancellation, timeouts, Windows child
process cleanup, settings persistence, and model-specific prompt/token handling.
The model setup scripts use pinned revisions and record checksums in a local
manifest. Re-run the tests and installation checks on your own environment.

Publication checks on the same date: 162 C++ test cases passed, with 5 skipped
for live-service availability or unsupported mid-flight-abort scenarios; all
10 Python worker tests passed. Windows installer/setup PowerShell files parsed
without errors. The test run also verified that the user's settings were unchanged.

## Screenshots

**Plasma**

<p align="center">
  <img src="https://invent.kde.org/websites/product-screenshots/-/raw/master/crow-translate/main.png" alt="Main"/>
</p>

**Plasma Mobile**

<p align="center">
  <img src="https://invent.kde.org/websites/product-screenshots/-/raw/master/crow-translate/main-mobile.png"alt="Main"/>
</p>

**Windows 10**

<p align="center">
  <img src="https://invent.kde.org/websites/product-screenshots/-/raw/master/crow-translate/main-windows.png" alt="Main"/>
</p>

## Features

- **Provider System**: Modular translation and TTS providers with runtime switching
- **Multiple Translation Engines**: [Multiple engines](https://codeberg.org/aryak/mozhi#supported-engines) provided by Mozhi (some instances can disable specific engines)*
- **LocalAI Backend**: Translate through any OpenAI-compatible endpoint - Ollama, LM Studio, FastFlowLM, Anthropic or a custom server - with model discovery and configurable prompts
- **Managed Local Translation**: Switch between PLaMo-2-translate, Index-Translate 2B, Hy-MT2 7B, Seed-X-PPO 7B and CAT-Translate-3.3B, with local English/Japanese detection and automatic model loading
- **Codex CLI Backend**: Translate with your existing Codex CLI login, automatic source-language detection and editable translation instructions
- **LibreTranslate**: Use a LibreTranslate instance through Mozhi or directly, with an optional API key
- **Advanced TTS Options**:
  - **Piper Neural TTS**: High-quality offline neural text-to-speech with ONNX Runtime
  - **Qt TextToSpeech**: System TTS integration
  - **Mozhi TTS**: Online TTS via Mozhi proxy
- **Modular OCR**: Three interchangeable engines - Tesseract, local Indra OCR, and any vision model reachable over an OpenAI-compatible endpoint
- **Image Input**: Open, drop or paste an image to recognize and translate the text in it
- **Status Strip**: See what is currently running - capture, recognition, detection, translation or speech - and what failed
- **Custom Language Support**: Register and persist custom languages beyond standard locales
- **Screen Translation**: Translate and speak text from screen captures or text selection
- **Wayland Support**: Improved screenshot integration with KWin ScreenShot2 API
- **Highly Customizable Shortcuts**: Global hotkeys with desktop environment integration
- **Command-line Interface**: Rich CLI with provider selection and output formatting
- **D-Bus API**: Complete automation interface
- **Cross-platform**: Qt6-based with Linux, FreeBSD, and Windows support

:warning: *While Mozhi acts as a proxy to protect your privacy, the third-party services it uses may store and analyze the text you send. 

## Default keyboard shortcuts

You can change them in the settings. Some key sequences may not be available due to OS limitations.

Wayland does not support global shortcuts registration, but you can use [D-Bus](#d-bus-api) to bind actions in the system settings. For desktop environments that support [additional applications actions](https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html#extra-actions) (KDE, for example) you will see them predefined in the system shortcut settings. You can also use them for X11 sessions, but you need to disable global shortcuts registration in the application settings to avoid conflicts.

### Global

| Key                                             | Description                        |
| ----------------------------------------------- | ---------------------------------- |
| <kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>E</kbd> | Translate selected text            |
| <kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>S</kbd> | Speak selected text                |
| <kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>F</kbd> | Speak translation of selected text |
| <kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>G</kbd> | Stop speaking                      |
| <kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>D</kbd> | Speak / pause                      |
| <kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>C</kbd> | Show main window                   |
| <kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>I</kbd> | Recognize screen area into input (OCR only) |
| <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>Z</kbd> | Translate screen area after the configured delay |
| <kbd>Ctrl</kbd> + <kbd>Alt</kbd> + <kbd>O</kbd> | Translate text in screen area      |

### In main window

| Key                                               | Description                             |
| ------------------------------------------------- | --------------------------------------- |
| <kbd>Ctrl</kbd> + <kbd>Return</kbd>               | Translate                               |
| <kbd>Ctrl</kbd> + <kbd>R</kbd>                    | Swap languages                          |
| <kbd>Ctrl</kbd> + <kbd>Q</kbd>                    | Close window                            |
| <kbd>Ctrl</kbd> + <kbd>S</kbd>                    | Speak source / pause text speaking      |
| <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>S</kbd> | Speak translation / pause text speaking |
| <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>C</kbd> | Copy translation to clipboard           |
| <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>X</kbd> | Cancel translation                      |
| <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>V</kbd> | Paste clipboard text into input         |
| <kbd>Ctrl</kbd> + <kbd>PageUp</kbd> / <kbd>PageDown</kbd> | Previous / next installed local model |
| <kbd>Ctrl</kbd> + <kbd>,</kbd> | Open settings |
| <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>P</kbd> | Edit current local model / Codex / Claude Code instructions |
| <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>M</kbd> | Manage local AI |

Ctrl+Shift+Z is migrated once to delayed capture+translate, removing old OCR or
inversion bindings of that key. Saved Ctrl+Alt+G duplicates are also repaired:
G stops speech, and D plays/pauses it when available. Other custom bindings and
edits made after migration are retained. Conflicts must be resolved before saving;
a failed global registration restores previous bindings. Global shortcuts depend
on OS support and keys claimed by other apps.

## CLI commands

### Translation with Claude Code

Install the native [Claude Code CLI](https://code.claude.com/docs/en/setup)
and sign in using `claude auth login`. Claude Code 2.1.318 or later is the
supported baseline. In **Settings → Translation**, select **Claude Code**.
The executable can be found automatically or selected manually. Windows native
installations under `~/.local/bin` are found even with an outdated PATH.

The default model is **Haiku 4.5**, pinned to `claude-haiku-4-5-20251001`.
You can select Sonnet, Opus or enter another model ID. Models use the access
available to your CLI login. Haiku 4.5 uses the CLI's thinking defaults; the
adaptive effort selector is disabled for this model. Timeout, basic instructions
and additional instructions can be edited like Codex, including via Ctrl+Shift+P.

```bash
crow --translation-provider claude-code --source en --translation ja "Hello, world!"
crow --translation-provider claude-code --translation ja --json "Hello, world!"
```

Each request runs in a fresh temporary directory with built-in tools, MCP
servers and user customizations disabled. Existing CLI authentication is reused;
Crow does not store a Claude API key or bundle Claude Code. Requests do not save
conversation history. Language detection, OCR-to-translation, progress and
cancellation use the same application controls as other providers.

日本語: 「設定 → 翻訳」で「Claude Code」を選択し、「自動検出」でCLIを設定できます。
入力欄と翻訳欄は、それぞれCtrl＋マウスホイールで文字を拡大・縮小できます。
この操作は表示用で、保存済みフォント設定は変更しません。

### Translation with Codex

Install a current [Codex CLI](https://learn.chatgpt.com/docs/cli) and run
`codex login` first. In **Settings → Translation**, select **Codex** and, if
needed, browse to the native Codex executable (`codex.exe` on Windows).
The executable must be on PATH or specified by its full path; on Windows,
select the native executable rather than an npm `.cmd` launcher.

Codex uses its existing CLI authentication. Crow does not store an API key.
The default model is `gpt-6-luna` with low reasoning effort, and the timeout
is 300 seconds. In Translation settings, choose the model and reasoning effort
(`low`, `medium`, `high`, `xhigh` or `max`) from the drop-down lists. The model
field also accepts custom names. You can change the timeout and additional
style or terminology instructions there. Model access depends on
your Codex account; unavailable models produce an error without switching models.

```sh
crow --translation-provider codex --source en --translation ja "Hello, world!"
crow --translation-provider codex --translation ja --json "Hello, world!"
```

Only the translation is displayed; CLI diagnostics are kept separate. Each
request uses an ephemeral session in a temporary directory with a read-only
sandbox, without loading the user Codex configuration. Your existing OCR
engine can recognize an image and pass its text to the Codex translation provider.

日本語: Codex CLI をインストールして `codex login` を実行した後、
「設定 → 翻訳」で「Codex」を選択してください。モデルと思考レベルは
コンボボックスから選択できます。モデル名は直接入力もできます。
実行ファイル、タイムアウト、口調や用語などの追加指示も設定できます。

### Managed local translation models (Windows)

In **Settings → Translation**, select **Local translation models**. Choose
PLaMo-2-translate, Index-Translate 2B, Hy-MT2 7B, Seed-X-PPO 7B or CAT-Translate-3.3B there, or use
the model selector in the main window. Models load on the first translation;
switching models unloads the previous one. English and Japanese are supported,
including local automatic source-language detection and text recognized by OCR.
For automatic destination switching, set the first and second translation
languages to English and Japanese in the same settings page.

The application starts its own loopback-only llama.cpp server. Installation
requires Python 3.14, Visual Studio C++ tools, PowerShell 7 and approximately
80 GB of temporary disk space for PLaMo conversion. Run these steps from the
repository root; models and runtime binaries stay under the ignored
`third_party/local-translation` directory:

```powershell
python tools/setup_local_models.py --step dependencies
python tools/setup_local_models.py --step runtime
& ./third_party/local-translation/venv/Scripts/python.exe tools/setup_local_models.py --step models
& ./third_party/local-translation/venv/Scripts/python.exe tools/setup_local_models.py --step convert
& ./third_party/local-translation/venv/Scripts/python.exe tools/setup_local_models.py --step manifest
```

Select this directory in Translation settings. Subsequent translations run
offline. The manifest pins upstream revisions and records model checksums.
PLaMo uses official weights converted with the upstream `Q4_0-ssm-f16` recipe
and a RoPE-corrected runtime; Index and Hy use official Q4_K_M GGUFs. Seed uses
the community Q4_K_M conversion by mradermacher with its official tokenizer;
prompt token IDs bypass incompatible GGUF tokenization. Original model licenses apply.

```sh
crow --tp local-models --local-model index-translate-2b -s auto -t ja "Hello, world!"
crow --tp local-models --local-model plamo-2-translate -s en -t ja "Hello, world!"
crow --tp local-models --local-model hy-mt2-7b -s ja -t en "こんにちは。"
crow --tp local-models --local-model seed-x-ppo-7b -s en -t ja "Hello, world!"
```

日本語: 「設定 → 翻訳」で「ローカル翻訳モデル」を選びます。
モデル名は設定画面とメイン画面から切り替えられます。初回翻訳時に読み込み、
原文の英語・日本語はこのPC内で判定します。モデル取得後はオフラインで使えます。

### General command-line usage

The program also has a console interface.

**Usage:** `crow [options] text`

| Option                     | Description                                                                                                          |
| -------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| `-h, --help`               | Display help                                                                                                         |
| `-v, --version`            | Display version information                                                                                          |
| `-c, --codes`              | Display language codes                                                                                               |
| `-s, --source <code>`      | Specify the source language (by default, engine will try to determine the language on its own)                       |
| `-t, --translation <code>` | Specify the translation language(s), split by '+' (by default, the system language is used)                          |
| `-e, --engine <engine>`    | Specify the translator engine ('google', 'yandex', 'bing', 'libretranslate' or 'lingva'), Google is used by default  |
| `-u, --url <URL>`          | Specify Mozhi instance URL. Instance URL from app settings will be used by default                                   |
| `--tp, --translation-provider <provider>` | Specify translation provider ('copy', 'mozhi', 'localai', 'codex', 'claude-code' or 'local-models'). Provider from app settings will be used by default |
| `--local-model <id>`       | Select a managed local translation model by its preset ID                                                            |
| `--tts, --tts-provider <provider>` | Specify TTS provider ('none', 'mozhi', 'qt', or 'piper'). Provider from app settings will be used by default |
| `-r, --speak-translation`  | Speak the translation                                                                                                |
| `-o, --speak-source`       | Speak the source                                                                                                     |
| `-f, --file`               | Read source text from files. Arguments will be interpreted as file paths                                             |
| `-i, --stdin`              | Add stdin data to source text                                                                                        |
| `-a, --audio-only`         | Do not print any text when using `--speak-source` or `--speak-translation`                                           |
| `-b, --brief`              | Print only translations                                                                                              |
| `-j, --json`               | Print output formatted as JSON                                                                                       |

**Note:** If you do not pass startup arguments to the program, the GUI starts.

## D-Bus API

    org.kde.CrowTranslate
    ├── /org/kde/CrowTranslate/MainWindow
    |   |   # Global shortcuts
    |   ├── method void org.kde.CrowTranslate.MainWindow.translateSelection();
    |   ├── method void org.kde.CrowTranslate.MainWindow.speakSelection();
    |   ├── method void org.kde.CrowTranslate.MainWindow.speakTranslatedSelection();
    |   ├── method void org.kde.CrowTranslate.MainWindow.playPauseSpeaking();
    |   ├── method void org.kde.CrowTranslate.MainWindow.stopSpeaking();
    |   ├── method void org.kde.CrowTranslate.MainWindow.open();
    |   ├── method void org.kde.CrowTranslate.MainWindow.copyTranslatedSelection();
    |   ├── method void org.kde.CrowTranslate.MainWindow.recognizeScreenArea();
    |   ├── method void org.kde.CrowTranslate.MainWindow.translateScreenArea();
    |   ├── method void org.kde.CrowTranslate.MainWindow.delayedRecognizeScreenArea();
    |   ├── method void org.kde.CrowTranslate.MainWindow.delayedTranslateScreenArea();
    |   ├── method void org.kde.CrowTranslate.MainWindow.toggleOcrNegate();
    |   |   # Application control
    |   ├── method void org.kde.CrowTranslate.MainWindow.openSettings();
    |   └── method void org.kde.CrowTranslate.MainWindow.quit();
    └── /org/kde/CrowTranslate/Ocr
        ├── method void org.kde.CrowTranslate.Ocr.applyParameters(QVariantMap parameters);
        └── method void org.kde.CrowTranslate.Ocr.applyParameters(QVariantMap parameters, bool saveSettings);

For example, you can show main window using `dbus-send`:

```bash
dbus-send --type=method_call --dest=org.kde.CrowTranslate /org/kde/CrowTranslate/MainWindow org.kde.CrowTranslate.MainWindow.open
```

Or via `qdbus`:

```bash
qdbus org.kde.CrowTranslate /org/kde/CrowTranslate/MainWindow org.kde.CrowTranslate.MainWindow.open
# or shorter
qdbus org.kde.CrowTranslate /org/kde/CrowTranslate/MainWindow open
```

## Global shortcuts in wayland

Wayland doesn't provide API for global shortcuts and you need to register them by yourself. 

### KDE

KDE have a convenient feature to define shortcuts in `.desktop` file and import them in settings. We already defined these shortcuts in the `.desktop` file, you just need to activate it in settings.

Open Settings -> Keyboard -> Shortcuts -> Add new -> Application, select "Crow Translate" and press Ok.

If you migrating from 2.x, you will need to remove older entry from the settings first. It's because the application `.desktop` file changed.

This method will also work for X11.

### GNOME

For GNOME you need to manually set D-Bus commands as global shortcuts. For example, to translate selected text use the following:

```bash
qdbus org.kde.CrowTranslate /org/kde/CrowTranslate/MainWindow translateSelection
```

You can set a hotkey for this command in GNOME system settings.

## OCR

### Indra OCR

Install the official [Indra OCR](https://github.com/datalab-to/indra) CLI with
Python 3.10 or later: `python -m pip install indra-ocr`. In **Settings → OCR**,
select **Indra OCR (local)**. If `indra-ocr` is not on PATH, browse to its
executable (`indra-ocr.exe` in your Python environment's `Scripts` directory on
Windows). A dedicated environment at `indra-venv` beside Crow's executable is
detected automatically. The timeout defaults to 300 seconds.

Images and screen captures are recognized locally, and the resulting text uses
your selected translation provider. Languages are detected automatically;
Tesseract language files are not required. The first recognition downloads
about 76 MB of models from Hugging Face and caches them. You can select an
existing model directory to run offline. Model weights have their own
[license](https://huggingface.co/datalab-to/indra-ocr/blob/main/LICENSE).

日本語: 「設定 → OCR」で「Indra OCR (local)」を選択してください。
実行ファイルの場所とタイムアウトを設定できます。初回はモデルを自動取得し、
以後はローカルで認識します。言語は自動判定されます。

### Tesseract

The application can translate text from the screen using [Tesseract](https://github.com/tesseract-ocr/tesseract). To recognize text, you need to additionally install trained models and select at least one model in the settings. Choose only the models for the languages you need. The more models there are, the worse the performance and quality of OCR.

**OCR Enhancement Options:**
- **Image Inversion**: Toggle image inversion before OCR processing for improved accuracy on dark backgrounds or low-contrast text (configurable in settings)

You can manually download models from the [tessdata_fast](https://github.com/tesseract-ocr/tessdata_fast) repository or install the `org.kde.CrowTranslate.tessdata` extension if you use the app from [Flatpak](https://flatpak.org).

## Dependencies

### Required

- [CMake](https://cmake.org) 3.16+
- [Extra CMake Modules](https://github.com/KDE/extra-cmake-modules)
- [Qt6](https://www.qt.io) 6.8+ with Widgets, Network, Multimedia, Concurrent, TextToSpeech, DBus (Linux) modules
- [Tesseract](https://tesseract-ocr.github.io) 4.0+
- [Png2Ico](https://www.winterdrache.de/freeware/png2ico) or [IcoTool](https://www.nongnu.org/icoutils) for generating executable icon (Windows)

### Optional

- [ONNX Runtime](https://onnxruntime.ai/) 1.22+ (for Piper neural TTS)
- [espeak-ng](https://github.com/espeak-ng/espeak-ng) (bundled as submodule for Piper phonemization)
- [KWayland](https://invent.kde.org/frameworks/kwayland)

### External libraries

This project uses the following external libraries, which included as git submodules:

- [QHotkey](https://github.com/Skycoder42/QHotkey) - provides global shortcuts for desktop platforms.
- [SingleApplication](https://github.com/itay-grudev/SingleApplication) - prevents launch of multiple application instances.

## Icons

[Breeze](https://invent.kde.org/frameworks/breeze-icons) icon theme is bundled to provide icons on Windows and fallback icons on Linux.

[circle-flags](https://github.com/HatScripts/circle-flags "A collection of 300+ minimal circular SVG country flags") icons are used for flags.

## Installation

Windows x64 installers for this unofficial fork are available on the
[GitHub Releases page](https://github.com/ringo7pie/crow-translate/releases/latest).
Download the `crow-translate-*-setup.exe` asset. The installer includes Qt,
Tesseract, English/Japanese OCR data, Visual C++ runtime DLLs and license notices.
The installer asks whether to open the optional setup for Indra OCR and each local
translation model. Indra and CAT-Translate-3.3B start selected; license consent starts unchecked.
CAT is recommended and is the default for new installations; updates keep the current model.
Choose a default translation model from selected or installed models, or keep
the existing default. Settings apply on the next application start after the
model has been verified. Review the displayed
capacity, full licenses and eligibility conditions before agreeing to install.
Python is provisioned privately; PLaMo is downloaded from PFN and converted on
the recipient's PC (80 GB free space required). Verified conversion inputs and
download archives are removed after success; interrupted downloads are kept for
retry. Runtime files, licenses and installation records remain. Select No to skip all model and
Python downloads. The Start menu can reopen the optional setup later. Codex CLI
remains a separate installation. See `README-Windows.txt` for details. Matching
application and dependency source archives and SHA-256 checksums accompany each
installer. These installers are unsigned.

Downloads are available on the [KDE downloads](https://download.kde.org/stable/crow-translate) page. Also check out the [webpage](https://apps.kde.org/crowtranslate) for other installation methods.

**Note:** On Linux to make the application look native on a non-KDE desktop environment, you need to configure Qt applications styling. This can be done by using [qt6ct](https://github.com/trialuser02/qt6ct) or [adwaita-qt6](https://github.com/FedoraQt/adwaita-qt). Please check the appropriate installation guide for your distribution.

**Note:** Windows requires [Microsoft Visual C++ Redistributable 2019](https://support.microsoft.com/en-us/topic/the-latest-supported-visual-c-downloads-2647da03-1eea-4433-9aff-95f26a218cc0) to work.

## Building

### Building executable

You can build **Crow Translate** by using the following commands:

```bash
# Initialize git submodules (required for bundled libraries)
git submodule update --init --recursive

# Configure and build
mkdir build
cd build
cmake .. # Or `cmake -D CMAKE_BUILD_TYPE=Release ..` for single-configuration generators such as Ninja or GNU Make
cmake --build . # Or `cmake --build . --config Release` for multi-config generators such as Visual Studio Generators or Xcode
```

You will then get a binary named `crow`.

**Note:** The `--recursive` flag initializes espeak-ng and other submodules needed for compilation.

### Building a package using CPack

CMake can create [specified package types](https://cmake.org/cmake/help/latest/manual/cpack-generators.7.html) automatically.

If you use Makefile, Ninja, or Xcode generator you can use [package](https://cmake.org/cmake/help/latest/module/CPack.html#targets-package-and-package-source) target:

```bash
git submodule update --init --recursive
mkdir build
cd build
cmake -D CMAKE_BUILD_TYPE=Release -D CPACK_GENERATOR=DEB .. # You can specify several types of packages separated by semicolons in double quotes, for example: `CPACK_GENERATOR="DEB;ZIP;NSIS"`
cmake --build . --target package
```

Or you can use [CPack](https://cmake.org/cmake/help/latest/module/CPack.html) utility for any generators:

```bash
git submodule update --init --recursive
mkdir build
cd build
cmake .. # Or `cmake -D CMAKE_BUILD_TYPE=Release ..` for single-configuration generators such as Ninja or GNU Make
cpack -G DEB # Or `cpack -G DEB -C Release` for multi-config generators such as Visual Studio Generators or Xcode
```

You can also compile it using [Craft](https://community.kde.org/Craft):

```bash
craft crow-translate
```

### Build parameters

- `WITH_PORTABLE_MODE` - Enable portable functionality. If you create file named `settings.ini` in the app folder and Crow will store the configuration in it. It also adds the “Portable Mode” option to the application settings, which does the same.
- `WITH_KWAYLAND` - Find and use KWayland library for better Wayland integration.
- `WITH_TTS` - Build the text-to-speech engines and their UI (default ON). Turning it off removes the speech providers, the play/pause/stop buttons and the voice selection.
- `WITH_PIPER_TTS` - Enable Piper neural TTS provider (requires ONNX Runtime and espeak-ng, and `WITH_TTS`).
- `ESPEAKNG_USE_SYSTEM` - Link against a system espeak-ng instead of building the bundled submodule. Useful for distribution packaging. Crow requires `espeak_TextToPhonemesWithTerminator`, an API that is not part of the espeak-ng 1.52.0 release, so the system espeak-ng must be built from git (see [espeak-ng!2127](https://github.com/espeak-ng/espeak-ng/pull/2127)); the build verifies this at configure time and fails otherwise. Linux only.

Build parameters are passed at configuration stage: `cmake -D WITH_PORTABLE_MODE=ON -D WITH_PIPER_TTS=ON ..`.

**Note for Piper TTS**: When enabled with `-D WITH_PIPER_TTS=ON`, the Piper neural TTS provider requires downloading voice models separately. Voice models can be downloaded from [Hugging Face Piper voices](https://huggingface.co/rhasspy/piper-voices) and should be placed in the application's data directory. ONNX Runtime and espeak-ng submodule are automatically configured when this option is enabled.
