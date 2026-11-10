include(${HELIX_SOURCE_ROOT}/cmake/HelixProtobuf.cmake)
set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${_generated}")
set(_schema "${HELIX_SOURCE_ROOT}/protos/helix_mmo.proto")
add_custom_command(
	OUTPUT "${_generated}/helix_mmo.pb.cc" "${_generated}/helix_mmo.pb.h"
	COMMAND protobuf::protoc --proto_path=${HELIX_SOURCE_ROOT}/protos --cpp_out=${_generated} ${_schema}
	DEPENDS "${_schema}" protobuf::protoc
	VERBATIM)
add_library(helix_protocol STATIC src/Codec.cpp src/Messages.cpp src/ProtobufSerializer.cpp src/SerializerRegistry.cpp "${_generated}/helix_mmo.pb.cc" "${_generated}/helix_mmo.pb.h")
add_library(helix::protocol ALIAS helix_protocol)
set_target_properties(helix_protocol PROPERTIES EXPORT_NAME protocol)
target_compile_features(helix_protocol PUBLIC cxx_std_20)
target_link_libraries(helix_protocol PUBLIC protobuf::libprotobuf)
target_include_directories(helix_protocol PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
	$<BUILD_INTERFACE:${_generated}>
	$<INSTALL_INTERFACE:include>)
if(MSVC)
	target_compile_options(helix_protocol PUBLIC /utf-8)
endif()
install(TARGETS helix_protocol EXPORT helixTargets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
install(FILES "${_generated}/helix_mmo.pb.h" DESTINATION include)
install(FILES "${_schema}" DESTINATION share/helix/protos)
if(HELIX_ENABLE_TESTS)
	add_executable(helix_protocol_test tests/protocol_test.cpp)
	target_link_libraries(helix_protocol_test PRIVATE helix_protocol)
	add_test(NAME protocol-buffers COMMAND helix_protocol_test)
	if(NOT HELIX_BUILD_FLATBUFFERS)
		add_executable(helix_protobuf_serializer_test tests/serializer_test.cpp)
		target_link_libraries(helix_protobuf_serializer_test PRIVATE helix_protocol)
		add_test(NAME packet-serializers COMMAND helix_protobuf_serializer_test)
	endif()
endif()
