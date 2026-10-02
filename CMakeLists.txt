cmake_minimum_required(VERSION 3.20)
project(NL_CommandCenter_v6_qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 QUIET COMPONENTS Core Gui Widgets Network OpenGL OpenGLWidgets Concurrent)

# Fallback Qt discovery when no preset/environment selection is active.
if(NOT Qt6_FOUND)
    if(DEFINED ENV{Qt6_DIR})
        set(Qt6_DIR "$ENV{Qt6_DIR}")
    elseif(DEFINED ENV{QTDIR})
        list(APPEND CMAKE_PREFIX_PATH "$ENV{QTDIR}")
    endif()

    if(WIN32)
        file(GLOB _qt_roots LIST_DIRECTORIES true
            "E:/Qt/*"
            "E:/QT/*"
            "C:/Qt/*"
        )

        foreach(_qt_root IN LISTS _qt_roots)
            file(GLOB _qt_variants LIST_DIRECTORIES true
                "${_qt_root}/mingw_*"
                "${_qt_root}/msvc*"
            )

            foreach(_qt_variant IN LISTS _qt_variants)
                if(EXISTS "${_qt_variant}/lib/cmake/Qt6/Qt6Config.cmake")
                    list(PREPEND CMAKE_PREFIX_PATH "${_qt_variant}")
                    set(_qt_found TRUE)
                    break()
                endif()
            endforeach()

            if(_qt_found)
                break()
            endif()
        endforeach()
    endif()

    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network OpenGL OpenGLWidgets Concurrent)
endif()

include(CTest)

# === V3 Redesign sources ===
# Older V3 page/service prototypes under src/ui/pages, src/service, and src/model
# are intentionally not compiled unless listed below; see docs/legacy_unused_sources.md.
set(V3_SOURCES
    src/core/channel_info.h
    src/core/channel_routing.h
    src/core/channel_routing.cpp
    src/core/system_status.h
    src/core/app_version.h
    src/core/acquisition_mode.h
    src/core/stim_protocol.h
    src/core/spi_protocol.h
    src/io/metadata_schema.h
    src/license/device_fingerprint.h
    src/license/device_fingerprint.cpp
    src/license/license_policy.h
    src/license/license_client.h
    src/license/license_client.cpp
    src/license/license_dialog.h
    src/license/license_dialog.cpp
    src/model/system_status_model.h
    src/model/system_status_model.cpp
    src/ui/widgets/top_status_bar.h
    src/ui/widgets/top_status_bar.cpp
    src/ui/widgets/session_toolbar.h
    src/ui/widgets/session_toolbar.cpp
    src/ui/widgets/settings_panel.h
    src/ui/widgets/settings_panel.cpp
    src/ui/widgets/stack_waveform_view.h
    src/ui/widgets/stack_waveform_view.cpp
    src/ui/widgets/sweep_waveform_view.h
    src/ui/widgets/sweep_waveform_view.cpp
    src/ui/widgets/recording_panel.h
    src/ui/widgets/recording_panel.cpp
    src/ui/widgets/activity_map_view.h
    src/ui/widgets/activity_map_view.cpp
    src/ui/widgets/spike_grid_view.h
    src/ui/widgets/spike_grid_view.cpp
    src/ui/widgets/spike_detail_window.h
    src/ui/widgets/spike_detail_window.cpp
    src/service/update_client.h
    src/service/update_client.cpp
    src/service/dummy_stream_server.h
    src/service/dummy_stream_server.cpp
    src/service/session_hub.h
    src/service/session_hub.cpp
    src/io/session_manifest.h
    src/io/session_manifest.cpp
    src/io/recording_worker.h
    src/io/recording_worker.cpp
    src/ui/theme/theme.qrc
)

