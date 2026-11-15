cmake_minimum_required(VERSION 3.10)
project(wingmere C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Bake the shader file into the binary so the game is a single executable.
set(SHADER_FILE ${CMAKE_SOURCE_DIR}/shaders/wingmere.glsl)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SHADER_FILE})
file(READ ${SHADER_FILE} SHADER_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," SHADER_HEX "${SHADER_HEX}")
file(WRITE ${CMAKE_BINARY_DIR}/shaders_gen.h "static const unsigned char shader_src[] = {${SHADER_HEX}};\n")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(CURL REQUIRED libcurl)
pkg_check_modules(ZLIB REQUIRED zlib)
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

add_executable(wingmere
  src/main.c src/geo.c src/tiles.c src/airport.c src/terrain.c
  src/aircraft.c src/flight.c src/render.c src/hud.c src/audio.c src/mvt.c src/traffic.c)
target_compile_definitions(wingmere PRIVATE _GNU_SOURCE WINGMERE_DATADIR="${CMAKE_INSTALL_PREFIX}/share/wingmere/data")
target_include_directories(wingmere PRIVATE ${SDL2_INCLUDE_DIRS} ${CURL_INCLUDE_DIRS} ${CMAKE_BINARY_DIR} src)
target_link_directories(wingmere PRIVATE ${SDL2_LIBRARY_DIRS} ${CURL_LIBRARY_DIRS})
target_link_libraries(wingmere PRIVATE ${SDL2_LIBRARIES} ${CURL_LIBRARIES} ${ZLIB_LIBRARIES} OpenGL::GL Threads::Threads m)
target_compile_options(wingmere PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation ${SDL2_CFLAGS_OTHER})

include(GNUInstallDirs)
install(TARGETS wingmere RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY data/ DESTINATION ${CMAKE_INSTALL_DATADIR}/wingmere/data)
install(FILES packaging/wingmere.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/wingmere.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES shaders/wingmere.glsl DESTINATION ${CMAKE_INSTALL_DATADIR}/wingmere)
