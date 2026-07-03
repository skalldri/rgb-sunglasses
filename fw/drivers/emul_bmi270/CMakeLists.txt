# Normally this would be a zephyr_library() and zephyr_library_sources(), but
# it doesn't link correctly with out-of-tree drivers (see drivers/vm3011).
zephyr_sources(emul_bmi270.c)
zephyr_include_directories(.)
