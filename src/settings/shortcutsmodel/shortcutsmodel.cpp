/*
 * SPDX-FileCopyrightText: 2018 Hennadii Chernyshchyk <genaloner@gmail.com>
 * SPDX-FileCopyrightText: 2022 Volk Milit <javirrdar@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "shortcutsmodel.h"

#include "shortcutitem.h"
#include "settings/appsettings.h"

#include <QFont>
#include <QMetaEnum>

ShortcutsModel::ShortcutsModel(QObject *parent)
    : QAbstractItemModel(parent)
    , m_rootItem(new ShortcutItem(this))
{
    // Global shortcuts
    m_globalShortcuts = new ShortcutItem(tr("Global"), m_rootItem);

    m_translateSelectionShortcut = new ShortcutItem(tr("Translate selected text"), QStringLiteral("go-next"), m_globalShortcuts);
    m_translateSelectionShortcut->setDefaultShortcut(AppSettings::defaultTranslateSelectionShortcut());

    m_speakSelectionShortcut = new ShortcutItem(tr("Speak selected text"), QStringLiteral("media-playback-start"), m_globalShortcuts);
    m_speakSelectionShortcut->setDefaultShortcut(AppSettings::defaultSpeakSelectionShortcut());

    m_speakTranslatedSelectionShortcut = new ShortcutItem(tr("Speak translation of selected text"), QStringLiteral("media-playback-start"), m_globalShortcuts);
    m_speakTranslatedSelectionShortcut->setDefaultShortcut(AppSettings::defaultSpeakTranslatedSelectionShortcut());

    m_stopSpeakingShortcut = new ShortcutItem(tr("Stop text speaking"), QStringLiteral("media-playback-stop"), m_globalShortcuts);
    m_stopSpeakingShortcut->setDefaultShortcut(AppSettings::defaultStopSpeakingShortcut());

    m_playPauseSpeakingShortcut = new ShortcutItem(tr("Speak / pause text speaking"), QStringLiteral("media-playback-pause"), m_globalShortcuts);
    m_playPauseSpeakingShortcut->setDefaultShortcut(AppSettings::defaultPlayPauseSpeakingShortcut());

    m_showMainWindowShortcut = new ShortcutItem(tr("Show main window"), QStringLiteral("window"), m_globalShortcuts);
    m_showMainWindowShortcut->setDefaultShortcut(AppSettings::defaultShowMainWindowShortcut());

    m_copyTranslatedSelectionShortcut = new ShortcutItem(tr("Translate selected text and copy to clipboard"), QStringLiteral("edit-copy"), m_globalShortcuts);
    m_copyTranslatedSelectionShortcut->setDefaultShortcut(AppSettings::defaultCopyTranslatedSelectionShortcut());

    m_recognizeScreenAreaShortcut = new ShortcutItem(tr("Recognize text in screen area"), QStringLiteral("transform-crop"), m_globalShortcuts);
    m_recognizeScreenAreaShortcut->setDefaultShortcut(AppSettings::defaultRecognizeScreenAreaShortcut());

    m_translateScreenAreaShortcut = new ShortcutItem(tr("Translate text in screen area"), QStringLiteral("transform-crop"), m_globalShortcuts);
    m_translateScreenAreaShortcut->setDefaultShortcut(AppSettings::defaultTranslateScreenAreaShortcut());

    m_delayedRecognizeScreenAreaShortcut = new ShortcutItem(tr("Recognize text in screen area with delay"), QStringLiteral("transform-crop"), m_globalShortcuts);
    m_delayedRecognizeScreenAreaShortcut->setDefaultShortcut(AppSettings::defaultDelayedRecognizeScreenAreaShortcut());

    m_delayedTranslateScreenAreaShortcut = new ShortcutItem(tr("Translate text in screen area with delay"), QStringLiteral("transform-crop"), m_globalShortcuts);
    m_delayedTranslateScreenAreaShortcut->setDefaultShortcut(AppSettings::defaultDelayedTranslateScreenAreaShortcut());

    m_toggleOcrNegateShortcut = new ShortcutItem(tr("Toggle color inversion for OCR"), QStringLiteral("transform-crop"), m_globalShortcuts);

    // Window shortcuts
    auto *windowShortcuts = new ShortcutItem(tr("Main window"), m_rootItem);

    m_translateShortcut = new ShortcutItem(tr("Translate"), QStringLiteral("go-next"), windowShortcuts);
    m_translateShortcut->setDefaultShortcut(AppSettings::defaultTranslateShortcut());

    m_swapShortcut = new ShortcutItem(tr("Swap languages"), QStringLiteral("object-flip-horizontal"), windowShortcuts);
    m_swapShortcut->setDefaultShortcut(AppSettings::defaultSwapShortcut());

    m_closeWindowShortcut = new ShortcutItem(tr("Close window"), QStringLiteral("application-exit"), windowShortcuts);
    m_closeWindowShortcut->setDefaultShortcut(AppSettings::defaultCloseWindowShortcut());

    const QStringList descriptions{tr("Cancel translation"), tr("Paste clipboard into input"), tr("Previous installed local model"),
        tr("Next installed local model"), tr("Open settings"), tr("Edit current model instructions"), tr("Manage local AI")};
    const auto ids = AppSettings::extraShortcutIds();
    for (int i = 0; i < ids.size(); ++i) {
        auto *item = new ShortcutItem(descriptions.at(i), QStringLiteral("preferences-other"), windowShortcuts);
        item->setDefaultShortcut(AppSettings::defaultExtraShortcut(ids.at(i)));
        m_extraShortcuts.insert(ids.at(i), item);
    }

    // Source text shortcuts
    auto *sourceText = new ShortcutItem(tr("Source text"), windowShortcuts);

    m_speakSourceShortcut = new ShortcutItem(tr("Speak / pause text speaking"), QStringLiteral("media-playback-start"), sourceText);
    m_speakSourceShortcut->setDefaultShortcut(AppSettings::defaultSpeakSourceShortcut());

    // Translation text shortcuts
    auto *translationText = new ShortcutItem(tr("Translation"), windowShortcuts);

    m_speakTranslationShortcut = new ShortcutItem(tr("Speak / pause speaking"), QStringLiteral("media-playback-start"), translationText);
    m_speakTranslationShortcut->setDefaultShortcut(AppSettings::defaultSpeakTranslationShortcut());

    m_copyTranslationShortcut = new ShortcutItem(tr("Copy to clipboard"), QStringLiteral("edit-copy"), translationText);
    m_copyTranslationShortcut->setDefaultShortcut(AppSettings::defaultCopyTranslationShortcut());
}

ShortcutsModel::~ShortcutsModel()
{
    delete m_rootItem;
}

QVariant ShortcutsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};

    const auto *shortcut = static_cast<ShortcutItem *>(index.internalPointer());

    switch (role) {
    case Qt::ToolTipRole:
    case Qt::DisplayRole:
        switch (index.column()) {
        case DescriptionColumn:
            return shortcut->description();
        case ShortcutColumn:
            return shortcut->shortcut();
        }
        break;
    case Qt::DecorationRole:
        if (index.column() == DescriptionColumn)
            return shortcut->icon();
        break;
    case Qt::FontRole:
        if (shortcut->childCount() > 0) {
            QFont font;
            font.setBold(true);
            return font;
        }
    }

    return {};
}

QVariant ShortcutsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case DescriptionColumn:
            return tr("Description");
        case ShortcutColumn:
            return tr("Shortcut");
        }
    }

    return {};
}

QModelIndex ShortcutsModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return {};

    ShortcutItem *parentItem = parent.isValid() ? static_cast<ShortcutItem *>(parent.internalPointer()) : m_rootItem;

    ShortcutItem *childItem = parentItem->child(row);
    if (childItem == nullptr)
        return {};

    return createIndex(row, column, childItem);
}

QModelIndex ShortcutsModel::parent(const QModelIndex &index) const
{
    if (!index.isValid())
        return {};

    auto *item = static_cast<ShortcutItem *>(index.internalPointer());
    ShortcutItem *parentItem = item->parentItem();

    if (parentItem == m_rootItem)
        return {};

    return createIndex(parentItem->row(), 0, parentItem);
}

int ShortcutsModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return m_rootItem->childCount();

    return static_cast<ShortcutItem *>(parent.internalPointer())->childCount();
}

int ShortcutsModel::columnCount(const QModelIndex & /*parent*/) const
{
    return QMetaEnum::fromType<Column>().keyCount();
}

