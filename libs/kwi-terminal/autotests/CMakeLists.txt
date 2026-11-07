# SPDX-FileCopyrightText: 2026 Eike Hein <hein@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

find_package(Python3 REQUIRED COMPONENTS Interpreter)

file(GLOB stock_palettes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../data/color-schemes/*.colorscheme")
foreach(palette IN LISTS stock_palettes)
    get_filename_component(name "${palette}" NAME)
    configure_file("${palette}" "${KWI_TEST_DATA_DIR}/kwi/color-schemes/stock/${name}" COPYONLY)
endforeach()

ecm_add_test(ptyprocesstest.cpp
    LINK_LIBRARIES kwi-terminal Qt6::Test KF6::Pty KF6::I18n KF6::CoreAddons)
ecm_add_test(hostprocessprotocoltest.cpp
    LINK_LIBRARIES kwi-terminal Qt6::Test Qt6::DBus)
target_compile_definitions(hostprocessprotocoltest PRIVATE QT_NO_KEYWORDS)
ecm_add_test(outputsavejobtest.cpp LINK_LIBRARIES kwi-terminal ghostty-qt Qt6::Test KF6::CoreAddons)
ecm_add_test(sessiontest.cpp LINK_LIBRARIES kwi-terminal Qt6::Test)
ecm_add_test(texthighlightertest.cpp LINK_LIBRARIES kwi-terminal Qt6::Test)
set_tests_properties(texthighlightertest PROPERTIES LABELS "unit;terminal")
set_tests_properties(ptyprocesstest hostprocessprotocoltest outputsavejobtest sessiontest
    PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(ptyprocesstest PROPERTIES LABELS "integration;process")
set_tests_properties(hostprocessprotocoltest PROPERTIES LABELS "unit;process")
set_tests_properties(outputsavejobtest PROPERTIES LABELS "integration;storage")
set_tests_properties(sessiontest PROPERTIES LABELS "integration;terminal;process")

ecm_add_test(accessibilitytest.cpp LINK_LIBRARIES kwi-terminal ghostty-qt Qt6::Test KF6::I18n)
ecm_add_test(clipboardtest.cpp LINK_LIBRARIES kwi-terminal ghostty-qt Qt6::Test)
ecm_add_test(imetest.cpp LINK_LIBRARIES kwi-terminal Qt6::Test Qt6::QuickTest)
ecm_add_test(terminalinputtest.cpp LINK_LIBRARIES kwi-terminal ghostty-qt Qt6::Test Qt6::QuickTest KF6::I18n KF6::CoreAddons KF6::KIOCore)
ecm_add_test(terminalsearchtest.cpp LINK_LIBRARIES kwi-terminal ghostty-qt Qt6::Test KF6::I18n)

ecm_add_test(qmlconsumertest.cpp LINK_LIBRARIES Qt6::Quick Qt6::QuickControls2 Qt6::Test)
add_dependencies(qmlconsumertest kwi-terminalplugin)
foreach(test IN ITEMS ptyprocesstest imetest terminalinputtest qmlconsumertest)
    target_compile_definitions(${test} PRIVATE KWI_TEST_PYTHON="${Python3_EXECUTABLE}")
endforeach()

set_tests_properties(accessibilitytest clipboardtest imetest terminalinputtest terminalsearchtest qmlconsumertest
    PROPERTIES TIMEOUT 60 LABELS "integration;terminal;gui" RESOURCE_LOCK kwi_gui)
get_property(terminal_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${terminal_tests} PROPERTIES
    ENVIRONMENT_MODIFICATION
        "QML_IMPORT_PATH=path_list_prepend:${CMAKE_CURRENT_BINARY_DIR}/../src/qml;LD_LIBRARY_PATH=path_list_prepend:$<TARGET_FILE_DIR:kwi-terminal>;XDG_DATA_DIRS=path_list_prepend:${KWI_TEST_DATA_DIR};LANGUAGE=set:en")
