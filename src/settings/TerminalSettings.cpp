// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "TerminalSettings.h"
#include "SettingsConstraints_p.h"
#include "TerminalColors.h"
#include "TitleFormat_p.h"

#include <KLocalizedString>
#include <KSharedConfig>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QGuiApplication>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Kwi
{

TerminalSettings::TerminalSettings(QObject *parent)
    : TerminalPreferences{KSharedConfig::openConfig(QStringLiteral("kwiterminalrc"), KConfig::NoGlobals), parent}
{
    refreshFixedWidthFonts();
    qGuiApp->installEventFilter(this);
    connect(qGuiApp, &QGuiApplication::fontDatabaseChanged, this, &TerminalSettings::refreshFixedWidthFonts);

    const auto changeSignals = {&TerminalPreferences::useSystemFontChanged,       &TerminalPreferences::fontFamilyChanged,
                                &TerminalPreferences::fontPointSizeChanged,       &TerminalPreferences::colorSchemeChanged,
                                &TerminalPreferences::backgroundOpacityChanged,   &TerminalPreferences::backgroundBlurChanged,
                                &TerminalPreferences::tabBarAtBottomChanged,      &TerminalPreferences::showTabStatusIconsChanged,
                                &TerminalPreferences::useCustomWindowSizeChanged, &TerminalPreferences::windowColumnsChanged,
                                &TerminalPreferences::windowRowsChanged,          &TerminalPreferences::scrollbackLinesChanged,
                                &TerminalPreferences::silenceSecondsChanged,      &TerminalPreferences::titleFormatChanged,
                                &TerminalPreferences::remoteTitleFormatChanged,   &TerminalPreferences::windowTitleFromApplicationChanged,
                                &TerminalPreferences::cursorStyleChanged,         &TerminalPreferences::cursorBlinkingChanged,
                                &TerminalPreferences::confirmCloseRunningChanged, &TerminalPreferences::confirmMultilinePasteChanged,
                                &TerminalPreferences::copyOnSelectChanged,        &TerminalPreferences::requireCtrlForLinksChanged,
                                &TerminalPreferences::syntaxHighlightingChanged,  &TerminalPreferences::terminalPaddingChanged};

    for (const auto signal : changeSignals) {
        // Every entry is a typed signal; Clazy cannot resolve the loop variable.
        connect(this, signal, this, &TerminalSettings::defaultsStateChanged); // clazy:exclude=connect-non-signal
    }

    connect(this, &TerminalPreferences::useSystemFontChanged, this, &TerminalPreferences::fontFamilyChanged);
    connect(this, &TerminalPreferences::useSystemFontChanged, this, &TerminalPreferences::fontPointSizeChanged);
    connect(this, &TerminalSettings::fixedWidthFontsChanged, this, &TerminalPreferences::fontFamilyChanged);

    read();
    m_watcher = KConfigWatcher::create(sharedConfig());
    connect(m_watcher.data(), &KConfigWatcher::configChanged, this, [this] {
        read();
        Q_EMIT defaultsStateChanged();
    });
}

void TerminalSettings::usrRead()
{
    if (!std::isfinite(TerminalPreferences::fontPointSize())) {
        TerminalPreferences::setFontPointSize(defaultFontPointSizeValue());
    }

    if (!std::isfinite(TerminalPreferences::backgroundOpacity())) {
        TerminalPreferences::setBackgroundOpacity(defaultBackgroundOpacityValue());
    }

    if (!titleFormatError(TerminalPreferences::titleFormat()).isEmpty()) {
        TerminalPreferences::setTitleFormat(defaultTitleFormatValue());
    }

    if (!titleFormatError(TerminalPreferences::remoteTitleFormat()).isEmpty()) {
        TerminalPreferences::setRemoteTitleFormat(defaultRemoteTitleFormatValue());
    }

    if (!SettingsConstraints::isValidCursorStyle(TerminalPreferences::cursorStyle())) {
        TerminalPreferences::setCursorStyle(defaultCursorStyleValue());
    }

    if (TerminalPreferences::scrollbackLines() < -1) {
        TerminalPreferences::setScrollbackLines(0);
    }
}

QString TerminalSettings::fontFamily() const
{
    const QString family{TerminalPreferences::fontFamily()};

    return useSystemFont() || !m_fixedWidthFonts.contains(family) ? QFontDatabase::systemFont(QFontDatabase::FixedFont).family() : family;
}

qreal TerminalSettings::fontPointSize() const
{
    const qreal size{TerminalPreferences::fontPointSize()};

    if (!useSystemFont() && std::isfinite(size) && size != 0) {
        return std::clamp(size, SettingsConstraints::MinimumFontPointSize, SettingsConstraints::MaximumFontPointSize);
    }

    const QFont systemFont{QFontDatabase::systemFont(QFontDatabase::FixedFont)};

    return systemFont.pointSizeF() > 0 ? systemFont.pointSizeF() : QFontInfo(systemFont).pointSizeF();
}

qreal TerminalSettings::backgroundOpacity() const
{
    const qreal value{TerminalPreferences::backgroundOpacity()};

    return std::isfinite(value) ? value : defaultBackgroundOpacityValue();
}

int TerminalSettings::scrollbackLines() const
{
    const int value{TerminalPreferences::scrollbackLines()};

    return value < -1 ? 0 : value;
}

QString TerminalSettings::titleFormat() const
{
    const QString format{TerminalPreferences::titleFormat()};

    return titleFormatError(format).isEmpty() ? format : defaultTitleFormatValue();
}

QString TerminalSettings::remoteTitleFormat() const
{
    const QString format{TerminalPreferences::remoteTitleFormat()};

    return titleFormatError(format).isEmpty() ? format : defaultRemoteTitleFormatValue();
}

QString TerminalSettings::cursorStyle() const
{
    const QString style{TerminalPreferences::cursorStyle()};

    return SettingsConstraints::isValidCursorStyle(style) ? style : defaultCursorStyleValue();
}

QString TerminalSettings::titleFormatError(const QString &value) const
{
    return formatTitle(value).error;
}

QString TerminalSettings::previewTitleFormat(const QString &value, bool remote) const
{
    const auto result = formatTitle(value,
                                    {i18nc("@info title format preview", "Shell"),
                                     QStringLiteral("/home/user/project"),
                                     QStringLiteral("user"),
                                     remote ? QStringLiteral("server.example.org") : QStringLiteral("host"),
                                     remote ? QStringLiteral("top") : QStringLiteral("bash"),
                                     QStringLiteral("/home/user")});

    return result.error.isEmpty() ? result.text : QString{};
}

void TerminalSettings::setTitleFormat(const QString &value)
{
    if (titleFormatError(value).isEmpty()) {
        TerminalPreferences::setTitleFormat(value);
    }
}

void TerminalSettings::setRemoteTitleFormat(const QString &value)
{
    if (titleFormatError(value).isEmpty()) {
        TerminalPreferences::setRemoteTitleFormat(value);
    }
}

QStringList TerminalSettings::fixedWidthFonts() const
{
    return m_fixedWidthFonts;
}

void TerminalSettings::setUseSystemFont(bool value)
{
    if (value == useSystemFont() || isUseSystemFontImmutable()) {
        return;
    }

    if (!value) {
        // Save the system font on first opt-out, then preserve the user's font across toggles.
        if (!m_fixedWidthFonts.contains(TerminalPreferences::fontFamily())) {
            TerminalPreferences::setFontFamily(fontFamily());
        }

        const qreal size{TerminalPreferences::fontPointSize()};

        if (!std::isfinite(size) || size == 0) {
            TerminalPreferences::setFontPointSize(
                std::clamp(fontPointSize(), SettingsConstraints::MinimumFontPointSize, SettingsConstraints::MaximumFontPointSize));
        }
    }

    TerminalPreferences::setUseSystemFont(value);
}

void TerminalSettings::setFontFamily(const QString &value)
{
    if (!m_fixedWidthFonts.contains(value) || isFontFamilyImmutable() || (useSystemFont() && isUseSystemFontImmutable())) {
        return;
    }

    TerminalPreferences::setFontFamily(value);
    setUseSystemFont(false);
}

void TerminalSettings::setFontPointSize(qreal value)
{
    if (!std::isfinite(value) || isFontPointSizeImmutable() || (useSystemFont() && isUseSystemFontImmutable())) {
        return;
    }

    TerminalPreferences::setFontPointSize(std::clamp(value, SettingsConstraints::MinimumFontPointSize, SettingsConstraints::MaximumFontPointSize));
    setUseSystemFont(false);
}

void TerminalSettings::refreshFixedWidthFonts()
{
    QStringList fixedFonts{};
    const auto families = QFontDatabase::families();

    for (const QString &family : families) {
        if (QFontDatabase::isFixedPitch(family)) {
            fixedFonts.append(family);
        }
    }

    const QString systemFamily{QFontDatabase::systemFont(QFontDatabase::FixedFont).family()};

    if (!fixedFonts.contains(systemFamily)) {
        fixedFonts.prepend(systemFamily);
    }

    if (fixedFonts != m_fixedWidthFonts) {
        m_fixedWidthFonts = std::move(fixedFonts);

        Q_EMIT fixedWidthFontsChanged();
    }
}

bool TerminalSettings::eventFilter(QObject *object, QEvent *event)
{
    if (object == qGuiApp && (event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::ThemeChange)) {
        refreshFixedWidthFonts();

        Q_EMIT fontFamilyChanged();
        Q_EMIT fontPointSizeChanged();
    }

    return TerminalPreferences::eventFilter(object, event);
}

void TerminalSettings::setColorScheme(const QString &value)
{
    if (isValidTerminalPalette(value)) {
        TerminalPreferences::setColorScheme(value);
    }
}

void TerminalSettings::setScrollbackLines(int value)
{
    TerminalPreferences::setScrollbackLines(value < -1 ? 0 : value);
}

void TerminalSettings::setBackgroundOpacity(qreal value)
{
    if (std::isfinite(value)) {
        TerminalPreferences::setBackgroundOpacity(value);
    }
}

void TerminalSettings::setCursorStyle(const QString &value)
{
    if (SettingsConstraints::isValidCursorStyle(value)) {
        TerminalPreferences::setCursorStyle(value);
    }
}

void TerminalSettings::resetAppearanceDefaults()
{
    resetGroupDefaults(QStringLiteral("Appearance"));
}

void TerminalSettings::resetTerminalDefaults()
{
    resetGroupDefaults(QStringLiteral("Terminal"));
}

void TerminalSettings::resetBehaviorDefaults()
{
    resetGroupDefaults(QStringLiteral("Behavior"));
}

bool TerminalSettings::canResetAppearance() const
{
    return canResetGroup(QStringLiteral("Appearance"));
}

bool TerminalSettings::canResetTerminal() const
{
    return canResetGroup(QStringLiteral("Terminal"));
}

bool TerminalSettings::canResetBehavior() const
{
    return canResetGroup(QStringLiteral("Behavior"));
}

bool TerminalSettings::canResetGroup(const QString &name) const
{
    const auto preferences = items();

    return std::any_of(preferences.cbegin(), preferences.cend(), [&name](const KConfigSkeletonItem *item) {
        return item->group() == name && !item->isImmutable() && !item->isDefault();
    });
}

void TerminalSettings::resetGroupDefaults(const QString &name)
{
    const auto preferences = items();

    for (KConfigSkeletonItem *item : preferences) {
        if (item->group() == name && !item->isImmutable()) {
            item->setDefault();
        }
    }
}

}
