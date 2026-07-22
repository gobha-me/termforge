find_package(Catch2 3 QUIET)

if (Catch2_FOUND)
else ()
    if (NOT Catch2_URI)
        set(Catch2_URI https://github.com/catchorg/Catch2.git) 
    endif()

    if (NOT Catch2_TAG)
        set(Catch2_TAG v3.5.2)
    endif()

    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY ${Catch2_URI}
        GIT_TAG ${Catch2_TAG}
    )

    FetchContent_MakeAvailable(Catch2)
endif()
