# Fails if the library uses anything NFR-PORT-1/4 forbids. Run with -DROOT=<repository root>.
set(FORBIDDEN "<thread>" "<mutex>" "<atomic>" "<sys/" "<arpa/" "<netinet/" "<windows.h>" "<winsock"
    "_clock::now")

file(GLOB_RECURSE SOURCES "${ROOT}/include/*" "${ROOT}/src/*")
if(NOT SOURCES)
    message(FATAL_ERROR "No sources found under ${ROOT}")
endif()

set(FAILED FALSE)
foreach(SOURCE IN LISTS SOURCES)
    file(READ "${SOURCE}" CONTENT)
    foreach(TEXT IN LISTS FORBIDDEN)
        string(FIND "${CONTENT}" "${TEXT}" POSITION)
        if(NOT POSITION EQUAL -1)
            message(SEND_ERROR "${SOURCE}: forbidden text '${TEXT}'")
            set(FAILED TRUE)
        endif()
    endforeach()
endforeach()

if(FAILED)
    message(FATAL_ERROR "Forbidden includes or calls found")
endif()
