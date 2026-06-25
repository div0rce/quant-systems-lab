option(QSL_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(QSL_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

# ASan and TSan instrument memory access in incompatible ways and cannot be combined.
if(QSL_ENABLE_ASAN AND QSL_ENABLE_TSAN)
  message(FATAL_ERROR "QSL_ENABLE_ASAN and QSL_ENABLE_TSAN are mutually exclusive; enable only one.")
endif()

if(QSL_ENABLE_ASAN)
  # -fno-sanitize-recover=undefined makes UBSan ABORT on the first violation instead of its default
  # "recover" mode (print a diagnostic and continue, with the process still exiting 0). Without it
  # the UBSan half of this gate is non-functional: a pure-UBSan defect (signed overflow, invalid
  # enum/bool load, out-of-range shift, misaligned/null pointer arithmetic) prints a warning but the
  # test still PASSES, so `make asan` and the CI sanitizers job stay green. ASan already aborts on
  # memory errors by default; this brings UBSan to parity.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ThreadSanitizer (M27): data-race detection for the concurrent pipeline. It is a correctness
# gate, not a performance tool, never collect benchmark numbers under TSan.
if(QSL_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()
