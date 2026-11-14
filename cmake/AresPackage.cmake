# CPack External generator script: build the IPK from the staging directory
# and, when the Homebrew Channel manifest source URLs are configured, generate
# manifest.json (with ipkHash) next to it.
#
# ares-package names the IPK from appinfo.json, so package into an empty
# scratch directory and take whatever single .ipk it produced.
set(_out "${CPACK_TOPLEVEL_DIRECTORY}/ares-package")
file(REMOVE_RECURSE "${_out}")
file(MAKE_DIRECTORY "${_out}")

# Two ares-package flavours are in the wild: webosbrew/ares-cli-rs understands
# --force-arch, the LG webOS SDK CLI does not (and infers the device arch from
# the app). Detect the flag instead of requiring one specific build.
set(_arch_args "")
execute_process(COMMAND ares-package --help OUTPUT_VARIABLE _ares_help ERROR_VARIABLE _ares_help)
if(_ares_help MATCHES "force-arch")
	set(_arch_args --force-arch "${CPACK_WEBOS_PACKAGE_ARCH}")
endif()

execute_process(COMMAND ares-package "${CPACK_TEMPORARY_DIRECTORY}" -o "${_out}" ${_arch_args}
	COMMAND_ERROR_IS_FATAL ANY
)
file(GLOB _ipks "${_out}/*.ipk")
list(LENGTH _ipks _count)
if(NOT _count EQUAL 1)
	message(FATAL_ERROR "Expected one IPK from ares-package, got: ${_ipks}")
endif()
get_filename_component(_ipk_name "${_ipks}" NAME)
set(_ipk "${CPACK_PACKAGE_DIRECTORY}/${_ipk_name}")
# Copy instead of rename: replacing an existing file with RENAME fails with
# EACCES on a Windows-mounted source tree (DrvFs), and a copy overwrites it.
#
# A Windows host can also keep the previous IPK open (Explorer preview, search
# indexer, editor), which makes even the copy fail. That must not fail the
# build, so try the canonical name and then a few suffixed ones, and let
# `cmake -E copy_if_different` report success (a CMake-level file(COPY_FILE)
# failure would be fatal).
set(_copied FALSE)
foreach(_suffix "" "-new" "-2" "-3" "-4")
	if(_suffix STREQUAL "")
		set(_target "${_ipk}")
	else()
		string(REGEX REPLACE "\.ipk$" "${_suffix}.ipk" _target "${_ipk}")
	endif()
	execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_ipks}" "${_target}"
		RESULT_VARIABLE _copy_rc OUTPUT_QUIET ERROR_QUIET)
	if(_copy_rc EQUAL 0)
		set(_ipk "${_target}")
		set(_copied TRUE)
		break()
	endif()
endforeach()
if(NOT _copied)
	message(FATAL_ERROR "Could not write the IPK into ${CPACK_PACKAGE_DIRECTORY}: every candidate name is locked")
endif()
if(NOT _ipk STREQUAL "${CPACK_PACKAGE_DIRECTORY}/${_ipk_name}")
	message(WARNING "the canonical IPK name is locked by another process; wrote ${_ipk}")
endif()
message(STATUS "IPK: ${_ipk}")

find_program(GEN_MANIFEST webosbrew-gen-manifest)
if(NOT GEN_MANIFEST)
	message(STATUS "webosbrew-gen-manifest not found, skipping manifest generation")
	return()
endif()
# The manifest needs absolute URLs for the icon and the project page; both are
# per-repository settings, so pass them on the cmake command line:
#   -DWEBOS_MANIFEST_ICON=https://.../icon.png -DWEBOS_MANIFEST_LINK=https://github.com/<you>/<repo>
if(NOT CPACK_WEBOS_MANIFEST_ICON OR NOT CPACK_WEBOS_MANIFEST_LINK)
	message(STATUS "manifest URLs not configured (WEBOS_MANIFEST_ICON / WEBOS_MANIFEST_LINK), skipping manifest generation")
	return()
endif()

# manifest.json is the name the apps-repo listing points at
# (releases/latest/download/manifest.json).
execute_process(COMMAND "${GEN_MANIFEST}"
	-p "${_ipk}"
	-o "${CPACK_PACKAGE_DIRECTORY}/manifest.json"
	-i "${CPACK_WEBOS_MANIFEST_ICON}"
	-l "${CPACK_WEBOS_MANIFEST_LINK}"
	COMMAND_ERROR_IS_FATAL ANY
)
