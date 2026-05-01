include(AMXConfig)
include(AddSAMPPlugin)


set(SAMP_SDK_ROOT "${PROJECT_SOURCE_DIR}/libs/sdk")
find_package(SAMPSDK REQUIRED)

set(DCC_CONFIG_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/config_headers")

configure_file(
	"../kook-connector.inc.in"
	"kook-connector.inc"
	@ONLY
)

configure_file(
	"version.hpp.in"
	"${DCC_CONFIG_INCLUDE_DIR}/version.hpp"
	@ONLY
)


add_samp_plugin(kook-connector 
	${SAMPSDK_DIR}/amxplugin.cpp
	${SAMPSDK_DIR}/amxplugin2.cpp
	Bot.cpp
	Bot.hpp
	Channel.cpp
	Channel.hpp
	Error.hpp
	Guild.cpp
	Guild.hpp
	Logger.cpp
	Logger.hpp
	Message.cpp
	Message.hpp
	Network.cpp
	Network.hpp
	Singleton.hpp
	Http.cpp
	Http.hpp
	Callback.hpp
	PawnDispatcher.cpp
	PawnDispatcher.hpp
	Role.cpp
	Role.hpp
	SampConfigReader.cpp
	SampConfigReader.hpp
	User.cpp
	User.hpp
	WebSocket.cpp
	WebSocket.hpp
	Embed.cpp
	Embed.hpp
	Emoji.cpp
	Emoji.hpp
	main.cpp
	misc.hpp
	natives.cpp
	natives.hpp
	sdk.hpp
	types.hpp
	utils.hpp
	plugin.def
)

if (UNIX)
	target_sources(kook-connector PUBLIC ${SAMPSDK_DIR}/amx/getch.c)
endif()

set_property(TARGET kook-connector PROPERTY CXX_STANDARD 17)
set_property(TARGET kook-connector PROPERTY CXX_STANDARD_REQUIRED ON)

target_include_directories(kook-connector PRIVATE
	${OPENSSL_INCLUDE_DIR}
	${Boost_INCLUDE_DIR}
	${SAMPSDK_INCLUDE_DIR}
	${DCC_CONFIG_INCLUDE_DIR}
	${LOGCORE_INCLUDE_DIR}
	${PROJECT_SOURCE_DIR}/libs/json
	${PROJECT_SOURCE_DIR}/libs/date/include
)

if(MSVC)
	target_compile_definitions(kook-connector PRIVATE
		BOOST_ALL_NO_LIB # disable auto-linking
		_CRT_SECURE_NO_WARNINGS
		NOMINMAX
		_WIN32_WINNT=0x0600
		_SCL_SECURE_NO_WARNINGS
	)
	target_link_libraries(kook-connector Crypt32.lib)

endif()

target_compile_definitions(kook-connector PRIVATE ONLY_C_LOCALE) # make the date library use the C locale only

target_link_libraries(kook-connector log-core fmt OpenSSL::SSL OMP-SDK)

if(UNIX)
	if(NOT APPLE)
		target_link_libraries(kook-connector rt)
	endif()
	
	set_target_properties(kook-connector PROPERTIES
		INSTALL_RPATH "$ORIGIN/..:$ORIGIN/")
endif()

# increase warning levels
if (MSVC)
	target_compile_options(kook-connector PRIVATE
		/W4
		/WX-
		/permissive-
		/wd4819
	)

	# also enable multi-processor compilation
	target_compile_options(kook-connector PRIVATE
		/MP
	)
elseif(UNIX)
	target_compile_options(kook-connector PRIVATE
		-Wall
		-Wextra
		-pedantic
		-Werror
	)
endif()

if (MSVC)
	# WORKAROUND: Boost 1.70.0+ Beast has a bug that prevents using MSVC compiler optimizations
	# see https://github.com/boostorg/beast/issues/1582
	target_compile_options(kook-connector PUBLIC "$<$<CONFIG:Release>:/Od>")
endif()


install(TARGETS kook-connector DESTINATION "plugins/")
install(TARGETS log-core DESTINATION "./")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/kook-connector.inc" DESTINATION "pawno/include/")
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "./")
if (WIN32 AND NOT OPENSSL_USE_STATIC_LIBS)
	file (GLOB _openssl_dlls  "${OPENSSL_ROOT_DIR}/bin/lib*.dll")
	foreach (_dll_file ${_openssl_dlls})
		install (FILES "${_dll_file}" DESTINATION "./")
	endforeach()
endif()


set(CPACK_PACKAGE_VERSION ${DCC_VERSION})
if(WIN32)
	set(CPACK_GENERATOR ZIP)
else()
	set(CPACK_GENERATOR TGZ)
endif()

include(CPack)