# === Legacy sources ===
set(SOURCES
    src/main.cpp
    src/core/constants.h
    src/core/network_state.h
    src/core/channel_address_state.h
    src/core/channel_address_state.cpp
    src/core/channel_config_model.h
    src/core/channel_config_model.cpp
    src/core/tdm_context.h
    src/core/tdm_context.cpp
    src/config/config_manager.h
    src/config/config_manager.cpp
    src/core/threadsafe_queue.h
    src/core/stream_block_info.h
    src/data/ring_buffer.h
    src/data/data_store.h
    src/data/data_store.cpp
    src/io/session_recorder.h
    src/io/session_recorder.cpp
    src/io/matlab_channel_exporter.h
    src/io/matlab_channel_exporter.cpp
    src/network/data_sorter.h
    src/network/data_sorter.cpp
    src/network/socket_receiver2.h
    src/network/socket_receiver2.cpp
    src/network/timestamp_checker.h
    src/network/timestamp_checker.cpp
    src/network/replay_controller.h
    src/network/replay_controller.cpp
    src/network/sorter_worker.h
    src/network/sorter_worker.cpp
    src/signal/sndr_calculator.h
    src/signal/sndr_calculator.cpp
    src/signal/spike_filter.h
    src/signal/spike_filter.cpp
    src/signal/spike_snippet_store.h
    src/signal/spike_snippet_store.cpp
    src/service/spike_detect_worker.h
    src/service/spike_detect_worker.cpp
    src/theme/theme_manager.h
    src/theme/theme_manager.cpp
    src/ui/waveform_widget.h
    src/ui/waveform_widget.cpp
    src/ui/spi_control_panel.h
    src/ui/spi_control_panel.cpp
    src/ui/channel_map_panel.h
    src/ui/channel_map_panel.cpp
    src/ui/realtime_plot_panel.h
    src/ui/realtime_plot_panel.cpp
    src/ui/analyzer_panel.h
    src/ui/analyzer_panel.cpp
    src/ui/sweep_plot_panel.h
    src/ui/sweep_plot_panel.cpp
    src/ui/spike_panel.h
    src/ui/spike_panel.cpp
    src/ui/command_center_main_window.h
    src/ui/command_center_main_window.cpp
)

add_executable(${PROJECT_NAME} ${V3_SOURCES} ${SOURCES})

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    src
)

set(OPENSSL_ROOT_DIR "" CACHE PATH "OpenSSL root directory for Ed25519 license verification")
if(WIN32)
    foreach(_openssl_candidate
        "${OPENSSL_ROOT_DIR}"
        "E:/Qt/Tools/mingw1120_64/opt"
        "E:/QT/Tools/mingw1120_64/opt"
        "D:/software/Library"
    )
        if(_openssl_candidate
           AND EXISTS "${_openssl_candidate}/include/openssl/evp.h"
           AND EXISTS "${_openssl_candidate}/lib/libcrypto.dll.a")
            set(_openssl_root "${_openssl_candidate}")
            break()
        endif()
    endforeach()

    if(NOT _openssl_root)
        message(FATAL_ERROR "OpenSSL libcrypto not found; required for license response signature verification.")
    endif()

    target_include_directories(${PROJECT_NAME} PRIVATE "${_openssl_root}/include")
    target_link_libraries(${PROJECT_NAME} PRIVATE "${_openssl_root}/lib/libcrypto.dll.a")
    set(_openssl_crypto_dll "${_openssl_root}/bin/libcrypto-1_1-x64.dll")
else()
    find_package(OpenSSL REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::Crypto)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Concurrent
)

set(CCV2_DEFAULT_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/NL_CommandCenter_v6_config.ini")
if(EXISTS "${CCV2_DEFAULT_CONFIG}")
    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CCV2_DEFAULT_CONFIG}"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/NL_CommandCenter_v6_config.ini"
        COMMENT "Copying default user config"
        VERBATIM
    )
endif()

