# Plume: own default colors instead of Telegram ones.
# Applies plume_day.palette (lines "key: #color;") on top of lib_ui/ui/colors.palette
# before lib_ui generates its styles. Rewrites the file only when something changes.
set(plume_palette_target ${CMAKE_CURRENT_SOURCE_DIR}/lib_ui/ui/colors.palette)
file(READ ${plume_palette_target} plume_palette_text)
file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/plume_day.palette plume_palette_lines REGEX "^[A-Za-z0-9_]+: #")
set(plume_palette_new "${plume_palette_text}")
foreach(plume_line ${plume_palette_lines})
    string(REGEX MATCH "^([A-Za-z0-9_]+): (#[0-9a-fA-F]+);" plume_match "${plume_line}")
    if (plume_match)
        set(plume_key ${CMAKE_MATCH_1})
        set(plume_value ${CMAKE_MATCH_2})
        string(REGEX REPLACE "\n${plume_key}: [^;\n]*;" "\n${plume_key}: ${plume_value};" plume_palette_new "${plume_palette_new}")
    endif()
endforeach()
if (NOT "${plume_palette_new}" STREQUAL "${plume_palette_text}")
    file(WRITE ${plume_palette_target} "${plume_palette_new}")
    message(STATUS "Plume: default palette applied")
endif()
