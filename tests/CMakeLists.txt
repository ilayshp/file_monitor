
set(TARGET_NAME	tests)

add_executable(${TARGET_NAME}
  scoped_temp_folder.cpp
  scoped_temp_folder.hpp
  integration.cpp
  main.cpp)
  
target_link_libraries(${TARGET_NAME}
  PUBLIC file_monitor)

target_include_directories(${TARGET_NAME}
  PUBLIC .)

add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