if(WIN32)
    option(ENABLE_AUTO_QT_DEPLOY "Automatically copy Qt runtime DLLs next to executable" ON)
    if(EXISTS "${_openssl_crypto_dll}")
        add_custom_command(
            TARGET ${PROJECT_NAME}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_openssl_crypto_dll}"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            COMMENT "Copying OpenSSL libcrypto for license verification"
            VERBATIM
        )
    endif()

    if(ENABLE_AUTO_QT_DEPLOY)
        set(_windeployqt "")

        if(DEFINED ENV{WINDEPLOYQT_EXECUTABLE} AND EXISTS "$ENV{WINDEPLOYQT_EXECUTABLE}")
            set(_windeployqt "$ENV{WINDEPLOYQT_EXECUTABLE}")
        endif()

        if(NOT _windeployqt AND DEFINED Qt6_DIR)
            get_filename_component(_qt6_cmake_dir "${Qt6_DIR}" DIRECTORY)
            get_filename_component(_qt6_lib_dir "${_qt6_cmake_dir}" DIRECTORY)
            get_filename_component(_qt6_prefix "${_qt6_lib_dir}" DIRECTORY)
            set(_windeployqt_candidate "${_qt6_prefix}/bin/windeployqt.exe")
            if(EXISTS "${_windeployqt_candidate}")
                set(_windeployqt "${_windeployqt_candidate}")
            endif()
        endif()

        if(NOT _windeployqt)
            find_program(_windeployqt NAMES windeployqt.exe windeployqt
                HINTS
                    "E:/Qt/6.5.3/mingw_64/bin"
                    "E:/QT/6.5.3/mingw_64/bin"
                    "C:/Qt/6.5.3/mingw_64/bin"
            )
        endif()

        if(_windeployqt)
            add_custom_command(
                TARGET ${PROJECT_NAME}
                POST_BUILD
                COMMAND "${_windeployqt}" --dir "$<TARGET_FILE_DIR:${PROJECT_NAME}>" "$<TARGET_FILE:${PROJECT_NAME}>"
                COMMENT "Deploying Qt runtime DLLs next to executable"
                VERBATIM
            )
        else()
            message(WARNING "windeployqt not found; Qt DLL deployment is skipped.")
        endif()
    endif()
endif()

