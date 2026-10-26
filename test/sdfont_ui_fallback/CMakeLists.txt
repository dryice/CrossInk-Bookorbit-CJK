# CJK UI-font fallback lifetime: real SdCardFontSystem + manager + registry +
# real .cpfont fixtures in an in-memory filesystem, against the real renderer
# font resolution. See SdFontUiFallbackTest.cpp.
add_executable(SdFontUiFallbackTest SdFontUiFallbackTest.cpp
  ${REPO_ROOT}/src/SdCardFontSystem.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontManager.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp)
target_include_directories(SdFontUiFallbackTest PRIVATE stubs
  ../epub_grayscale/raster_stubs
  ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/include
  ${REPO_ROOT}/src ${REPO_ROOT}/include
  ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/MiniBidi ${REPO_ROOT}/lib/InflateReader ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget
  ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/Epub
  ../memory_policy/stubs)
target_compile_definitions(SdFontUiFallbackTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1
  CROSSINK_SCALABLE_FONTS=0 CROSSINK_APP_CAP_TOUCH=0 CROSSINK_APP_CAP_USB_DRIVE=0)
target_compile_options(SdFontUiFallbackTest PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(SdFontUiFallbackTest PRIVATE -fsanitize=address,undefined)
target_link_libraries(SdFontUiFallbackTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(SdFontUiFallbackTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
