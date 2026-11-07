// SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.ki18n
import org.kde.kirigami as Kirigami
import org.kde.kirigami.forms as Forms
import org.kde.kwi

SettingsPage {
    id: root

    required property AppSettings appSettings
    required property ColorSchemeModel terminalPalettes
    property bool showWindowSettings: true

    objectName: "appearanceSettings"
    title: KI18n.i18nc("@title:window", "Appearance")
    defaultsVisible: true
    defaultsEnabled: root.appSettings.canResetAppearance

    onRestoreDefaultsRequested: {
        root.appSettings.resetAppearanceDefaults();
        root.appSettings.save();
    }

    header: Kirigami.InlineMessage {
        visible: text.length > 0
        text: TextFormatting.plainTextToHtml(root.terminalPalettes.errorString)
        type: Kirigami.MessageType.Error
    }

    Forms.Form {
        Forms.FormGroup {
            title: KI18n.i18nc("@title:group", "Text")

            Forms.FormEntry {
                contentItem: Controls.CheckBox {
                    objectName: "systemFontSetting"
                    Layout.fillWidth: true
                    text: KI18n.i18nc("@option:check", "Use system fixed-width font")
                    enabled: !root.appSettings.isUseSystemFontImmutable
                    checked: root.appSettings.useSystemFont

                    onToggled: {
                        root.appSettings.useSystemFont = checked;
                        root.appSettings.save();
                    }
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label:listbox", "Font:")
                enabled: !root.appSettings.useSystemFont && !root.appSettings.isFontFamilyImmutable

                contentItem: Controls.ComboBox {
                    objectName: "fontFamilySetting"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    model: root.appSettings.fixedWidthFonts
                    currentIndex: root.appSettings.fixedWidthFonts.indexOf(root.appSettings.fontFamily)

                    onActivated: index => {
                        root.appSettings.fontFamily = root.appSettings.fixedWidthFonts[index];
                        root.appSettings.save();
                    }
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label:spinbox", "Size (pt):")
                enabled: !root.appSettings.useSystemFont && !root.appSettings.isFontPointSizeImmutable

                contentItem: Controls.SpinBox {
                    id: fontSize

                    objectName: "fontSizeSetting"
                    editable: true
                    from: 60
                    to: 480
                    stepSize: 5
                    value: Math.round(root.appSettings.fontPointSize * 10)
                    textFromValue: (value, locale) => Number(value / 10).toLocaleString(locale, 'f', 1)
                    valueFromText: (text, locale) => Math.round(Number.fromLocaleString(locale, text) * 10)

                    validator: DoubleValidator {
                        bottom: 6
                        top: 48
                        decimals: 1
                        notation: DoubleValidator.StandardNotation
                        locale: fontSize.locale.name
                    }

                    onValueModified: {
                        root.appSettings.fontPointSize = value / 10;
                        root.appSettings.save();
                    }
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label:spinbox", "Padding (px):")
                enabled: !root.appSettings.isTerminalPaddingImmutable

                contentItem: Controls.SpinBox {
                    objectName: "terminalPaddingSetting"
                    editable: true
                    from: 0
                    to: 32
                    value: root.appSettings.terminalPadding

                    onValueModified: {
                        root.appSettings.terminalPadding = value;
                        root.appSettings.save();
                    }
                }
            }

            Forms.FormEntry {
                fullWidth: true

                contentItem: Controls.Label {
                    Layout.fillWidth: true
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 24
                    Layout.minimumWidth: 0
                    text: KI18n.i18nc("@info font preview. Include letters, digits and shell punctuation.", "Aa Bb 0123456789 — ~/Documents $")
                    font.family: root.appSettings.fontFamily
                    font.pointSize: root.appSettings.fontPointSize
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    Accessible.name: KI18n.i18nc("@info:accessibility", "Terminal font preview")
                }
            }
        }

        Forms.FormGroup {
            title: KI18n.i18nc("@title:group", "Colors")

            Forms.FormEntry {
                title: KI18n.i18nc("@label:listbox", "Color scheme:")
                enabled: !root.appSettings.isColorSchemeImmutable

                contentItem: RowLayout {
                    Kirigami.FormData.buddyFor: colorSchemeSelector
                    spacing: Kirigami.Units.smallSpacing

                    Controls.ComboBox {
                        id: colorSchemeSelector

                        objectName: "colorSchemeSelector"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        textRole: "name"
                        valueRole: "id"
                        model: root.terminalPalettes.entries
                        currentIndex: root.terminalPalettes.currentIndex

                        onActivated: index => {
                            root.appSettings.colorScheme = model[index].id;
                            root.appSettings.save();
                            root.terminalPalettes.clearError();
                        }
                    }

                    Controls.Button {
                        objectName: "editColorSchemeButton"
                        text: KI18n.i18nc("@action:button", "Edit Color Scheme…")
                        icon.name: "document-edit"
                        display: Controls.AbstractButton.IconOnly
                        enabled: root.terminalPalettes.canEditCurrent
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay

                        onClicked: root.editScheme(false)
                    }

                    Controls.ToolButton {
                        id: paletteMenuButton

                        objectName: "colorSchemeActionsButton"
                        text: KI18n.i18nc("@action:button", "More Color Scheme Actions")
                        icon.name: "overflow-menu"
                        display: Controls.AbstractButton.IconOnly
                        down: pressed || paletteMenu.visible
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered && !paletteMenu.visible
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay

                        onClicked: paletteMenu.open()

                        Controls.Menu {
                            id: paletteMenu

                            objectName: "colorSchemeActionsMenu"
                            y: paletteMenuButton.height

                            Controls.MenuItem {
                                text: KI18n.i18nc("@action:inmenu", "Duplicate…")
                                icon.name: "edit-copy"

                                onTriggered: root.editScheme(true)
                            }

                            Controls.MenuItem {
                                text: KI18n.i18nc("@action:inmenu", "Import…")
                                icon.name: "document-import"

                                onTriggered: paletteFile.open()
                            }

                            Controls.MenuSeparator {}

                            Controls.MenuItem {
                                text: KI18n.i18nc("@action:inmenu", "Remove")
                                icon.name: "edit-delete"
                                enabled: root.terminalPalettes.canRemoveCurrent

                                onTriggered: root.terminalPalettes.removeScheme(root.appSettings.colorScheme)
                            }
                        }
                    }
                }
            }

            Forms.FormEntry {
                subtitle: KI18n.i18nc("@info", "Uses the colors of the color scheme.")

                contentItem: Controls.CheckBox {
                    Layout.fillWidth: true
                    objectName: "syntaxHighlightingSetting"
                    text: KI18n.i18nc("@option:check", "Highlight common patterns in output")
                    enabled: !root.appSettings.isSyntaxHighlightingImmutable
                    checked: root.appSettings.syntaxHighlighting

                    onToggled: {
                        root.appSettings.syntaxHighlighting = checked;
                        root.appSettings.save();
                    }
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label:slider", "Background opacity:")

                contentItem: RowLayout {
                    Kirigami.FormData.buddyFor: opacitySlider

                    Controls.Slider {
                        id: opacitySlider

                        objectName: "backgroundOpacitySetting"
                        Layout.fillWidth: true
                        enabled: !root.appSettings.isBackgroundOpacityImmutable
                        from: 0
                        to: 100
                        stepSize: 1
                        Kirigami.StyleHints.tickMarkStepSize: 10
                        value: Math.round(root.appSettings.backgroundOpacity * 100)

                        onMoved: {
                            root.appSettings.backgroundOpacity = value / 100;
                            root.appSettings.save();
                        }
                    }

                    Controls.Label {
                        id: opacityLabel

                        Layout.preferredWidth: Math.ceil(opacityLabelMetrics.advanceWidth)
                        text: KI18n.i18nc("@label", "%1%", Math.round(opacitySlider.value))
                        horizontalAlignment: Text.AlignRight

                        TextMetrics {
                            id: opacityLabelMetrics

                            font: opacityLabel.font
                            renderType: opacityLabel.renderType
                            text: KI18n.i18nc("@label", "%1%", 999)
                        }
                    }

                    Controls.CheckBox {
                        id: blurSetting

                        objectName: "backgroundBlurSetting"
                        text: KI18n.i18nc("@option:check", "Blur")
                        enabled: !root.appSettings.isBackgroundBlurImmutable
                        checked: root.appSettings.backgroundBlur

                        onToggled: {
                            root.appSettings.backgroundBlur = checked;
                            root.appSettings.save();
                        }
                    }
                }
            }
        }

        Forms.FormGroup {
            title: KI18n.i18nc("@title:group", "Tab Bar")

            Forms.FormEntry {
                title: KI18n.i18nc("@label:listbox", "Position:")

                contentItem: Controls.ComboBox {
                    objectName: "tabBarPositionSetting"
                    enabled: !root.appSettings.isTabBarAtBottomImmutable
                    model: [KI18n.i18nc("@item:inlistbox tab bar position", "Top"), KI18n.i18nc("@item:inlistbox tab bar position", "Bottom")]
                    currentIndex: root.appSettings.tabBarAtBottom ? 1 : 0

                    onActivated: index => {
                        root.appSettings.tabBarAtBottom = index === 1;
                        root.appSettings.save();
                    }
                }
            }

            Forms.FormEntry {
                contentItem: Controls.CheckBox {
                    objectName: "showTabStatusIconsSetting"
                    Layout.fillWidth: true
                    text: KI18n.i18nc("@option:check", "Show status icons")
                    enabled: !root.appSettings.isShowTabStatusIconsImmutable
                    checked: root.appSettings.showTabStatusIcons

                    onToggled: {
                        root.appSettings.showTabStatusIcons = checked;
                        root.appSettings.save();
                    }
                }
            }
        }

        Forms.FormGroup {
            title: KI18n.i18nc("@title:group", "Window")
            visible: root.showWindowSettings

            Forms.FormEntry {
                contentItem: Controls.CheckBox {
                    objectName: "useCustomWindowSizeSetting"
                    Layout.fillWidth: true
                    text: KI18n.i18nc("@option:check", "Use custom size for new windows")
                    enabled: !root.appSettings.isUseCustomWindowSizeImmutable
                    checked: root.appSettings.useCustomWindowSize

                    onToggled: {
                        root.appSettings.useCustomWindowSize = checked;
                        root.appSettings.save();
                    }
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label:spinbox", "Columns:")
                enabled: root.appSettings.useCustomWindowSize && !root.appSettings.isWindowColumnsImmutable

                contentItem: Controls.SpinBox {
                    objectName: "windowColumnsSetting"
                    editable: true
                    from: 2
                    to: 999
                    value: root.appSettings.windowColumns

                    onValueModified: {
                        root.appSettings.windowColumns = value;
                        root.appSettings.save();
                    }
                }
            }

            Forms.FormEntry {
                title: KI18n.i18nc("@label:spinbox", "Rows:")
                enabled: root.appSettings.useCustomWindowSize && !root.appSettings.isWindowRowsImmutable

                contentItem: Controls.SpinBox {
                    objectName: "windowRowsSetting"
                    editable: true
                    from: 1
                    to: 999
                    value: root.appSettings.windowRows

                    onValueModified: {
                        root.appSettings.windowRows = value;
                        root.appSettings.save();
                    }
                }
            }
        }
    }

    function editScheme(createCopy: bool): void {
        schemeEditorLoader.active = true;
        (schemeEditorLoader.item as ColorSchemeEditor).edit(root.appSettings.colorScheme, createCopy);
    }

    Loader {
        id: schemeEditorLoader

        active: false

        sourceComponent: ColorSchemeEditor {
            appSettings: root.appSettings
            terminalPalettes: root.terminalPalettes

            onClosed: schemeEditorLoader.active = false
        }
    }

    FileDialog {
        id: paletteFile

        objectName: "importPaletteDialog"
        title: KI18n.i18nc("@title:window", "Import Color Scheme")
        nameFilters: [KI18n.i18nc("@item:inlistbox", "Konsole Color Schemes (*.colorscheme)")]
        fileMode: FileDialog.OpenFile

        onAccepted: root.terminalPalettes.importScheme(selectedFile)
    }
}