Qt::ItemFlags ShortcutsModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    Qt::ItemFlags itemFlags = QAbstractItemModel::flags(index);
    const auto *item = static_cast<const ShortcutItem *>(index.internalPointer());
    itemFlags.setFlag(Qt::ItemIsEnabled, item->isEnabled());

    return itemFlags;
}

void ShortcutsModel::loadShortcuts(const AppSettings &settings)
{
    // Global shortcuts
    m_translateSelectionShortcut->setShortcut(settings.translateSelectionShortcut());
    m_speakSelectionShortcut->setShortcut(settings.speakSelectionShortcut());
    m_speakTranslatedSelectionShortcut->setShortcut(settings.speakTranslatedSelectionShortcut());
    m_stopSpeakingShortcut->setShortcut(settings.stopSpeakingShortcut());
    m_playPauseSpeakingShortcut->setShortcut(settings.playPauseSpeakingShortcut());
    m_showMainWindowShortcut->setShortcut(settings.showMainWindowShortcut());
    m_copyTranslatedSelectionShortcut->setShortcut(settings.copyTranslatedSelectionShortcut());
    m_recognizeScreenAreaShortcut->setShortcut(settings.recognizeScreenAreaShortcut());
    m_translateScreenAreaShortcut->setShortcut(settings.translateScreenAreaShortcut());
    m_delayedRecognizeScreenAreaShortcut->setShortcut(settings.delayedRecognizeScreenAreaShortcut());
    m_delayedTranslateScreenAreaShortcut->setShortcut(settings.delayedTranslateScreenAreaShortcut());
    m_toggleOcrNegateShortcut->setShortcut(settings.toggleOcrNegateShortcut());

    // Window shortcuts
    m_translateShortcut->setShortcut(settings.translateShortcut());
    m_swapShortcut->setShortcut(settings.swapShortcut());
    m_closeWindowShortcut->setShortcut(settings.closeWindowShortcut());

    // Source text shortcuts
    m_speakSourceShortcut->setShortcut(settings.speakSourceShortcut());

    // Translation text shortcuts
    m_speakTranslationShortcut->setShortcut(settings.speakTranslationShortcut());
    m_copyTranslationShortcut->setShortcut(settings.copyTranslationShortcut());
    for (auto it = m_extraShortcuts.cbegin(); it != m_extraShortcuts.cend(); ++it)
        it.value()->setShortcut(settings.extraShortcut(it.key()));
}

