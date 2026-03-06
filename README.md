# C++ Project Template

C++ 项目模板，基于 CMake + vcpkg + GoogleTest

## 目录结构

```
<PROJECT_NAME>/
├── CMakeLists.txt              # 根 CMake 配置
├── vcpkg.json                  # vcpkg 依赖清单
├── cmake/                      # CMake 工具模块
│   ├── CompilerOptions.cmake   # 编译器选项
│   └── VcpkgDeps.cmake         # vcpkg 自动依赖管理
├── src/                        # 源代码
│   ├── CMakeLists.txt
│   └── main.cpp
├── include/                    # 头文件
├── libs/                      # 第三方库 (手动管理)
│   └── googletest/            # 内置 GoogleTest
├── tests/                     # 测试代码
│   ├── CMakeLists.txt
│   └── main_test.cpp
├── scripts/                   # 构建脚本
│   ├── build.bat              # Windows 构建
│   ├── config.bat             # 配置项目名称
│   └── detect_libs.bat        # 自动检测 libs/ 目录
├── docs/                      # 文档
├── .gitignore
└── README.md
```

## 特性

- **CMake 构建系统** - 支持 Visual Studio 2022
- **vcpkg 依赖管理** - Manifest 模式，自动安装依赖
- **GoogleTest 单元测试** - 内置集成
- **自动依赖链接** - 无需手动配置 vcpkg 依赖
- **Debug/Release 自动切换** - 自动使用正确的 DLL

## 快速开始

### 前置条件

1. 安装 [Visual Studio 2022](https://visualstudio.microsoft.com/)
2. 安装 [vcpkg](https://github.com/microsoft/vcpkg)
3. 设置环境变量 `VCPKG_ROOT` 指向 vcpkg 目录

### 配置项目名称

```batch
scripts\config.bat
```

### 构建

```batch
scripts\build.bat
```

### 构建 Release 版本

```batch
scripts\build.bat release
```

### 清理构建目录

```batch
scripts\build.bat clean
```

### 运行测试

```batch
build\tests\Debug\PlaygroundCpp_tests.exe
```

## 添加依赖

在 `vcpkg.json` 的 `dependencies` 数组中添加包名：

```json
{
  "dependencies": [
    "curl",
    "7zip"
  ]
}
```

重新构建后，`cmake/VcpkgDeps.cmake` 会自动：
1. 链接 vcpkg 包
2. 设置 include 路径
3. 复制 DLL 到输出目录

## 添加新代码

1. 源代码放 `src/`，头文件放 `include/`
2. 修改 `src/CMakeLists.txt` 添加源文件
3. 重新构建

## 添加测试

1. 测试代码放 `tests/`
2. 修改 `tests/CMakeLists.txt` 添加测试文件
3. 运行测试

## libs/ 目录

将 header-only 库放入 `libs/` 目录，每个库一个子目录：

```
libs/
├── concurrentqueue/
│   └── include/
└── thread-pool/
    └── include/
```

运行 `scripts\detect_libs.bat` 自动检测并配置。

## 文档

- [vcpkg 依赖管理问题报告](docs/vcpkg-dependency-issues.md)
