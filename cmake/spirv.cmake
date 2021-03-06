####
# Find either LLVM SPIR-V compiler ...
####

if(NOT SPIRV_COMPILER_ROOT)
	# Try to detect the location of the SPIRV-LLVM binaries
	find_program(LLVM_SPIRV_FOUND NAMES llvm-spirv HINTS "/opt/SPIRV-LLVM/build/bin/")
	if(LLVM_SPIRV_FOUND)
		get_filename_component(SPIRV_COMPILER_ROOT "${LLVM_SPIRV_FOUND}" DIRECTORY)
	endif()
endif()
if(SPIRV_COMPILER_ROOT)
	message(STATUS "Khronos OpenCL toolkit: ${SPIRV_COMPILER_ROOT}")
	find_file(SPIRV_CLANG_FOUND clang PATHS ${SPIRV_COMPILER_ROOT} NO_DEFAULT_PATH)
	find_file(SPIRV_LLVM_SPIR_FOUND llvm-spirv PATHS ${SPIRV_COMPILER_ROOT} NO_DEFAULT_PATH)
	if(SPIRV_CLANG_FOUND)
		message(STATUS "Khronos OpenCL compiler: ${SPIRV_CLANG_FOUND}")
	endif()
endif()

####
# ... or find LLVM SPIR-V translator
####
if(NOT SPIRV_COMPILER_ROOT)
	if(NOT SPIRV_TRANSLATOR_ROOT)
		# Try to detect the location of the SPIRV-LLVM-Translator binaries
		find_program(LLVM_SPIRV_FOUND NAMES llvm-spirv HINTS "/opt/SPIRV-LLVM-Translator/build/tools/")
		if(LLVM_SPIRV_FOUND)
			get_filename_component(SPIRV_TRANSLATOR_ROOT "${LLVM_SPIRV_FOUND}" DIRECTORY)
		endif()
	endif()
	if(SPIRV_TRANSLATOR_ROOT)
		message(STATUS "Khronos OpenCL toolkit: ${SPIRV_TRANSLATOR_ROOT}")
		# The translator uses the built-in clang
		find_program(SPIRV_CLANG_FOUND clang)
		find_file(SPIRV_LLVM_SPIR_FOUND llvm-spirv PATHS ${SPIRV_TRANSLATOR_ROOT} NO_DEFAULT_PATH)
		if(SPIRV_CLANG_FOUND)
			message(STATUS "Khronos OpenCL compiler: ${SPIRV_CLANG_FOUND}")
		endif()
	endif()
endif()

if(NOT SPIRV_COMPILER_ROOT AND NOT SPIRV_TRANSLATOR_ROOT)
	message(WARNING "SPIR-V frontend configured, but no SPIR-V compiler found!")
endif()

####
# Enable SPIR-V Tools front-end
####

