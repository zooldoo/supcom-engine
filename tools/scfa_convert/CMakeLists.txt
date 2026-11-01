# Moho's replays (.SCFAReplay) as the engine's (.oscreplay), and the engine's
# entity trace against retail's /synclog, outside the engine: the game plays
# only its own replays.
add_library(osc_scfa STATIC moho_replay.cpp convert.cpp synclog.cpp)
add_library(osc::scfa ALIAS osc_scfa)
target_include_directories(osc_scfa PUBLIC ${CMAKE_SOURCE_DIR}/tools)
target_link_libraries(osc_scfa PUBLIC osc::sim PRIVATE osc::warnings)

add_executable(osc_scfa_convert main.cpp)
target_link_libraries(osc_scfa_convert PRIVATE osc::warnings osc::scfa)

add_executable(osc_synclog_diff synclog_diff.cpp)
target_link_libraries(osc_synclog_diff PRIVATE osc::warnings osc::scfa)
