cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

project(binja_diff_view C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(BINJA_DIFF_BUILD_UI_PLUGIN
    "Build the Binary Ninja UI plugin" ON)
option(BINJA_DIFF_REFRESH_LIVE_PLUGIN
    "Refresh the live user plugin when building from its source directory" ON)

# Binary Ninja install (override with -DBN_INSTALL_DIR=...)
if(NOT BN_INSTALL_DIR)
    if(APPLE)
        set(BN_INSTALL_DIR "/Applications/Binary Ninja Personal Dev/Binary Ninja.app")
    elseif(WIN32)
        set(BN_INSTALL_DIR "$ENV{ProgramFiles}/Vector35/BinaryNinja")
    else()
        set(BN_INSTALL_DIR "/opt/binaryninja")
    endif()
endif()

# Prefer the matching local Qt SDK installed by the documented aqt command.
# This avoids silently compiling against a different Homebrew Qt minor version.
set(BINJA_DIFF_QT_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/toolchain/qt-6.11.1/6.11.1/macos"
    CACHE PATH "Qt SDK matching Binary Ninja's bundled Qt runtime")
if(EXISTS "${BINJA_DIFF_QT_ROOT}/lib/cmake/Qt6/Qt6Config.cmake")
    list(PREPEND CMAKE_PREFIX_PATH "${BINJA_DIFF_QT_ROOT}")
endif()
find_package(Qt6 COMPONENTS Core REQUIRED)
if(BINJA_DIFF_BUILD_UI_PLUGIN)
    find_package(Qt6 COMPONENTS Gui Widgets Concurrent REQUIRED)
endif()

# Build the bundled Rust matching engine. Keeping its target directory inside
# this build tree makes the plugin independent of the deprecated rust_diff
# plugin while preserving the exact same C ABI and algorithms.
find_program(CARGO_EXECUTABLE cargo REQUIRED)
if(APPLE)
    set(RUST_ENGINE_FILENAME "librust_diff.dylib")
elseif(WIN32)
    set(RUST_ENGINE_FILENAME "rust_diff.dll")
else()
    set(RUST_ENGINE_FILENAME "librust_diff.so")
endif()
set(RUST_ENGINE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/rust_engine/Cargo.toml")
set(RUST_ENGINE_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/rust-engine-target")
set(RUST_ENGINE_LIBRARY "${RUST_ENGINE_TARGET_DIR}/release/${RUST_ENGINE_FILENAME}")
file(GLOB_RECURSE RUST_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/rust_engine/src/*.rs")
add_custom_command(
    OUTPUT "${RUST_ENGINE_LIBRARY}"
    COMMAND "${CMAKE_COMMAND}" -E env
        "CARGO_TARGET_DIR=${RUST_ENGINE_TARGET_DIR}"
        "${CARGO_EXECUTABLE}" build --locked --release
        --manifest-path "${RUST_ENGINE_MANIFEST}"
    DEPENDS
        "${RUST_ENGINE_MANIFEST}"
        "${CMAKE_CURRENT_SOURCE_DIR}/rust_engine/Cargo.lock"
        ${RUST_ENGINE_SOURCES}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/rust_engine"
    COMMENT "Building bundled rust_diff engine"
    VERBATIM)
add_custom_target(binja_diff_rust_engine ALL DEPENDS "${RUST_ENGINE_LIBRARY}")

if(BINJA_DIFF_BUILD_UI_PLUGIN)
  add_library(binja_diff_view SHARED
	src/corpusmodel.cpp
	src/corpusmodel.h
	src/corpusengine.cpp
	src/corpusmetrics.cpp
	src/corpusengine.h
	src/corpuspersistence.cpp
	src/corpuspersistence.h
	src/corpusview.cpp
	src/corpusview.h
	src/patchsignature.cpp
	src/patchsignature.h
	src/securityrelevance.cpp
    src/diffengine.cpp
    src/diffengine.h
	src/symbolport.cpp
	src/symbolport.h
    src/diffcontroller.cpp
    src/diffcontroller.h
    src/diffview.cpp
    src/diffview.h
	src/diffresultspanel.cpp
	src/diffresultspanel.h
    src/diffrenderlayer.cpp
    src/diffrenderlayer.h)
  add_dependencies(binja_diff_view binja_diff_rust_engine)
  # The Rust engine is loaded dynamically rather than linked, so a normal target
  # dependency alone does not relink the native plugin when only Rust sources
  # change. Make the engine an explicit link dependency so the post-build staging
  # and live-install copy cannot leave an older private dylib behind.
  set_property(TARGET binja_diff_view APPEND PROPERTY LINK_DEPENDS
      "${RUST_ENGINE_LIBRARY}")

  # Keep the engine beside the UI plugin in the build tree. DiffEngine searches
  # this location for in-place development; installation moves the engine into a
  # private resource directory that Binary Ninja does not scan as a plugin.
  add_custom_command(TARGET binja_diff_view POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${RUST_ENGINE_LIBRARY}" "$<TARGET_FILE_DIR:binja_diff_view>/${RUST_ENGINE_FILENAME}")
endif()

# Native plugins must use the API revision matching the installed core ABI.
# By default, derive the exact upstream tag from the application bundle and
# keep that checkout in FetchContent's build-local cache. BN_API_PATH remains
# available for offline builds and API development checkouts.
set(BN_API_PATH "" CACHE PATH "Use an existing Binary Ninja API checkout")
set(BINJA_DIFF_BN_API_TAG "" CACHE STRING
    "Override the automatically selected Binary Ninja API tag")
set(BINJA_DIFF_BN_API_ADDED FALSE)

if(BN_API_PATH)
    if(NOT IS_ABSOLUTE "${BN_API_PATH}")
        get_filename_component(BN_API_PATH "${BN_API_PATH}" ABSOLUTE
            BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
else()
    set(BINJA_DIFF_BN_PRODUCT_VERSION "")
    if(APPLE)
        set(BINJA_DIFF_BN_INFO_PLIST "${BN_INSTALL_DIR}/Contents/Info.plist")
        if(EXISTS "${BINJA_DIFF_BN_INFO_PLIST}")
            # Reconfigure automatically when an in-place Binary Ninja update
            # replaces the bundle metadata with a new build number.
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${BINJA_DIFF_BN_INFO_PLIST}")
            execute_process(
                COMMAND /usr/bin/plutil -extract CFBundleShortVersionString raw
                    -o - "${BINJA_DIFF_BN_INFO_PLIST}"
                OUTPUT_VARIABLE BINJA_DIFF_BN_PRODUCT_VERSION
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
        endif()
    elseif(NOT BINJA_DIFF_BN_API_TAG)
        # Preserve the traditional adjacent-checkout default on platforms
        # where the installed product version is not exposed via a plist.
        get_filename_component(BN_API_PATH "../binaryninja-api" ABSOLUTE)
    endif()

    if(NOT BN_API_PATH)
        if(BINJA_DIFF_BN_API_TAG)
            set(BINJA_DIFF_RESOLVED_BN_API_TAG "${BINJA_DIFF_BN_API_TAG}")
        elseif(BINJA_DIFF_BN_PRODUCT_VERSION MATCHES
                "^([0-9]+\\.[0-9]+\\.[0-9]+)")
            set(BINJA_DIFF_BN_BUILD_VERSION "${CMAKE_MATCH_1}")
            if(BINJA_DIFF_BN_PRODUCT_VERSION MATCHES "(^|[-_])dev($|[-_])")
                set(BINJA_DIFF_BN_API_CHANNEL "dev")
            else()
                set(BINJA_DIFF_BN_API_CHANNEL "stable")
            endif()
            set(BINJA_DIFF_RESOLVED_BN_API_TAG
                "${BINJA_DIFF_BN_API_CHANNEL}/${BINJA_DIFF_BN_BUILD_VERSION}")
        else()
            message(FATAL_ERROR
                "Could not determine the installed Binary Ninja build from "
                "${BN_INSTALL_DIR}. Set BN_API_PATH to a matching local checkout "
                "or BINJA_DIFF_BN_API_TAG to an exact upstream tag.")
        endif()

        message(STATUS
            "Using Binary Ninja API ${BINJA_DIFF_RESOLVED_BN_API_TAG} "
            "for installed build ${BINJA_DIFF_BN_PRODUCT_VERSION}")
        include(FetchContent)
        FetchContent_Declare(binja_diff_binaryninja_api
            GIT_REPOSITORY https://github.com/Vector35/binaryninja-api.git
            GIT_TAG "${BINJA_DIFF_RESOLVED_BN_API_TAG}"
            GIT_SHALLOW TRUE
            GIT_PROGRESS TRUE
            GIT_SUBMODULES vendor/fmt
            GIT_SUBMODULES_RECURSE FALSE)
        FetchContent_MakeAvailable(binja_diff_binaryninja_api)
        set(BN_API_PATH "${binja_diff_binaryninja_api_SOURCE_DIR}")
        set(BINJA_DIFF_BN_API_ADDED TRUE)
    endif()
endif()

if(NOT EXISTS "${BN_API_PATH}/binaryninjaapi.h")
    message(FATAL_ERROR "Binary Ninja API not found at ${BN_API_PATH}")
endif()
execute_process(
	COMMAND git tag --points-at HEAD --list "dev/*" --sort=-version:refname
	WORKING_DIRECTORY "${BN_API_PATH}"
	OUTPUT_VARIABLE BINJA_DIFF_BN_API_TAG_LIST
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET)
if(BINJA_DIFF_BN_API_TAG_LIST)
	string(REPLACE "\n" ";" BINJA_DIFF_BN_API_TAGS "${BINJA_DIFF_BN_API_TAG_LIST}")
	list(GET BINJA_DIFF_BN_API_TAGS 0 BINJA_DIFF_BN_API_TAG)
endif()
if(NOT BINJA_DIFF_BN_API_ADDED)
    add_subdirectory("${BN_API_PATH}" api)
endif()

# Older API revisions can vendor fmt releases whose C++20 consteval checks fail
# under newer AppleClang versions. The Binary Ninja API itself uses C++17 at
# those revisions, so compile only its fmt target at the same language level.
file(STRINGS "${BN_API_PATH}/binaryninjacore.h" BINJA_DIFF_BN_ABI_LINE
	REGEX "^#define BN_CURRENT_CORE_ABI_VERSION [0-9]+" LIMIT_COUNT 1)
string(REGEX MATCH "[0-9]+$" BINJA_DIFF_BN_ABI_VERSION
	"${BINJA_DIFF_BN_ABI_LINE}")
if(TARGET fmt AND BINJA_DIFF_BN_ABI_VERSION LESS 180)
	set_property(TARGET fmt PROPERTY CXX_STANDARD 17)
	message(STATUS
		"Using C++17 for the legacy Binary Ninja API fmt target "
		"(core ABI ${BINJA_DIFF_BN_ABI_VERSION})")
endif()

# The C++ API can compile successfully against headers newer than the installed
# core and then fail much later with opaque undefined symbols. Probe the ABI at
# configure time so developers get an immediate, actionable diagnosis.
if(NOT CMAKE_CROSSCOMPILING AND CORE_LIBRARY)
	try_run(BINJA_DIFF_ABI_RUN_RESULT BINJA_DIFF_ABI_COMPILE_RESULT
		"${CMAKE_CURRENT_BINARY_DIR}/bn-abi-probe"
		"${CMAKE_CURRENT_SOURCE_DIR}/tests/check_bn_abi.cpp"
		CMAKE_FLAGS
			"-DINCLUDE_DIRECTORIES=${BN_API_PATH}"
		LINK_LIBRARIES "${CORE_LIBRARY}"
		RUN_OUTPUT_VARIABLE BINJA_DIFF_ABI_OUTPUT
		COMPILE_OUTPUT_VARIABLE BINJA_DIFF_ABI_COMPILE_OUTPUT)
	if(NOT BINJA_DIFF_ABI_COMPILE_RESULT)
		message(FATAL_ERROR
			"Could not compile the Binary Ninja ABI compatibility probe.\n"
			"API checkout: ${BN_API_PATH}\nCore library: ${CORE_LIBRARY}\n"
			"${BINJA_DIFF_ABI_COMPILE_OUTPUT}")
	elseif(NOT BINJA_DIFF_ABI_RUN_RESULT EQUAL 0)
		string(STRIP "${BINJA_DIFF_ABI_OUTPUT}" BINJA_DIFF_ABI_OUTPUT)
		message(FATAL_ERROR
			"Binary Ninja API/core ABI mismatch (${BINJA_DIFF_ABI_OUTPUT}).\n"
			"Verify BN_INSTALL_DIR and remove any stale BN_API_PATH or "
			"BINJA_DIFF_BN_API_TAG override, then reconfigure.")
	endif()
endif()

if(BINJA_DIFF_BUILD_UI_PLUGIN)
  target_include_directories(binja_diff_view PRIVATE ${BN_API_PATH}/vendor)
  target_link_libraries(binja_diff_view binaryninjaui Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent)
  target_compile_options(binja_diff_view PRIVATE
      $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wno-unused-parameter;-Wno-ignored-qualifiers>)

# Match the Rust engine's release-time whole-program optimization on the native
# UI side. This affects only Release builds and is enabled only when the active
# compiler/linker pair proves support during configuration.
  include(CheckIPOSupported)
  check_ipo_supported(RESULT BINJA_DIFF_IPO_SUPPORTED
      OUTPUT BINJA_DIFF_IPO_ERROR LANGUAGES CXX)
  if(BINJA_DIFF_IPO_SUPPORTED)
      set_property(TARGET binja_diff_view PROPERTY
          INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  else()
      message(STATUS "Native IPO/LTO unavailable: ${BINJA_DIFF_IPO_ERROR}")
  endif()

  set_target_properties(binja_diff_view PROPERTIES
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON
      POSITION_INDEPENDENT_CODE ON
      PREFIX "")

# The plugin must bind to Binary Ninja's bundled Qt frameworks at runtime, not
# the Qt development package used for compilation. Build with the final Binary
# Ninja rpath from the outset so CMake installation does not try to rewrite a
# post-processed Mach-O file.
  if(APPLE)
	  set_target_properties(binja_diff_view PROPERTIES
		  BUILD_WITH_INSTALL_RPATH TRUE
		  INSTALL_RPATH "${BN_INSTALL_DIR}/Contents/MacOS;${BN_INSTALL_DIR}/Contents/Frameworks")
	  add_custom_command(TARGET binja_diff_view POST_BUILD
		  COMMAND codesign -f -s - $<TARGET_FILE:binja_diff_view>)
  endif()

# This repository is commonly checked out directly as
#   <Binary Ninja user plugins>/binja_diff_view/
# while Binary Ninja loads the native plugin from the parent directory. In that
# development layout, a successful build must also refresh the live install;
# otherwise restarting Binary Ninja silently loads an older top-level dylib.
# Builds in every other location retain the normal explicit `cmake --install`
# workflow and never write outside their build tree.
get_filename_component(BINJA_DIFF_SOURCE_PARENT
	"${CMAKE_CURRENT_SOURCE_DIR}/.." REALPATH)
get_filename_component(BINJA_DIFF_USER_PLUGIN_ROOT
	"${BN_USER_PLUGINS_DIR}" REALPATH)
  if(BINJA_DIFF_REFRESH_LIVE_PLUGIN AND
	BINJA_DIFF_SOURCE_PARENT STREQUAL BINJA_DIFF_USER_PLUGIN_ROOT)
	message(STATUS
		"Binja Diff source is inside the Binary Ninja user plugin directory; "
		"successful builds will refresh the live plugin automatically")
	if(APPLE)
	  # Replacing a loaded dylib in place can leave dyld with stale signed pages.
	  # Rename the fully copied file so existing readers retain the old inode.
	  add_custom_command(TARGET binja_diff_view POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"${BN_USER_PLUGINS_DIR}/.binja_diff_view"
		COMMAND "${CMAKE_COMMAND}" -E copy
			"${RUST_ENGINE_LIBRARY}"
			"${BN_USER_PLUGINS_DIR}/.binja_diff_view/${RUST_ENGINE_FILENAME}.next"
		COMMAND "${CMAKE_COMMAND}" -E rename
			"${BN_USER_PLUGINS_DIR}/.binja_diff_view/${RUST_ENGINE_FILENAME}.next"
			"${BN_USER_PLUGINS_DIR}/.binja_diff_view/${RUST_ENGINE_FILENAME}"
		COMMAND "${CMAKE_COMMAND}" -E copy
			"$<TARGET_FILE:binja_diff_view>"
			"${BN_USER_PLUGINS_DIR}/$<TARGET_FILE_NAME:binja_diff_view>.next"
		COMMAND "${CMAKE_COMMAND}" -E rename
			"${BN_USER_PLUGINS_DIR}/$<TARGET_FILE_NAME:binja_diff_view>.next"
			"${BN_USER_PLUGINS_DIR}/$<TARGET_FILE_NAME:binja_diff_view>"
		COMMENT "Refreshing live Binary Ninja plugin installation"
		VERBATIM)
	else()
	  add_custom_command(TARGET binja_diff_view POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"${BN_USER_PLUGINS_DIR}/.binja_diff_view"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"$<TARGET_FILE:binja_diff_view>"
			"${BN_USER_PLUGINS_DIR}/$<TARGET_FILE_NAME:binja_diff_view>"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"${RUST_ENGINE_LIBRARY}"
			"${BN_USER_PLUGINS_DIR}/.binja_diff_view/${RUST_ENGINE_FILENAME}"
		COMMENT "Refreshing live Binary Ninja plugin installation"
		VERBATIM)
	endif()
  elseif(NOT BINJA_DIFF_REFRESH_LIVE_PLUGIN)
	message(STATUS
		"Live plugin refresh is disabled; build outputs will remain in this build tree")
  endif()

  # Keep the FFI-only Rust library out of the top-level plugin directory. Binary
  # Ninja treats every native library there as an independently loadable plugin.
  # macOS must replace signed dylibs by rename during explicit installation too.
  if(APPLE)
    install(CODE "
      include(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/atomic_install.cmake\")
      binja_diff_atomic_install(\"${RUST_ENGINE_LIBRARY}\"
        \"${BN_USER_PLUGINS_DIR}/.binja_diff_view/${RUST_ENGINE_FILENAME}\")
      binja_diff_atomic_install(\"$<TARGET_FILE:binja_diff_view>\"
        \"${BN_USER_PLUGINS_DIR}/$<TARGET_FILE_NAME:binja_diff_view>\")")
  else()
    bn_install_plugin(binja_diff_view)
    install(PROGRAMS "${RUST_ENGINE_LIBRARY}"
        DESTINATION "${BN_USER_PLUGINS_DIR}/.binja_diff_view")
  endif()
else()
  message(STATUS "UI plugin build is disabled; configuring headless tests only")
endif()

include(CTest)
option(BINJA_DIFF_RUN_INTEGRATION_TESTS
    "Build and register licensed Binary Ninja integration tests" OFF)
if(BUILD_TESTING)
	add_executable(memcpy_safe_fixture EXCLUDE_FROM_ALL tests/fixtures/memcpy_fixture.c)
	add_executable(memcpy_bug_fixture_unstripped EXCLUDE_FROM_ALL tests/fixtures/memcpy_fixture.c)
	target_compile_definitions(memcpy_bug_fixture_unstripped PRIVATE MEMCPY_BUG=1)
	foreach(fixture memcpy_safe_fixture memcpy_bug_fixture_unstripped)
		target_compile_options(${fixture} PRIVATE -O1 -g -fno-inline -fno-builtin-memcpy)
	endforeach()
	# Retain local symbols for fixture ground truth, but prevent debug imports
	# from restoring those names in the stripped copy used by the porting test.
	target_compile_options(memcpy_bug_fixture_unstripped PRIVATE -g0)
	set(MEMCPY_BUG_STRIPPED "${CMAKE_CURRENT_BINARY_DIR}/memcpy_bug_fixture.stripped")
	if(APPLE)
		set(BINJA_DIFF_STRIP_ARGS -S -x)
	else()
		set(BINJA_DIFF_STRIP_ARGS --strip-all)
	endif()
	add_custom_command(
		OUTPUT "${MEMCPY_BUG_STRIPPED}"
		COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:memcpy_bug_fixture_unstripped>"
			"${MEMCPY_BUG_STRIPPED}"
		COMMAND "${CMAKE_STRIP}" ${BINJA_DIFF_STRIP_ARGS} "${MEMCPY_BUG_STRIPPED}"
		DEPENDS memcpy_bug_fixture_unstripped
		COMMENT "Creating stripped memcpy regression fixture"
		VERBATIM)
	add_custom_target(memcpy_bug_fixture DEPENDS "${MEMCPY_BUG_STRIPPED}")
	if(BINJA_DIFF_RUN_INTEGRATION_TESTS)
		add_test(NAME binja_diff_fixture_symbol_tests
			COMMAND "${CMAKE_COMMAND}"
				"-DSYMBOLIZED=$<TARGET_FILE:memcpy_safe_fixture>"
				"-DSTRIPPED=${MEMCPY_BUG_STRIPPED}"
				"-DNM_TOOL=${CMAKE_NM}"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_fixture_symbols.cmake")
		set_tests_properties(binja_diff_fixture_symbol_tests PROPERTIES LABELS "integration")
	endif()
	add_executable(corpus_reference_fixture EXCLUDE_FROM_ALL tests/fixtures/corpus_fixture.c)
	add_executable(corpus_changed_fixture EXCLUDE_FROM_ALL tests/fixtures/corpus_fixture.c)
	add_executable(corpus_added_fixture EXCLUDE_FROM_ALL tests/fixtures/corpus_fixture.c)
	add_executable(corpus_removed_fixture EXCLUDE_FROM_ALL tests/fixtures/corpus_fixture.c)
	add_executable(corpus_renamed_fixture EXCLUDE_FROM_ALL tests/fixtures/corpus_fixture.c)
	add_executable(corpus_vulnerable_fixture EXCLUDE_FROM_ALL tests/fixtures/corpus_fixture.c)
	add_executable(corpus_fixed_fixture EXCLUDE_FROM_ALL tests/fixtures/corpus_fixture.c)
	add_executable(corpus_independent_fixture EXCLUDE_FROM_ALL tests/fixtures/corpus_fixture.c)
	add_executable(corpus_unrelated_fixture EXCLUDE_FROM_ALL tests/fixtures/corpus_fixture.c)
	add_executable(corpus_larger_fixture EXCLUDE_FROM_ALL tests/fixtures/corpus_fixture.c)
	target_compile_definitions(corpus_changed_fixture PRIVATE CORPUS_VARIANT=1)
	target_compile_definitions(corpus_added_fixture PRIVATE CORPUS_VARIANT=2)
	target_compile_definitions(corpus_removed_fixture PRIVATE CORPUS_VARIANT=3)
	target_compile_definitions(corpus_renamed_fixture PRIVATE CORPUS_VARIANT=4)
	target_compile_definitions(corpus_vulnerable_fixture PRIVATE CORPUS_VARIANT=5)
	target_compile_definitions(corpus_fixed_fixture PRIVATE CORPUS_VARIANT=6)
	target_compile_definitions(corpus_independent_fixture PRIVATE CORPUS_VARIANT=7)
	target_compile_definitions(corpus_unrelated_fixture PRIVATE CORPUS_VARIANT=8)
	target_compile_definitions(corpus_larger_fixture PRIVATE CORPUS_VARIANT=9)
	foreach(fixture corpus_reference_fixture corpus_changed_fixture corpus_added_fixture
		corpus_removed_fixture corpus_renamed_fixture corpus_vulnerable_fixture
		corpus_fixed_fixture corpus_independent_fixture corpus_unrelated_fixture
		corpus_larger_fixture)
		target_compile_options(${fixture} PRIVATE -O1 -g -fno-inline)
	endforeach()
	add_test(NAME binja_diff_corpus_fixture_symbol_tests
		COMMAND "${CMAKE_COMMAND}"
			"-DREFERENCE=$<TARGET_FILE:corpus_reference_fixture>"
			"-DCHANGED=$<TARGET_FILE:corpus_changed_fixture>"
			"-DADDED=$<TARGET_FILE:corpus_added_fixture>"
			"-DREMOVED=$<TARGET_FILE:corpus_removed_fixture>"
			"-DRENAMED=$<TARGET_FILE:corpus_renamed_fixture>"
			"-DVULNERABLE=$<TARGET_FILE:corpus_vulnerable_fixture>"
			"-DFIXED=$<TARGET_FILE:corpus_fixed_fixture>"
			"-DINDEPENDENT=$<TARGET_FILE:corpus_independent_fixture>"
			"-DUNRELATED=$<TARGET_FILE:corpus_unrelated_fixture>"
			"-DLARGER=$<TARGET_FILE:corpus_larger_fixture>"
			"-DNM_TOOL=${CMAKE_NM}"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_corpus_fixture_symbols.cmake")

    add_executable(binja_diff_model_tests tests/diffmodel_tests.cpp)
    add_executable(binja_diff_weighted_cache_tests tests/weightedcache_tests.cpp)
    target_include_directories(binja_diff_weighted_cache_tests PRIVATE src)
    add_test(NAME binja_diff_weighted_cache_tests COMMAND binja_diff_weighted_cache_tests)
    add_executable(binja_diff_shared_result_cache_tests tests/sharedresultcache_tests.cpp)
    target_include_directories(binja_diff_shared_result_cache_tests PRIVATE src)
    find_package(Threads REQUIRED)
    add_executable(binja_diff_keyed_work_gate_tests tests/keyedworkgate_tests.cpp)
    target_include_directories(binja_diff_keyed_work_gate_tests PRIVATE src)
    target_link_libraries(binja_diff_keyed_work_gate_tests PRIVATE Threads::Threads)
    add_test(NAME binja_diff_keyed_work_gate_tests COMMAND binja_diff_keyed_work_gate_tests)
    set_tests_properties(binja_diff_keyed_work_gate_tests PROPERTIES TIMEOUT 10)
    target_link_libraries(binja_diff_shared_result_cache_tests PRIVATE Threads::Threads)
    add_test(NAME binja_diff_shared_result_cache_tests COMMAND binja_diff_shared_result_cache_tests)
    add_executable(binja_diff_disk_cache_tests tests/diskcache_tests.cpp)
    target_include_directories(binja_diff_disk_cache_tests PRIVATE src)
    target_link_libraries(binja_diff_disk_cache_tests PRIVATE Qt6::Core)
    add_test(NAME binja_diff_disk_cache_tests COMMAND binja_diff_disk_cache_tests)
    target_include_directories(binja_diff_model_tests PRIVATE src)
    target_compile_options(binja_diff_model_tests PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>)
	add_test(NAME binja_diff_model_tests COMMAND binja_diff_model_tests)
	add_executable(binja_diff_corpus_model_tests
		tests/corpusmodel_tests.cpp
		src/corpusmodel.cpp)
	target_include_directories(binja_diff_corpus_model_tests PRIVATE src)
	target_compile_options(binja_diff_corpus_model_tests PRIVATE
		$<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic>)
	add_test(NAME binja_diff_corpus_model_tests COMMAND binja_diff_corpus_model_tests)
	add_dependencies(binja_diff_corpus_model_tests
		corpus_reference_fixture corpus_changed_fixture corpus_added_fixture
		corpus_removed_fixture corpus_renamed_fixture corpus_vulnerable_fixture
		corpus_fixed_fixture corpus_independent_fixture corpus_unrelated_fixture
		corpus_larger_fixture)
	add_executable(binja_diff_corpus_persistence_tests
		tests/corpuspersistence_tests.cpp
		src/corpuspersistence.cpp
		src/corpusmodel.cpp
		src/securityrelevance.cpp)
	target_include_directories(binja_diff_corpus_persistence_tests PRIVATE
		src ${BN_API_PATH}/vendor)
	target_link_libraries(binja_diff_corpus_persistence_tests
		binaryninjaapi Qt6::Core)
	target_compile_options(binja_diff_corpus_persistence_tests PRIVATE
		$<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wno-unused-parameter;-Wno-ignored-qualifiers>)
	add_test(NAME binja_diff_corpus_persistence_tests
		COMMAND binja_diff_corpus_persistence_tests)
	add_executable(binja_diff_corpus_metrics_tests
		tests/corpusmetrics_tests.cpp src/corpusmetrics.cpp src/corpusmodel.cpp)
	target_include_directories(binja_diff_corpus_metrics_tests PRIVATE src ${BN_API_PATH}/vendor)
	target_link_libraries(binja_diff_corpus_metrics_tests PRIVATE binaryninjaapi Qt6::Core)
	add_test(NAME binja_diff_corpus_metrics_tests COMMAND binja_diff_corpus_metrics_tests)
	add_executable(binja_diff_token_emphasis_tests tests/tokenemphasis_tests.cpp)
	target_include_directories(binja_diff_token_emphasis_tests PRIVATE src)
	target_link_libraries(binja_diff_token_emphasis_tests PRIVATE binaryninjaapi)
	add_test(NAME binja_diff_token_emphasis_tests COMMAND binja_diff_token_emphasis_tests)
	set_tests_properties(binja_diff_token_emphasis_tests PROPERTIES LABELS "unit")
	add_executable(binja_diff_export_benchmark EXCLUDE_FROM_ALL
		tests/export_benchmark.cpp src/corpuspersistence.cpp
		src/corpusmodel.cpp src/securityrelevance.cpp)
	target_include_directories(binja_diff_export_benchmark PRIVATE src ${BN_API_PATH}/vendor)
	target_link_libraries(binja_diff_export_benchmark PRIVATE binaryninjaapi Qt6::Core)
	add_test(NAME binja_diff_rust_engine_tests
		COMMAND "${CMAKE_COMMAND}" -E env
			"CARGO_TARGET_DIR=${RUST_ENGINE_TARGET_DIR}"
			"${CARGO_EXECUTABLE}" test --locked --lib
			--manifest-path "${RUST_ENGINE_MANIFEST}")
	set_tests_properties(binja_diff_rust_engine_tests PROPERTIES
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/rust_engine"
		TIMEOUT 180)

    set_tests_properties(
        binja_diff_corpus_fixture_symbol_tests binja_diff_model_tests binja_diff_weighted_cache_tests binja_diff_disk_cache_tests binja_diff_shared_result_cache_tests
        binja_diff_corpus_model_tests binja_diff_corpus_persistence_tests binja_diff_corpus_metrics_tests
        binja_diff_rust_engine_tests PROPERTIES LABELS "unit")
    set_tests_properties(binja_diff_keyed_work_gate_tests PROPERTIES LABELS "unit")

    if(BINJA_DIFF_RUN_INTEGRATION_TESTS)
		add_executable(binja_diff_symbol_patch_integration_tests
			tests/symbol_patch_integration_tests.cpp
			src/diffengine.cpp
			src/corpusmodel.cpp
			src/patchsignature.cpp
			src/securityrelevance.cpp
			src/symbolport.cpp)
		target_include_directories(binja_diff_symbol_patch_integration_tests PRIVATE
			src ${BN_API_PATH}/vendor)
		target_link_libraries(binja_diff_symbol_patch_integration_tests
			binaryninjaapi Qt6::Core)
		target_compile_options(binja_diff_symbol_patch_integration_tests PRIVATE
			$<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wno-unused-parameter;-Wno-ignored-qualifiers>)
		if(BINJA_DIFF_BN_API_TAG)
			target_compile_definitions(binja_diff_symbol_patch_integration_tests PRIVATE
				BINJA_DIFF_BN_API_TAG="${BINJA_DIFF_BN_API_TAG}")
		endif()
		add_dependencies(binja_diff_symbol_patch_integration_tests
			binja_diff_rust_engine memcpy_safe_fixture memcpy_bug_fixture)
		if(APPLE)
			set_target_properties(binja_diff_symbol_patch_integration_tests PROPERTIES
				BUILD_RPATH "${BN_INSTALL_DIR}/Contents/MacOS")
		endif()
		add_test(NAME binja_diff_symbol_patch_integration_tests
			COMMAND "${CMAKE_COMMAND}" -E env
				"BINJA_DIFF_ENGINE_PATH=${RUST_ENGINE_LIBRARY}"
				"BINJA_DIFF_CACHE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-cache"
				"$<TARGET_FILE:binja_diff_symbol_patch_integration_tests>"
				"$<TARGET_FILE:memcpy_safe_fixture>"
				"${MEMCPY_BUG_STRIPPED}"
				"$<TARGET_FILE:memcpy_bug_fixture_unstripped>"
				"${CMAKE_CURRENT_BINARY_DIR}/symbol_port_target.bndb")
		set_tests_properties(binja_diff_symbol_patch_integration_tests PROPERTIES
			SKIP_RETURN_CODE 77
			TIMEOUT 180)

        set_tests_properties(binja_diff_symbol_patch_integration_tests
            PROPERTIES LABELS "integration")
    endif()

	add_executable(binja_diff_historical_corpus_headless EXCLUDE_FROM_ALL
		tests/historical_corpus_headless.cpp
		src/diffengine.cpp
		src/corpusmodel.cpp)
	target_include_directories(binja_diff_historical_corpus_headless PRIVATE
		src ${BN_API_PATH}/vendor)
	target_link_libraries(binja_diff_historical_corpus_headless
		binaryninjaapi Qt6::Core)
	target_compile_options(binja_diff_historical_corpus_headless PRIVATE
		$<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wno-unused-parameter;-Wno-ignored-qualifiers>)
	add_dependencies(binja_diff_historical_corpus_headless binja_diff_rust_engine)
	if(APPLE)
		set_target_properties(binja_diff_historical_corpus_headless PROPERTIES
			BUILD_RPATH "${BN_INSTALL_DIR}/Contents/MacOS")
	endif()

	add_executable(binja_diff_curl_vulnerability_headless EXCLUDE_FROM_ALL
		tests/curl_vulnerability_headless.cpp
		src/diffengine.cpp)
	target_include_directories(binja_diff_curl_vulnerability_headless PRIVATE
		src ${BN_API_PATH}/vendor)
	target_link_libraries(binja_diff_curl_vulnerability_headless
		binaryninjaapi Qt6::Core)
	target_compile_options(binja_diff_curl_vulnerability_headless PRIVATE
		$<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wno-unused-parameter;-Wno-ignored-qualifiers>)
	add_dependencies(binja_diff_curl_vulnerability_headless binja_diff_rust_engine)
	if(APPLE)
		set_target_properties(binja_diff_curl_vulnerability_headless PROPERTIES
			BUILD_RPATH "${BN_INSTALL_DIR}/Contents/MacOS")
	endif()
endif()
