# Legacy unused sources

These files are kept only as V3 prototype/reference code. They are not part of
the current V5 executable because they are not listed in `CMakeLists.txt`.

The active V5 main window is built from:

- `src/ui/command_center_main_window.*`
- `src/ui/channel_map_panel.*`
- `src/ui/realtime_plot_panel.*`
- `src/ui/analyzer_panel.*`
- `src/ui/spi_control_panel.*`
- compiled widgets listed in `V3_SOURCES`

Do not treat the following files as current runtime behavior unless they are
explicitly re-added to `CMakeLists.txt` and wired into the main window:

- `src/service/acquisition_service.*`
- `src/service/analyzer_pipeline.*`
- `src/service/preview_pipeline.*`
- `src/service/safety_interlock.*`
- `src/service/spi_command_service.*`
- `src/model/console_log_model.*`
- `src/model/stimulation_model.*`
- `src/ui/pages/array_preview_page.*`
- `src/ui/pages/channel_map_page.*`
- `src/ui/pages/hardware_control_page.*`
- `src/ui/pages/page_base.*`

`src/service/update_client.*`, `src/model/channel_map_model.*`, and
`src/model/system_status_model.*` are still compiled and are not legacy-only.
