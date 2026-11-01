# Moho's replays (.SCFAReplay) as the engine's (.oscreplay), outside the
# engine: the game plays only its own replays.
add_library(osc_scfa STATIC moho_replay.cpp convert.cpp)
add_library(osc::scfa ALIAS osc_scfa)
target_include_directories(osc_scfa PUBLIC ${CMAKE_SOURCE_DIR}/tools)
target_link_libraries(osc_scfa PUBLIC osc::sim PRIVATE osc::warnings)

add_executable(osc_scfa_convert main.cpp)
target_link_libraries(osc_scfa_convert PRIVATE osc::warnings osc::scfa)
