option(QSL_ENABLE_ASAN "Enable AddressSanitizer" OFF)

if(QSL_ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