# If the complete tool collection is provided, compile the SPIR-V front-end
if(SPIRV_LLVM_SPIR_FOUND AND SPIRV_FRONTEND)
	message(STATUS "Compiling SPIR-V front-end...")
	if(DEPENDENCIES_USE_FETCH_CONTENT)
		# CMake 3.14 introduces https://cmake.org/cmake/help/latest/module/FetchContent.html which allows us to run the configuration step
		# of the downloaded dependencies at CMake configuration step and therefore, we have the proper targets available.
		include(FetchContent)
		FetchContent_Declare(SPIRV-Headers GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers.git)
		FetchContent_MakeAvailable(SPIRV-Headers)
		FetchContent_GetProperties(SPIRV-Headers SOURCE_DIR SPIRV_HEADERS_SOURCE_DIR)
		# To be compatible with the old style, we cannot (in src/CMakeLists.txt) directly link against the targets,
		# so define the variables that are expected.
		add_library(SPIRV-Dependencies STATIC IMPORTED)
		set(SPIRV_Headers_HEADERS ${SPIRV_HEADERS_SOURCE_DIR}/include)

		FetchContent_Declare(spirv-tools-project GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Tools.git)
		# CMake configuration flags for SPIRV-Tools project
		set(SPIRV_SKIP_EXECUTABLES OFF)
		set(SPIRV_SKIP_TESTS ON)
		set(SPIRV-Headers_SOURCE_DIR ${SPIRV_HEADERS_SOURCE_DIR})
		FetchContent_MakeAvailable(spirv-tools-project)
		# Set variables expected by the VC4CC library to the SPIRV-Tools libraries.
		set(SPIRV_Tools_LIBS SPIRV-Tools SPIRV-Tools-opt SPIRV-Tools-link)
		# the headers are already included by linking the targets
		set(SPIRV_Tools_HEADERS "")
		# we need to export these targets, since they are required by VC4CC which we export
		export(TARGETS SPIRV-Headers SPIRV-Tools SPIRV-Tools-opt SPIRV-Tools-link FILE spirv-exports.cmake)
	else()
		#Add SPIR-V headers project
		ExternalProject_Add(SPIRV-Headers-project
			PREFIX 				${CMAKE_BINARY_DIR}/spirv-headers
			GIT_REPOSITORY 		https://github.com/KhronosGroup/SPIRV-Headers.git
			UPDATE_COMMAND 		git pull -f https://github.com/KhronosGroup/SPIRV-Headers.git
			STEP_TARGETS 		build
			EXCLUDE_FROM_ALL	TRUE
			TIMEOUT 			30		#Timeout for downloads, in seconds
			CMAKE_ARGS
			  -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
			  -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
			  -DCMAKE_FIND_ROOT_PATH=${CMAKE_FIND_ROOT_PATH}
		)
		ExternalProject_Get_Property(SPIRV-Headers-project SOURCE_DIR)
		set(SPIRV_Headers_HEADERS ${SOURCE_DIR}/include)

		ExternalProject_Add(spirv-tools-project
			DEPENDS 			SPIRV-Headers-project-build
			PREFIX 				${CMAKE_BINARY_DIR}/spirv-tools
			GIT_REPOSITORY 		https://github.com/KhronosGroup/SPIRV-Tools.git
			UPDATE_COMMAND 		git pull -f https://github.com/KhronosGroup/SPIRV-Tools.git
			CMAKE_ARGS 			-DSPIRV_SKIP_EXECUTABLES:BOOL=ON -DSPIRV_SKIP_TESTS:BOOL=ON -DSPIRV-Headers_SOURCE_DIR:STRING=${SOURCE_DIR}
			STEP_TARGETS 		build
			EXCLUDE_FROM_ALL	TRUE
			TIMEOUT 			30		#Timeout for downloads, in seconds
			CMAKE_ARGS
			  -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
			  -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
			  -DCMAKE_FIND_ROOT_PATH=${CMAKE_FIND_ROOT_PATH}
		)
		ExternalProject_Get_Property(spirv-tools-project BINARY_DIR)
		ExternalProject_Get_Property(spirv-tools-project SOURCE_DIR)
		set(SPIRV_Tools_LIBS "-Wl,--whole-archive ${BINARY_DIR}/source/libSPIRV-Tools.a ${BINARY_DIR}/source/opt/libSPIRV-Tools-opt.a ${BINARY_DIR}/source/link/libSPIRV-Tools-link.a -Wl,--no-whole-archive")
		set(SPIRV_Tools_HEADERS ${SOURCE_DIR}/include)

		# This target is used to collect the dependencies on all SPIR-V library build steps
		add_library(SPIRV-Dependencies STATIC IMPORTED)
		add_dependencies(SPIRV-Dependencies SPIRV-Headers-project-build)
		add_dependencies(SPIRV-Dependencies spirv-tools-project-build)
	endif()
	set(VC4C_ENABLE_SPIRV_FRONTEND ON)
endif()