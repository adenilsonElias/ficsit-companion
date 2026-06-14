project(ficsit-companion)

set(HEADER_FILES
	include/domain/building.hpp
	include/domain/fractional_number.hpp
	include/domain/game_data.hpp
	include/domain/graph_model.hpp
	include/domain/graph_item_resolve.hpp
	include/domain/json.hpp
	include/domain/linear_solve.hpp
	include/domain/link.hpp
	include/domain/node.hpp
	include/domain/node_data_resolver.hpp
	include/domain/pin.hpp
	include/domain/rate_solver.hpp
	include/domain/recipe.hpp
	include/domain/resource_flow.hpp
	include/domain/vehicle_map.hpp
	include/domain/vehicle_map_camera.hpp
	include/domain/vehicle_map_query.hpp
	include/infra/editor_backend.hpp
	include/infra/file_store.hpp
	include/infra/sav_import.hpp
	include/infra/sav_import_service.hpp
	include/infra/sav_runner.hpp
	include/infra/save_watcher.hpp
	include/infra/session_serializer.hpp
	include/infra/settings_store.hpp
	include/infra/vehicle_map_session.hpp
	include/app/base_app.hpp
	include/app/production_app.hpp
	include/app/utils.hpp
	include/app/vehicle_map_app.hpp
	include/third_party/stb_image.h
)

# --- fc-core: pure/linkable sources (no SDL/OpenGL headers required) ---
# Split by architectural layer. Both lists feed fc-core; the separation
# documents the dependency rule (infra may depend on domain, never the reverse).
# domain/ : pure logic, zero UI/IO dependencies.
set(DOMAIN_SOURCE_FILES
    src/domain/building.cpp
    src/domain/fractional_number.cpp
    src/domain/game_data.cpp
    src/domain/graph_model.cpp
    src/domain/graph_item_resolve.cpp
    src/domain/json.cpp
    src/domain/linear_solve.cpp
    src/domain/link.cpp
    src/domain/node_base.cpp
    src/domain/craft_node.cpp
    src/domain/group_node.cpp
    src/domain/organizer_nodes.cpp
    src/domain/sink_node.cpp
    src/domain/logistics_node.cpp
    src/domain/vehicle_station_node.cpp
    src/domain/vehicle_route.cpp
    src/domain/extractor_node.cpp
    src/domain/node_data_resolver.cpp
    src/domain/pin.cpp
    src/domain/rate_solver.cpp
    src/domain/recipe.cpp
    src/domain/resource_flow.cpp
    src/domain/vehicle_map.cpp
    src/domain/vehicle_map_camera.cpp
    src/domain/vehicle_map_query.cpp
)

# infra/ : ports + adapters (file I/O, editor backend, serialization, sav import).
set(INFRA_SOURCE_FILES
    src/infra/editor_backend.cpp
    src/infra/file_store.cpp
    src/infra/sav_import.cpp
    src/infra/sav_import_service.cpp
    src/infra/sav_runner.cpp
    src/infra/save_watcher.cpp
    src/infra/session_serializer.cpp
    src/infra/settings_store.cpp
    src/infra/vehicle_map_session.cpp
)

