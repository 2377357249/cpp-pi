file(REMOVE_RECURSE
  ".12"
  "fmt.o"
  "fmt.pcm"
  "libfmt-moduled.12.2.1.dylib"
  "libfmt-moduled.12.dylib"
  "libfmt-moduled.dylib"
  "libfmt-moduled.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang )
  include(CMakeFiles/fmt-module.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
