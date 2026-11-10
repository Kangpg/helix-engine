# =============================================================================
# helix-engine 서버 엔진 — CMake 빌드
#
# include/ · ${HELIX_SOURCE_ROOT}/src/ · ${HELIX_SOURCE_ROOT}/samples/ 를 빌드한다.
#
# Windows IOCP / Linux epoll 코어. C#은 별도 관리 엔진 프로젝트로 빌드한다.
#
# 빌드:
#   cmake --preset windows-x64
#   cmake --build --preset windows-debug
# 산출물: build/windows-x64/.../helix_echo_phase1(.exe)
# =============================================================================

cmake_minimum_required(VERSION 3.21)
# C 도 활성화: KCP 원본 ikcp.c(ThirdParty/kcp)를 C로 컴파일하기 위함.
project(helix-engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 소스는 UTF-8(BOM 없음). MSVC가 cp949로 오인하지 않도록 명시(C4819 + 주석 깨짐 방지).
if(MSVC)
	add_compile_options(/utf-8)
endif()

# Phase 4부터 Linux(epoll) 빌드 지원. 플랫폼별 백엔드/링크는 아래에서 선택한다.
find_package(Threads REQUIRED)

# --- 엔진 코어 라이브러리 -----------------------------------------------------
set(HELIX_SOURCES
	${HELIX_SOURCE_ROOT}/src/core/Server.cpp
	${HELIX_SOURCE_ROOT}/src/core/ServerCore.cpp
	${HELIX_SOURCE_ROOT}/src/session/Session.cpp
	${HELIX_SOURCE_ROOT}/src/session/SessionManager.cpp
	${HELIX_SOURCE_ROOT}/src/net/NetBackendFactory.cpp
	${HELIX_SOURCE_ROOT}/src/net/Acceptor.cpp
	${HELIX_SOURCE_ROOT}/src/config/EngineConfig.cpp
	${HELIX_SOURCE_ROOT}/src/log/Log.cpp
	${HELIX_SOURCE_ROOT}/src/cluster/RespClient.cpp
	${HELIX_SOURCE_ROOT}/src/cluster/RedisBackplane.cpp
	${HELIX_SOURCE_ROOT}/src/cluster/BackplaneFactory.cpp
	${HELIX_SOURCE_ROOT}/src/transport/TransportFactory.cpp
	${HELIX_SOURCE_ROOT}/src/transport/kcp/KcpTransport.cpp
	${HELIX_SOURCE_ROOT}/src/core/TransportSession.cpp
	${HELIX_SOURCE_ROOT}/ThirdParty/kcp/ikcp.c          # KCP 원본(MIT). 사용자가 vendoring 해 둘 것.
	# ${HELIX_SOURCE_ROOT}/src/proto/FrameCodec.h  (header-only)
)

# 플랫폼별 I/O 백엔드 선택 (개방/폐쇄: 새 구현 추가만으로 확장).
if(WIN32)
	list(APPEND HELIX_SOURCES ${HELIX_SOURCE_ROOT}/src/net/WindowsIocpBackend.cpp
		${HELIX_SOURCE_ROOT}/src/net/WindowsRioBackend.cpp
		${HELIX_SOURCE_ROOT}/src/net/RioDataPlane.cpp
		${HELIX_SOURCE_ROOT}/src/net/RioBuffer.cpp)
else()
	list(APPEND HELIX_SOURCES ${HELIX_SOURCE_ROOT}/src/net/EpollBackend.cpp)          # epoll (Phase 4)
endif()

# 플랫폼별 링크/정의를 한 번에 적용하는 헬퍼.
function(helix_platform_setup tgt)
	if(WIN32)
		target_link_libraries(${tgt} PUBLIC ws2_32 mswsock)
		target_compile_definitions(${tgt} PUBLIC
			_WIN32_WINNT=0x0A00   # Windows 10+
			WIN32_LEAN_AND_MEAN
			NOMINMAX
			_CRT_SECURE_NO_WARNINGS)
	else()
		target_link_libraries(${tgt} PUBLIC Threads::Threads)
	endif()
endfunction()

set(HELIX_PUBLIC_HEADERS
	${HELIX_SOURCE_ROOT}/include/helix/helix.h
	${HELIX_SOURCE_ROOT}/include/helix/types.h
	${HELIX_SOURCE_ROOT}/include/helix/session.h
	${HELIX_SOURCE_ROOT}/include/helix/server_app.h
	${HELIX_SOURCE_ROOT}/include/helix/server.h
	${HELIX_SOURCE_ROOT}/include/helix/helix_c.h
	${HELIX_SOURCE_ROOT}/include/helix/engine_config.h
)

add_library(helix STATIC ${HELIX_SOURCES} ${HELIX_PUBLIC_HEADERS})

target_include_directories(helix
	PUBLIC
		$<BUILD_INTERFACE:${HELIX_SOURCE_ROOT}/include>
		$<INSTALL_INTERFACE:include>                          # 설치 후 소비자: <prefix>/include
	PRIVATE
		${HELIX_SOURCE_ROOT}/src
		${HELIX_SOURCE_ROOT}/ThirdParty/kcp
)
target_compile_features(helix PUBLIC cxx_std_20)
helix_platform_setup(helix)

if(HELIX_ENABLE_TESTS)
	add_executable(helix_session_partial_test ${HELIX_SOURCE_ROOT}/tests/session_partial_test.cpp)
	target_include_directories(helix_session_partial_test PRIVATE ${HELIX_SOURCE_ROOT}/src)
	target_link_libraries(helix_session_partial_test PRIVATE helix)
	add_test(NAME session-partial-send COMMAND helix_session_partial_test)
endif()

if(WIN32 AND HELIX_ENABLE_TESTS)
	add_executable(helix_windows_backend_test ${HELIX_SOURCE_ROOT}/tests/windows_backend_test.cpp)
	target_include_directories(helix_windows_backend_test PRIVATE ${HELIX_SOURCE_ROOT}/src)
	target_link_libraries(helix_windows_backend_test PRIVATE helix)
	add_test(NAME windows-iocp-rio COMMAND helix_windows_backend_test)
	set_tests_properties(windows-iocp-rio PROPERTIES TIMEOUT 30)
endif()

if(HELIX_BUILD_SAMPLES)
	# --- Phase 1 에코 샘플 (검증용 하니스) ---------------------------------------
	add_executable(helix_echo_phase1 ${HELIX_SOURCE_ROOT}/samples/echo_phase1/main.cpp)
	target_link_libraries(helix_echo_phase1 PRIVATE helix)

	# --- Phase 6 크로스노드 라우팅 샘플 (scale-out 검증) -------------------------
	add_executable(helix_route_phase6 ${HELIX_SOURCE_ROOT}/samples/route_phase6/main.cpp)
	target_link_libraries(helix_route_phase6 PRIVATE helix)

	# --- Phase 7 KCP 에코 클라이언트 (전송 검증) ---------------------------------
	# ikcp.c 는 helix(static)에 포함돼 있으므로 링크만으로 ikcp_* 심볼을 얻는다.
	# 이 테스트 클라이언트는 WinSock2 직접 사용 → Windows 전용(엔진과 무관).
	if(WIN32)
		add_executable(helix_kcp_echo_client ${HELIX_SOURCE_ROOT}/samples/kcp_echo_client/main.cpp)
		target_link_libraries(helix_kcp_echo_client PRIVATE helix)
		target_include_directories(helix_kcp_echo_client PRIVATE ${HELIX_SOURCE_ROOT}/ThirdParty/kcp)
	endif()

	# --- Phase 9 채팅 룸 샘플 (다세션 브로드캐스트 도메인 예제) -------------------
	add_executable(helix_chat_room ${HELIX_SOURCE_ROOT}/samples/chat_room/main.cpp)
	target_link_libraries(helix_chat_room PRIVATE helix)
endif()

# --- Phase 8: C ABI 공유 라이브러리 (helix.dll) — C#/타 언어 바인딩 대상 -------
# 엔진 전체 + helix_c.cpp 를 HELIX_BUILD_DLL 로 빌드해 helix_* 심볼을 export.
if(HELIX_BUILD_CAPI)
add_library(helix_capi SHARED ${HELIX_SOURCES} ${HELIX_PUBLIC_HEADERS} ${HELIX_SOURCE_ROOT}/src/capi/helix_c.cpp)
target_include_directories(helix_capi
	PUBLIC
		$<BUILD_INTERFACE:${HELIX_SOURCE_ROOT}/include>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		${HELIX_SOURCE_ROOT}/src
		${HELIX_SOURCE_ROOT}/ThirdParty/kcp
)
target_compile_features(helix_capi PUBLIC cxx_std_20)
helix_platform_setup(helix_capi)
target_compile_definitions(helix_capi PUBLIC
	HELIX_BUILD_DLL          # helix_c.h 의 HELIX_API → export (Win: __declspec / POSIX: visibility)
)
# DLL 런타임 이름은 helix.dll(= C#/P-Invoke 대상) 유지하되, import lib은 별도 이름으로.
# 정적 'helix' 타깃도 helix.lib 를 만들기 때문에, 둘이 같은 폴더에서 충돌하면
# 샘플이 C++ helix::Server 심볼을 못 찾고 링크 실패한다(import lib엔 C ABI만 export됨).
set_target_properties(helix_capi PROPERTIES
	RUNTIME_OUTPUT_NAME "helix"      # → helix.dll
	ARCHIVE_OUTPUT_NAME "helix_capi" # → helix_capi.lib (import lib, 충돌 회피)
)
endif()

message(STATUS "[Helix] C++ engine configured; optional C ABI: ${HELIX_BUILD_CAPI}")

# =============================================================================
# 설치 / export (Phase 9 상품화) — find_package(helix) 로 소비 가능하게.
#   설치:        cmake --install build/windows-x64 --config Release --prefix <dir>
#   소비자 측:   find_package(helix CONFIG REQUIRED); target_link_libraries(app PRIVATE helix::helix)
# =============================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(HELIX_INSTALL_TARGETS helix)
if(HELIX_BUILD_CAPI)
	list(APPEND HELIX_INSTALL_TARGETS helix_capi)
endif()
install(TARGETS ${HELIX_INSTALL_TARGETS}
	EXPORT helixTargets
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# 공개 헤더만 설치(${HELIX_SOURCE_ROOT}/include/helix/*). ${HELIX_SOURCE_ROOT}/src/ · ThirdParty/ 는 내부.
install(DIRECTORY ${HELIX_SOURCE_ROOT}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT helixTargets
	FILE helixTargets.cmake
	NAMESPACE helix::
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/helix)

write_basic_package_version_file(
	"${CMAKE_BINARY_DIR}/helixConfigVersion.cmake"
	VERSION ${PROJECT_VERSION}
	COMPATIBILITY SameMajorVersion)   # SemVer: 동일 major 내 호환

configure_package_config_file(
	"${HELIX_SOURCE_ROOT}/cmake/helixConfig.cmake.in"
	"${CMAKE_BINARY_DIR}/helixConfig.cmake"
	INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/helix)

install(FILES
	"${CMAKE_BINARY_DIR}/helixConfig.cmake"
	"${CMAKE_BINARY_DIR}/helixConfigVersion.cmake"
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/helix)

# 기본 설정 파일도 동봉(소비자가 바로 띄울 수 있게).
install(FILES ${HELIX_SOURCE_ROOT}/config/helix.toml
	DESTINATION ${CMAKE_INSTALL_DATADIR}/helix
	OPTIONAL)
