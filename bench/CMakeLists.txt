add_executable(load_generator load_generator.cpp)
target_link_libraries(load_generator PRIVATE Threads::Threads)

