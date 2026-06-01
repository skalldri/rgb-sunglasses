# Normally this would be a zephyr_library() and zephyr_library_sources(), but 
# it doesn't link correctly with out-of-tree drivers
zephyr_sources(vm3011_init.c)
zephyr_sources(vm3011.cpp)