void ShortcutsModel::saveShortcuts(AppSettings &settings) const
{
    // Global shortcuts
    settings.setTranslateSelectionShortcut(m_translateSelectionShortcut->shortcut());
    settings.setSpeakSelectionShortcut(m_speakSelectionShortcut->shortcut());
    settings.setSpeakTranslatedSelectionShortcut(m_speakTranslatedSelectionShortcut->shortcut());
    settings.setStopSpeakingShortcut(m_stopSpeakingShortcut->shortcut());
    settings.setPlayPauseSpeakingShortcut(m_playPauseSpeakingShortcut->shortcut());
    settings.setShowMainWindowShortcut(m_showMainWindowShortcut->shortcut());
    settings.setCopyTranslatedSelectionShortcut(m_copyTranslatedSelectionShortcut->shortcut());
    settings.setRecognizeScreenAreaShortcut(m_recognizeScreenAreaShortcut->shortcut());
    settings.setTranslateScreenAreaShortcut(m_translateScreenAreaShortcut->shortcut());
    settings.setDelayedRecognizeScreenAreaShortcut(m_delayedRecognizeScreenAreaShortcut->shortcut());
    settings.setDelayedTranslateScreenAreaShortcut(m_delayedTranslateScreenAreaShortcut->shortcut());
    settings.setToggleOcrNegateShortcut(m_toggleOcrNegateShortcut->shortcut());

    // Window shortcuts
    settings.setTranslateShortcut(m_translateShortcut->shortcut());
    settings.setSwapShortcut(m_swapShortcut->shortcut());
    settings.setCloseWindowShortcut(m_closeWindowShortcut->shortcut());

    // Source text shortcuts
    settings.setSpeakSourceShortcut(m_speakSourceShortcut->shortcut());

    // Translation text shortcuts
    settings.setSpeakTranslationShortcut(m_speakTranslationShortcut->shortcut());
    settings.setCopyTranslationShortcut(m_copyTranslationShortcut->shortcut());
    for (auto it = m_extraShortcuts.cbegin(); it != m_extraShortcuts.cend(); ++it)
        settings.setExtraShortcut(it.key(), it.value()->shortcut());
}