if(BUILD_TESTING)
    get_filename_component(_cxx_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)

    add_executable(ccv2_sndr_smoke
        tests/sndr_calculator_smoke.cpp
        src/signal/sndr_calculator.cpp
    )

    target_include_directories(ccv2_sndr_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        src
    )

    target_link_libraries(ccv2_sndr_smoke PRIVATE
        Qt6::Core
    )

    add_test(
        NAME sndr_smoke
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_sndr_smoke>
    )

    add_executable(ccv2_sndr_reference_compat
        tests/sndr_reference_compat.cpp
        src/signal/sndr_calculator.cpp
    )

    target_include_directories(ccv2_sndr_reference_compat PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        src
    )

    target_link_libraries(ccv2_sndr_reference_compat PRIVATE
        Qt6::Core
    )

    add_test(
        NAME sndr_reference_compat
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_sndr_reference_compat>
    )

    add_executable(ccv2_license_policy_smoke
        tests/license_policy_smoke.cpp
    )

    target_include_directories(ccv2_license_policy_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        src
    )

    target_link_libraries(ccv2_license_policy_smoke PRIVATE
        Qt6::Core
    )

    add_test(
        NAME license_policy_smoke
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_license_policy_smoke>
    )

    add_executable(ccv2_ui_pipeline_smoke
        tests/ui_pipeline_smoke.cpp
        src/core/channel_routing.cpp
        src/network/data_sorter.cpp
        src/network/sorter_worker.cpp
        src/data/data_store.cpp
        src/io/session_manifest.cpp
        src/io/session_recorder.cpp
        src/io/matlab_channel_exporter.cpp
        src/signal/sndr_calculator.cpp
        src/ui/waveform_widget.cpp
    )

    target_include_directories(ccv2_ui_pipeline_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        src
    )

    target_link_libraries(ccv2_ui_pipeline_smoke PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )

    add_test(
        NAME ui_pipeline_smoke
        COMMAND ${CMAKE_COMMAND} -E env
            "QT_QPA_PLATFORM=offscreen"
            "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins"
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_ui_pipeline_smoke>
    )

    add_executable(ccv2_socket_receiver2_smoke
        tests/socket_receiver2_smoke.cpp
        src/network/socket_receiver2.cpp
        src/network/data_sorter.cpp
    )

    target_include_directories(ccv2_socket_receiver2_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        src
    )

    target_link_libraries(ccv2_socket_receiver2_smoke PRIVATE
        Qt6::Core
        Qt6::Network
    )

    add_test(
        NAME socket_receiver2_smoke
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_socket_receiver2_smoke>
    )

    add_executable(ccv2_dummy_stream_server_smoke
        tests/dummy_stream_server_smoke.cpp
        src/service/dummy_stream_server.cpp
        src/network/socket_receiver2.cpp
        src/network/data_sorter.cpp
        src/network/timestamp_checker.cpp
        src/signal/spike_filter.cpp
        src/signal/spike_snippet_store.cpp
        src/service/spike_detect_worker.cpp
    )

    target_include_directories(ccv2_dummy_stream_server_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        src
    )

    target_link_libraries(ccv2_dummy_stream_server_smoke PRIVATE
        Qt6::Core
        Qt6::Network
    )

    add_test(
        NAME dummy_stream_server_smoke
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_dummy_stream_server_smoke>
    )

    add_executable(ccv2_timestamp_checker_smoke
        tests/timestamp_checker_smoke.cpp
        src/network/timestamp_checker.cpp
    )

    target_include_directories(ccv2_timestamp_checker_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        src
    )

    target_link_libraries(ccv2_timestamp_checker_smoke PRIVATE
        Qt6::Core
    )

    add_test(
        NAME timestamp_checker_smoke
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_timestamp_checker_smoke>
    )

    add_executable(ccv2_spike_filter_smoke
        tests/spike_filter_smoke.cpp
        src/signal/spike_filter.cpp
    )

    target_include_directories(ccv2_spike_filter_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        src
    )

    target_link_libraries(ccv2_spike_filter_smoke PRIVATE
        Qt6::Core
    )

    add_test(
        NAME spike_filter_smoke
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_spike_filter_smoke>
    )

    add_executable(ccv2_spike_grid_view_smoke
        tests/spike_grid_view_smoke.cpp
        src/signal/spike_snippet_store.cpp
        src/ui/widgets/spike_grid_view.cpp
    )

    target_include_directories(ccv2_spike_grid_view_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        src
    )

    target_link_libraries(ccv2_spike_grid_view_smoke PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
    )

    add_test(
        NAME spike_grid_view_smoke
        COMMAND ${CMAKE_COMMAND} -E env
            "QT_QPA_PLATFORM=offscreen"
            "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins"
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_spike_grid_view_smoke>
    )

    add_executable(ccv2_spike_snippet_smoke
        tests/spike_snippet_smoke.cpp
        src/signal/spike_filter.cpp
        src/signal/spike_snippet_store.cpp
        src/service/spike_detect_worker.cpp
    )

    target_include_directories(ccv2_spike_snippet_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        src
    )

    target_link_libraries(ccv2_spike_snippet_smoke PRIVATE
        Qt6::Core
    )

    add_test(
        NAME spike_snippet_smoke
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_spike_snippet_smoke>
    )

    add_executable(ccv2_session_hub_smoke
        tests/session_hub_smoke.cpp
        src/service/session_hub.cpp
        src/core/tdm_context.h
        src/core/tdm_context.cpp
        src/io/recording_worker.cpp
        src/io/session_manifest.cpp
        src/io/session_recorder.cpp
        src/service/dummy_stream_server.cpp
        src/network/socket_receiver2.cpp
        src/network/replay_controller.h
        src/network/replay_controller.cpp
    )

    target_include_directories(ccv2_session_hub_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        src
    )

    target_link_libraries(ccv2_session_hub_smoke PRIVATE
        Qt6::Core
        Qt6::Network
    )

    add_test(
        NAME session_hub_smoke
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_session_hub_smoke>
    )

    add_executable(ccv2_session_replay_smoke
        tests/session_replay_smoke.cpp
        src/service/session_hub.cpp
        src/core/tdm_context.h
        src/core/tdm_context.cpp
        src/io/recording_worker.cpp
        src/io/session_manifest.cpp
        src/io/session_recorder.cpp
        src/network/socket_receiver2.cpp
        src/network/replay_controller.h
        src/network/replay_controller.cpp
    )

    target_include_directories(ccv2_session_replay_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        src
    )

    target_link_libraries(ccv2_session_replay_smoke PRIVATE
        Qt6::Core
        Qt6::Network
    )

    add_test(
        NAME session_replay_smoke
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_session_replay_smoke>
    )

    add_executable(ccv2_session_state_smoke
        tests/session_state_smoke.cpp
        src/service/session_hub.cpp
        src/core/tdm_context.h
        src/core/tdm_context.cpp
        src/io/recording_worker.cpp
        src/io/session_manifest.cpp
        src/io/session_recorder.cpp
        src/service/dummy_stream_server.cpp
        src/network/socket_receiver2.cpp
        src/network/replay_controller.h
        src/network/replay_controller.cpp
    )

    target_include_directories(ccv2_session_state_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        src
    )

    target_link_libraries(ccv2_session_state_smoke PRIVATE
        Qt6::Core
        Qt6::Network
    )

    add_test(
        NAME session_state_smoke
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_session_state_smoke>
    )

    # --- TDM / correctness fixes (v6.1) ---
    function(ccv2_add_smoke name)
        add_executable(${name} ${ARGN})
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} src)
        target_link_libraries(${name} PRIVATE Qt6::Core)
        add_test(
            NAME ${name}
            COMMAND ${CMAKE_COMMAND} -E env
                "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
                $<TARGET_FILE:${name}>
        )
    endfunction()

    ccv2_add_smoke(ccv2_tdm_context_smoke
        tests/tdm_context_smoke.cpp
        src/core/tdm_context.cpp
        src/io/session_manifest.cpp
    )
    ccv2_add_smoke(ccv2_queue_drop_parity_smoke
        tests/queue_drop_parity_smoke.cpp
    )
    ccv2_add_smoke(ccv2_tdm_phase_b_export_smoke
        tests/tdm_phase_b_export_smoke.cpp
        src/io/matlab_channel_exporter.cpp
        src/io/session_manifest.cpp
        src/core/channel_routing.cpp
    )
    ccv2_add_smoke(ccv2_mean_metric_reference_smoke
        tests/mean_metric_reference_smoke.cpp
        src/network/data_sorter.cpp
    )
    ccv2_add_smoke(ccv2_trigger_tdm_slot_smoke
        tests/trigger_tdm_slot_smoke.cpp
    )
    ccv2_add_smoke(ccv2_frame_timestamp_reconciler_smoke
        tests/frame_timestamp_reconciler_smoke.cpp
    )
    ccv2_add_smoke(ccv2_tdm_timestamp_gap_export_smoke
        tests/tdm_timestamp_gap_export_smoke.cpp
        src/io/matlab_channel_exporter.cpp
        src/io/session_manifest.cpp
        src/core/channel_routing.cpp
    )
    ccv2_add_smoke(ccv2_tdm_upstream_gap_metric_smoke
        tests/tdm_upstream_gap_metric_smoke.cpp
        src/network/data_sorter.cpp
    )
    ccv2_add_smoke(ccv2_system_status_model_smoke
        tests/system_status_model_smoke.cpp
        src/model/system_status_model.cpp
    )
    ccv2_add_smoke(ccv2_channel_config_model_smoke
        tests/channel_config_model_smoke.cpp
        src/core/channel_config_model.cpp
    )

    foreach(_session_test ccv2_session_hub_smoke ccv2_session_replay_smoke ccv2_session_state_smoke)
        target_sources(${_session_test} PRIVATE src/network/timestamp_checker.cpp)
        target_link_libraries(${_session_test} PRIVATE Qt6::Concurrent)
    endforeach()

    add_executable(ccv2_review_regression
        tests/review_regression.cpp
        src/service/session_hub.cpp
        src/network/socket_receiver2.cpp
        src/network/replay_controller.cpp
        src/network/data_sorter.cpp
        src/network/sorter_worker.cpp
        src/network/timestamp_checker.cpp
        src/io/recording_worker.cpp
        src/io/session_recorder.cpp
        src/io/session_manifest.cpp
        src/io/matlab_channel_exporter.cpp
        src/core/tdm_context.cpp
        src/core/channel_routing.cpp
        src/core/channel_address_state.cpp
        src/core/network_state.h
        src/config/config_manager.cpp
        src/data/data_store.cpp
        src/service/spike_detect_worker.cpp
        src/signal/spike_filter.cpp
        src/signal/spike_snippet_store.cpp
        src/signal/sndr_calculator.cpp
        src/ui/analyzer_panel.cpp
        src/ui/spi_control_panel.cpp
        src/ui/waveform_widget.cpp
    )
    target_include_directories(ccv2_review_regression PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} src)
    target_link_libraries(ccv2_review_regression PRIVATE Qt6::Core Qt6::Network Qt6::Widgets Qt6::Concurrent)
    foreach(_case eof input split tdm spike_seek io_error zoom sample_rate fft spi_target record_integrity memory export_time seek_gap epoch connection_epoch record_tail)
        add_test(NAME review_${_case} COMMAND ${CMAKE_COMMAND} -E env
            "QT_QPA_PLATFORM=offscreen"
            "QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins"
            "PATH=$<TARGET_FILE_DIR:Qt6::Core>\;${_cxx_bin_dir}\;$ENV{PATH}"
            $<TARGET_FILE:ccv2_review_regression> ${_case})
        set_tests_properties(review_${_case} PROPERTIES TIMEOUT 30)
    endforeach()

endif()
