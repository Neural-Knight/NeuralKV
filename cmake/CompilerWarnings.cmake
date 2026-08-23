# Shared warning flags for all first-party targets.
function(neuralkv_set_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
  )
endfunction()
