option(QSL_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(QSL_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

# ASan and TSan instrument memory access in incompatible ways and cannot be combined.
if(QSL_ENABLE_ASAN AND QSL_ENABLE_TSAN)
  message(FATAL_ERROR "QSL_ENABLE_ASAN and QSL_ENABLE_TSAN are mutually exclusive; enable only one.")
endif()

if(QSL_ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ThreadSanitizer (M27): data-race detection for the concurrent pipeline. It is a correctness
# gate, not a performance tool — never collect benchmark numbers under TSan.
if(QSL_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()