void ShortcutsModel::resetAllShortcuts()
{
    beginResetModel();
    m_rootItem->resetAllShortucts();
    endResetModel();
}

QList<QKeySequence> ShortcutsModel::globalSequences() const
{
    QList<QKeySequence> result;
    for (int i = 0; i < m_globalShortcuts->childCount(); ++i)
        result << m_globalShortcuts->child(i)->shortcut();
    return result;
}

QString ShortcutsModel::conflictDescription() const
{
    QList<ShortcutItem *> leaves;
    const auto visit = [&leaves](auto &&self, ShortcutItem *item) -> void {
        if (!item->isEnabled())
            return;
        if (!item->childCount())
            leaves << item;
        for (int i = 0; i < item->childCount(); ++i)
            self(self, item->child(i));
    };
    visit(visit, m_rootItem);
    for (int i = 0; i < leaves.size(); ++i) {
        const auto first = leaves.at(i)->shortcut();
        if (first.isEmpty())
            continue;
        // A global hotkey must be one chord; QHotkey cannot register a sequence.
        if (leaves.at(i)->parentItem() == m_globalShortcuts && first.count() > 1)
            return tr("Global shortcuts must contain a single key combination: %1").arg(leaves.at(i)->description());
        for (int j = i + 1; j < leaves.size(); ++j) {
            const auto second = leaves.at(j)->shortcut();
            if (!second.isEmpty() && (first.matches(second) != QKeySequence::NoMatch || second.matches(first) != QKeySequence::NoMatch))
                return tr("%1 conflicts with %2 (%3).").arg(leaves.at(i)->description(), leaves.at(j)->description(), first.toString(QKeySequence::NativeText));
        }
    }
    return {};
}

void ShortcutsModel::setGlobalShortuctsEnabled(bool enabled)
{
    m_globalShortcuts->setEnabled(enabled);
}

void ShortcutsModel::updateShortcut(ShortcutItem *item)
{
    const QModelIndex modelIndex = index(item, ShortcutColumn);
    emit dataChanged(modelIndex, modelIndex, {Qt::DisplayRole});
}

void ShortcutsModel::updateItem(ShortcutItem *item)
{
    const QModelIndex modelIndexLeft = index(item, ShortcutColumn);
    const QModelIndex modelIndexRight = index(item, DescriptionColumn);
    emit dataChanged(modelIndexLeft, modelIndexRight, {Qt::DisplayRole});
}

QModelIndex ShortcutsModel::index(ShortcutItem *item, int column) const
{
    if (item == m_rootItem)
        return {};

    return index(item->row(), column, index(item->parentItem(), column));
}
