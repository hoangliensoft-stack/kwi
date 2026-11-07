# SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

add_library(kwi-terminal SHARED)
set_target_properties(kwi-terminal PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION})
generate_export_header(kwi-terminal
    EXPORT_MACRO_NAME KWI_TERMINAL_EXPORT
    EXPORT_FILE_NAME kwiterminal_export.h)

set_source_files_properties(declarative/Terminal.qml PROPERTIES QT_RESOURCE_ALIAS Terminal.qml)
ecm_add_qml_module(kwi-terminal
    GENERATE_PLUGIN_SOURCE
    URI org.kde.kwi.terminal
    VERSION 1.0
    RESOURCE_PREFIX "/qt/qml"
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qml/org/kde/kwi/terminal"
    CLASS_NAME KwiTerminalModulePlugin
    DEPENDENCIES QtQuick
    IMPORTS QtQuick.Controls org.kde.kirigami org.kde.ki18n
    QML_FILES declarative/Terminal.qml
    SOURCES
        Session.h Session.cpp SessionEvents_p.h
        OutputSaveJob.h OutputSaveJob.cpp
        SessionTitleFormatter.h SessionTitleFormatter.cpp
        TitleFormat_p.h TitleFormat.cpp
        ProcessLaunch.h ProcessLaunch.cpp
        TerminalColors.h TerminalColors.cpp
        process/ProcessInfo_p.h process/ProcessQueryLimits_p.h
        process/ProcessController.cpp process/ProcessController_p.h
        process/ProcessScope.cpp process/ProcessScope_p.h
        process/NativeProcess.cpp process/NativeProcess_p.h
        process/AttachedProcess.cpp process/AttachedProcess_p.h
        process/NativeProcessInfo.cpp process/NativeProcessInfo_p.h
        process/HostProcessProtocol.cpp process/HostProcessProtocol_p.h
        process/HostProcess.cpp process/HostProcess_p.h
        process/HostCommandQuery.cpp process/HostCommandQuery_p.h
        process/PtyProcess.h process/PtyProcess.cpp
        process/SshMetadataResolver_p.h process/SshMetadataResolver.cpp
        TerminalView.h TerminalView.cpp
        TerminalAccessible.cpp
        TerminalSearch.h TerminalSearch.cpp
        TextHighlighter_p.h TextHighlighter.cpp
)
target_include_directories(kwi-terminal PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
# Private C++ headers use ghostty-qt types. Its implementation is linked here.
target_link_libraries(kwi-terminal
    PUBLIC Qt6::Core Qt6::Gui Qt6::Quick $<COMPILE_ONLY:ghostty-qt>
    PRIVATE ghostty-qt ghostty-qt-quick Qt6::Concurrent Qt6::DBus Qt6::Network Qt6::QuickControls2
        KF6::CoreAddons KF6::Pty KF6::ConfigGui KF6::GuiAddons KF6::I18n KF6::I18nQml
        KF6::Kirigami)
target_compile_definitions(kwi-terminal PRIVATE TRANSLATION_DOMAIN="kwi-terminal")

install(TARGETS kwi-terminal ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
ecm_finalize_qml_module(kwi-terminal DESTINATION ${KDE_INSTALL_QMLDIR})

ecm_generate_qdoc(kwi-terminal kwi-terminal.qdocconf)
