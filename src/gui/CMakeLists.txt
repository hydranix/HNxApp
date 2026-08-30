# HNxPacker: the graphical front end.
#
# Links hnxpack_core directly rather than driving hnxpack.exe as a subprocess.
# That is what makes real progress reporting, cancellation and structured errors
# possible; shelling out would mean parsing stdout back into the information the
# library already has.

add_executable(HNxPacker WIN32
    main.cpp
    app.cpp
    draw.cpp
    gui.rc
    ${HNX_STUBS_RC_FILE}
)

target_link_libraries(HNxPacker PRIVATE
    hnxpack_core
    imgui
    hnx_warnings
)

if(MSVC)
    # The manifest comes from gui.rc, so the linker must not add a second one.
    target_link_options(HNxPacker PRIVATE /MANIFEST:NO)
endif()

hnx_use_embedded_stubs(HNxPacker)
