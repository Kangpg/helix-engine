if(NOT TARGET protobuf::libprotobuf)
	include(${HELIX_SOURCE_ROOT}/cmake/HelixProtobuf.cmake)
endif()
find_package(gRPC 1.85.0 EXACT CONFIG REQUIRED)
find_package(RdKafka 2.16.0 EXACT CONFIG REQUIRED)
set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_generated}")
set(_schema "${HELIX_SOURCE_ROOT}/protos/helix_services.proto")
add_custom_command(
	OUTPUT "${_generated}/helix_services.pb.cc" "${_generated}/helix_services.pb.h"
		"${_generated}/helix_services.grpc.pb.cc" "${_generated}/helix_services.grpc.pb.h"
	COMMAND protobuf::protoc --proto_path=${HELIX_SOURCE_ROOT}/protos
		--cpp_out=${_generated} --grpc_out=${_generated}
		--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin> ${_schema}
	DEPENDS "${_schema}" protobuf::protoc gRPC::grpc_cpp_plugin VERBATIM)
add_library(helix_services STATIC src/Event.cpp src/KafkaProducer.cpp src/KafkaConsumer.cpp src/EventRpc.cpp
	"${_generated}/helix_services.pb.cc" "${_generated}/helix_services.grpc.pb.cc")
add_library(helix::services ALIAS helix_services)
set_target_properties(helix_services PROPERTIES EXPORT_NAME services)
target_compile_features(helix_services PUBLIC cxx_std_20)
target_include_directories(helix_services PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
	PRIVATE "${_generated}")
target_link_libraries(helix_services PRIVATE gRPC::grpc++ protobuf::libprotobuf RdKafka::rdkafka)
if(MSVC)
	target_compile_options(helix_services PUBLIC /utf-8 PRIVATE /bigobj)
endif()
install(TARGETS helix_services EXPORT helixTargets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(FILES "${_schema}" DESTINATION share/helix/protos)
if(HELIX_BUILD_SAMPLES)
	add_executable(helix_service_demo samples/main.cpp)
	target_link_libraries(helix_service_demo PRIVATE helix_services helix)
endif()
if(HELIX_ENABLE_TESTS)
	add_executable(helix_services_test tests/services_test.cpp)
	target_link_libraries(helix_services_test PRIVATE helix_services helix)
	add_test(NAME service-contracts COMMAND helix_services_test)
	set_tests_properties(service-contracts PROPERTIES TIMEOUT 40)
	# Real-broker checks are run by tools/verify_services.py, not silently skipped by CTest.
	add_executable(helix_services_integration_test tests/integration_test.cpp)
	target_link_libraries(helix_services_integration_test PRIVATE helix_services helix)
endif()
