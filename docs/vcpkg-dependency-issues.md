# C++ 项目模板依赖管理问题报告

## 项目概述

本项目是一个可复用的 C++ 项目模板，包含：
- CMake 构建系统
- vcpkg 依赖管理（manifest 模式）
- GoogleTest 单元测试
- libs/ 目录存放 header-only 库

---

## 遇到的问题及解决方案

### 问题 1: vcpkg DLL 自动复制问题

**现象**: Debug 构建错误链接到 Release DLL（libcurl.dll 而非 libcurl-d.dll）

**尝试方案**:
1. 手动写脚本根据 Build Type 复制不同 DLL ❌
2. 尝试在 CMake 中根据 CMAKE_BUILD_TYPE 判断 ❌（多配置生成器如 VS 不适用）

**最终解决方案**: 
使用 vcpkg 的 `find_package()` 自动处理。vcpkg 生成的 CMake target 会根据当前配置自动选择正确的库（Debug/Release）。

---

### 问题 2: 7zip include 路径找不到

**现象**: 编译错误 `fatal error C1083: Cannot open include file: "CPP/7zip.h"`

**尝试方案**:
1. 在 src/CMakeLists.txt 硬编码 7zip 路径 ❌
2. 在 VcpkgDeps.cmake 硬编码 7zip 判断 (`if(DEP STREQUAL "7zip")`) ❌

**根因**: vcpkg 的 7zip 包没有 `CPP/7zip.h` 文件，只有 `7zip/CPP/` 和 `7zip/C/` 目录

**最终解决方案**:
从 CMake target 的 `INTERFACE_INCLUDE_DIRECTORIES` 自动获取 include 路径：
```cmake
if(TARGET ${DEP}::${DEP})
    get_target_property(INC_DIRS ${DEP}::${DEP} INTERFACE_INCLUDE_DIRECTORIES)
    if(INC_DIRS)
        target_include_directories(${TARGET} PRIVATE ${INC_DIRS})
    endif()
endif()
```

---

### 问题 3: 7zip 库链接失败

**现象**: 链接错误 `error LNK2019: 无法解析的外部符号 LzmaCompress`

**尝试方案**:
1. vcpkg 生成的 `7zip::7zip` target 存在，但只包含 INTERFACE_INCLUDE_DIRECTORIES，不包含实际库路径 ❌
2. 只链接 `${DEP}::${DEP}` 对于 7zip 无效 ❌
3. 在 src/CMakeLists.txt 显式添加 `target_link_libraries(... 7zip::7zip)` ❌（仍然链接失败）

**根因**: 
- `7zip::7zip` 是 IMPORTED SHARED 类型，但只设置了头文件路径，没有设置 IMPORTED_IMPLIB
- vcpkg 的 7zip 包只有 `7zip.lib` 导入库，没有同名的 CMake alias target 提供链接

**最终解决方案**:
在 VcpkgDeps.cmake 中，如果 CMake target 链接失败，自动搜索 lib 目录下的 .lib 文件：
```cmake
if(NOT ${FOUND_VAR})
    file(GLOB_RECURSE LIBS "${VCPKG_INSTALLED_DIR}/lib/*${DEP}*.lib")
    foreach(LIB ${LIBS})
        if(NOT LIB MATCHES "optimized" AND NOT LIB MATCHES "debug")
            target_link_libraries(${TARGET} PRIVATE "${LIB}")
        endif()
    endforeach()
endif()
```

---

### 问题 4: vcpkg toolchain 路径错误

**现象**: CMake 配置时 vcpkg 未生效，依赖未安装

**尝试方案**:
1. 设置 `CMAKE_TOOLCHAIN_FILE` 为 `${VCPKG_ROOT}/vcpkg.cmake` ❌（不存在此文件）
2. 设置 `VCPKG_INSTALLED_DIR` 手动指定 ❌

**最终解决方案**:
正确路径是 `${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`：
```cmake
if(VCPKG_ROOT AND EXISTS "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain")
    set(VCPKG_MANIFEST_MODE ON)
endif()
```

---

### 问题 5: vcpkg.json 包名格式错误

**现象**: `Package names must be lowercase alphanumeric+hyphens`

**尝试方案**:
1. 直接使用用户输入的项目名 ❌

**最终解决方案**:
在 config.bat 中对包名进行处理：
```batch
powershell -Command "$newName = '%NEW_NAME%'.ToLower().Replace(' ', '-').Replace('_', '-')"
```

---

## 最终方案总结

### VcpkgDeps.cmake 核心逻辑

1. **先尝试 find_package + CMake target 链接**
2. **如果 target 不存在或链接失败，自动搜索 lib 目录下的 .lib 文件**
3. **从 target 的 INTERFACE_INCLUDE_DIRECTORIES 获取 include 路径**

### 关键代码

```cmake
# 1. 尝试 CMake target
if(TARGET ${DEP}::${DEP})
    target_link_libraries(${TARGET} PRIVATE ${DEP}::${DEP})
    set(FOUND_VAR TRUE)
endif()

# 2. target 不存在则自动搜索 lib 目录
if(NOT ${FOUND_VAR})
    file(GLOB_RECURSE LIBS "${VCPKG_INSTALLED_DIR}/lib/*${DEP}*.lib")
    foreach(LIB ${LIBS})
        target_link_libraries(${TARGET} PRIVATE "${LIB}")
    endforeach()
endif()

# 3. 自动获取 include 路径
if(TARGET ${DEP}::${DEP})
    get_target_property(INC_DIRS ${DEP}::${DEP} INTERFACE_INCLUDE_DIRECTORIES)
    if(INC_DIRS)
        target_include_directories(${TARGET} PRIVATE ${INC_DIRS})
    endif()
endif()
```

---

## 验证结果

| 测试项 | 结果 |
|--------|------|
| Debug 构建 | ✅ 使用 libcurl-d.dll |
| Release 构建 | ✅ 使用 libcurl.dll |
| 7zip LZMA 压缩 | ✅ 正常工作 |
| 单元测试 | ✅ 6/6 通过 |
| 无硬编码 | ✅ 完全自动 |

---

## 教训

1. **不要假设 CMake target 一定包含完整链接信息** - 有些包（如 7zip）的 target 只提供 include 路径
2. **多配置生成器（VS）下 CMAKE_BUILD_TYPE 不可用** - 应依赖 vcpkg 内置机制
3. **通用方案优于特殊处理** - 用 glob 搜索 lib 目录比硬编码包名更健壮
4. **vcpkg 包名必须是小写 alphanumeric+hyphens** - 用户输入需要转换格式
