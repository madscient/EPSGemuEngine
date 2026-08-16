cmake_minimum_required(VERSION 3.20)
project(EPSGemuEngine VERSION 1.0.0 LANGUAGES CXX)

# mpeg_audio が std::numbers::pi を使うため C++20 が必要
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =======================================================
#  ソースファイル
# =======================================================
set(CORE_SOURCES
    # ay8910  AY8930 / YM2149 / AY-3-8910 → EPSG / SSG / PSG
    extern/ay8910/ay8910.cpp
    # mpeg_audio  AMM デコーダ → SSGS3
    extern/mpeg_audio/mpeg_audio.cpp
)

set(ENGINE_SOURCES
    src/EPSGemuEngine.cpp
    src/YmzSsg.cpp
    src/Ymz770.cpp
)

# =======================================================
#  共有ライブラリ
#  出力ファイル名: EPSGemuEngine.dll / libEPSGemuEngine.so / libEPSGemuEngine.dylib
#  FMEngineTest から使う場合は -e オプションでパスを指定する。
# =======================================================
add_library(EPSGemuEngine SHARED ${ENGINE_SOURCES} ${CORE_SOURCES})

target_include_directories(EPSGemuEngine PRIVATE
    src                     # FmEngineApi.h
    extern/ay8910
    extern/mpeg_audio
)

target_compile_definitions(EPSGemuEngine PRIVATE FMENGINE_EXPORTS)

# -------------------------------------------------------
#  プラットフォーム別設定
# -------------------------------------------------------
if(MSVC)
    target_compile_options(EPSGemuEngine PRIVATE /W3 /utf-8)
    # フォークしたコアは無改変で保つため、上流由来の縮小変換警告のみ抑止する
    set_source_files_properties(extern/ay8910/ay8910.cpp PROPERTIES COMPILE_OPTIONS "/wd4244")
    set_target_properties(EPSGemuEngine PROPERTIES
        OUTPUT_NAME              "EPSGemuEngine"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        PDB_OUTPUT_DIRECTORY     "${CMAKE_BINARY_DIR}/bin"
    )
else()
    target_compile_options(EPSGemuEngine PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fvisibility=hidden   # 非公開シンボルを隠す
    )
    find_package(Threads REQUIRED)
    target_link_libraries(EPSGemuEngine PRIVATE Threads::Threads)
    set_target_properties(EPSGemuEngine PROPERTIES
        OUTPUT_NAME              "EPSGemuEngine"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

# -------------------------------------------------------
#  インストール (任意)
# -------------------------------------------------------
install(TARGETS EPSGemuEngine
    RUNTIME DESTINATION bin   # Windows DLL
    LIBRARY DESTINATION lib   # Linux/macOS .so/.dylib
)
install(FILES src/FmEngineApi.h DESTINATION include)