add_library(fc-core STATIC ${DOMAIN_SOURCE_FILES} ${INFRA_SOURCE_FILES})
set_property(TARGET fc-core PROPERTY CXX_STANDARD 17)
target_include_directories(fc-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(fc-core PUBLIC ${imgui_INCLUDE_FOLDERS})

# --- ficsit-companion executable: rendering + main + utils (needs SDL/OpenGL) ---
# utils.cpp is here rather than fc-core because it includes SDL_opengl.h
# (SDL2 include path is only wired to the app target, not fc-core).
set(APP_SOURCE_FILES
    ${imgui_SOURCE}

    src/app/base_app.cpp
    src/app/production_app.cpp
    src/app/utils.cpp
    src/app/vehicle_map_app.cpp

    src/app/main.cpp
)
source_group(ImGui FILES ${imgui_SOURCE})

add_executable(${PROJECT_NAME} ${HEADER_FILES} ${APP_SOURCE_FILES})

# Reflect the domain/infra/app folder layout in IDE (e.g. Visual Studio) views.
# imgui_SOURCE lives outside src/ and is grouped separately above.
set(APP_PROJECT_SOURCES
    src/app/base_app.cpp
    src/app/production_app.cpp
    src/app/utils.cpp
    src/app/vehicle_map_app.cpp
    src/app/main.cpp
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src FILES
    ${DOMAIN_SOURCE_FILES} ${INFRA_SOURCE_FILES} ${APP_PROJECT_SOURCES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/include FILES ${HEADER_FILES})
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(${PROJECT_NAME} PRIVATE ${imgui_INCLUDE_FOLDERS})
target_link_libraries(${PROJECT_NAME} PRIVATE fc-core)

if (NOT DEFINED EMSCRIPTEN)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${OPENGL_LIBRARIES})
    set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:${PROJECT_NAME}>)
    if(TARGET SDL2::SDL2main)
        target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2main)
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE SDL2::SDL2-static)

    # Copy assets next to output build
    add_custom_command(
      TARGET ${PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/../assets $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )

    # Copy save-import tooling (Node wrapper script + package.json) next to output build
    add_custom_command(
      TARGET ${PROJECT_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/tools/sav_import $<TARGET_FILE_DIR:${PROJECT_NAME}>/tools/sav_import
    )

    install(TARGETS ${PROJECT_NAME} DESTINATION .)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../assets/" DESTINATION .)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/sav_import/" DESTINATION tools/sav_import)
else()
    # Make emscripten outputs html file instead of default js and redirect all emscripten generated files to a dedicated web folder
    set_target_properties(${PROJECT_NAME} PROPERTIES
        SUFFIX ".html"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/../web"
    )
    target_compile_options(${PROJECT_NAME} PRIVATE
        "-Os"
        "--use-port=sdl2"
        "-sDISABLE_EXCEPTION_CATCHING=0"
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        "-Os"
        "--use-port=sdl2"
        "-sWASM=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sNO_EXIT_RUNTIME=0"
        "-sDISABLE_EXCEPTION_CATCHING=0"
        "-sASYNCIFY" # TODO: switch to JSPI once there is better support
        "--preload-file" "${CMAKE_CURRENT_SOURCE_DIR}/../assets@/"
        "--preload-file" "${CMAKE_CURRENT_SOURCE_DIR}/tools/sav_import/web_loader.js@/sav_import/web_loader.js"
        "--shell-file" "${CMAKE_CURRENT_SOURCE_DIR}/../emscripten/shell_index.html"
        "-sMINIFY_HTML=0"
    )
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rename $<TARGET_FILE:${PROJECT_NAME}> $<TARGET_FILE_DIR:${PROJECT_NAME}>/index.html
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/../assets/icon.png $<TARGET_FILE_DIR:${PROJECT_NAME}>/icon.png
    )
endif()

# --- fc-tests: Catch2 test executable (desktop only) ---
# Add new test .cpp files here as more modules are extracted.
if (NOT DEFINED EMSCRIPTEN AND TARGET Catch2::Catch2WithMain)
    set(TEST_SOURCE_FILES
        tests/test_smoke.cpp
        tests/test_graph_types_link.cpp
        tests/test_file_store.cpp
        tests/test_settings_store.cpp
        tests/test_editor_backend.cpp
        tests/test_rate_solver.cpp
        tests/test_graph_model.cpp
        tests/test_session_serializer.cpp
        tests/test_sav_import.cpp
        tests/test_sav_import_service.cpp
        tests/test_vehicle_map_camera.cpp
        tests/test_vehicle_map_query.cpp
        tests/test_vehicle_map_session.cpp
        tests/test_vehicle_map_import.cpp
        tests/test_node_serialization.cpp
        tests/test_vehicle_station_node.cpp
        tests/test_vehicle_route_solve.cpp
        tests/test_linear_solve.cpp
        tests/test_group_node.cpp
        tests/test_resource_flow.cpp
    )
    add_executable(fc-tests ${TEST_SOURCE_FILES})
    set_property(TARGET fc-tests PROPERTY CXX_STANDARD 17)
    target_include_directories(fc-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    # Non-backend imgui + node-editor sources are needed because fc-core types
    # (FractionalNumber::RenderInputText, Recipe::Render, etc.) reference ImGui
    # functions at link time even in headless tests.
    target_sources(fc-tests PRIVATE ${imgui_core_SOURCE})

    # utils.cpp provides LoadTextureFromFile, which is referenced (at link time)
    # by recipe.cpp (Item::Item) and game_data.cpp (LoadData).
    # It requires SDL_opengl.h (via SDL2) and OpenGL at link time.
    target_sources(fc-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/app/utils.cpp)

    target_link_libraries(fc-tests PRIVATE fc-core Catch2::Catch2WithMain)
    target_link_libraries(fc-tests PRIVATE ${OPENGL_LIBRARIES})
    if(TARGET SDL2::SDL2-static)
        target_link_libraries(fc-tests PRIVATE SDL2::SDL2-static)
    endif()

    include(Catch)
    catch_discover_tests(fc-tests)
endif()
