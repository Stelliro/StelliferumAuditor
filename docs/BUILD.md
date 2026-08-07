# BUILD GUIDE - Stelliferum Auditor

## Overview

This guide covers building the Stelliferum Auditor on Windows, macOS, and Linux.

The project uses:
- **CMake 3.16+** for cross-platform builds
- **Raylib 5.5** for UI/graphics
- **C11** standard

---

## Quick Start

### Windows (Batch Script)

```cmd
# Release build
build.bat

# Debug build
build.bat debug

# Clean build
build.bat clean

# Full rebuild (clean + build)
build.bat rebuild
```

### Windows (PowerShell Script)

```powershell
# Release build
.\build.ps1

# Debug build
.\build.ps1 -Config Debug

# Clean
.\build.ps1 -Clean

# Rebuild with verbose output
.\build.ps1 -Rebuild -Verbose

# Build and install
.\build.ps1 -Install
```

### macOS / Linux

```bash
# Create and configure build directory
mkdir build && cd build
cmake .. -G "Unix Makefiles"

# Build
cmake --build . --config Release --parallel $(nproc)

# Or with make
make -j$(nproc)

# Run
./bin/StelliferumAuditor
```

---

## Prerequisites

### Windows
- **Visual Studio 2022** (Community, Professional, or Enterprise)
  - C++ workload required
  - Desktop development tools
- **CMake 3.16 or later**
  - Download: https://cmake.org/download/
  - Add to PATH during installation
- **Git** (for fetching dependencies)
  - Download: https://git-scm.com/

### macOS
- **Xcode Command Line Tools**
  ```bash
  xcode-select --install
  ```
- **CMake**
  ```bash
  brew install cmake
  ```
- **Git**
  ```bash
  brew install git
  ```

### Linux (Ubuntu/Debian)
```bash
sudo apt-get update
sudo apt-get install build-essential cmake git
```

### Linux (Fedora/RHEL)
```bash
sudo dnf install gcc cmake git
```

---

## Detailed Build Steps

### Method 1: Windows Batch Script (Easiest)

```cmd
cd C:\path\to\StelliferumAuditor
build.bat
```

The script will:
1. Check for required tools (CMake, Visual Studio, Git)
2. Create build directory
3. Configure with CMake
4. Build with Visual Studio
5. Copy config files

**Output:** `build\bin\StelliferumAuditor.exe`

### Method 2: Windows PowerShell Script

```powershell
cd C:\path\to\StelliferumAuditor
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
.\build.ps1
```

Options:
- `-Config Debug|Release` : Build configuration
- `-Clean` : Remove build artifacts
- `-Rebuild` : Clean and rebuild
- `-Verbose` : Show detailed output
- `-Install` : Install after build

### Method 3: Manual CMake (All Platforms)

```bash
# Create build directory
mkdir build
cd build

# Configure
cmake .. -G "Visual Studio 17 2022"  # Windows
# OR
cmake .. -G "Unix Makefiles"         # macOS/Linux
# OR
cmake .. -G Xcode                    # macOS (Xcode)

# Build
cmake --build . --config Release --parallel 8

# Run
./bin/StelliferumAuditor              # Linux/macOS
./bin/Release/StelliferumAuditor.exe  # Windows

# Optional: Install
cmake --install . --config Release
```

---

## Build Configuration

### Environment Variables

```bash
# Specify OLLAMA URL for economy balancer
export OLLAMA_URL=http://localhost:11434/api/generate

# Specify OLLAMA model
export OLLAMA_MODEL=llama3

# Or on Windows Command Prompt:
set OLLAMA_URL=http://localhost:11434/api/generate
set OLLAMA_MODEL=llama3
```

### CMake Options

```bash
# Use system raylib instead of fetching
cmake .. -DUSE_SYSTEM_RAYLIB=ON

# Custom install prefix
cmake .. -DCMAKE_INSTALL_PREFIX=/opt/stelliferum
```

---

## Troubleshooting

### CMake Says "Visual Studio Not Found"
- Ensure Visual Studio 2022 is installed with C++ workload
- Verify `devenv` command works in terminal:
  ```cmd
  devenv /version
  ```
- Try specifying VS version explicitly:
  ```cmd
  cmake .. -G "Visual Studio 17 2022" -A x64
  ```

### Git Fetch Fails for Raylib
- Check internet connection
- Verify Git is in PATH:
  ```bash
  git --version
  ```
- Try disabling enterprise proxy/firewall temporarily

### Build Fails with "Missing raylib"
- Ensure previous build directory is clean:
  ```bash
  rm -rf build
  mkdir build
  cd build
  cmake ..
  ```

### Linking Errors on Windows
- Ensure you're using the matching Visual Studio generator version
- Clean build directory and reconfigure

### CMake < 3.16
- Update CMake:
  ```bash
  cmake --version  # Check version
  # Download from https://cmake.org
  ```

---

## Running the Application

### After Building

```bash
# Linux/macOS
./build/bin/StelliferumAuditor

# Windows
.\build\bin\Release\StelliferumAuditor.exe
```

### Setting Up Data

1. **Copy your types.xml** files to the project directory
2. **Run the auditor** - it will parse and load files
3. **Export inventory** to CSV
4. **Run economy balancer**:
   ```bash
   python3 economy_balancer.py
   ```

### Economy Balancer Requirements

The economy balancer (`economy_balancer.py`) requires:

1. **OLLAMA** running locally (LLM reasoning engine)
   ```bash
   # Install OLLAMA
   # https://ollama.ai
   
   # Download a model
   ollama pull llama3
   
   # Run OLLAMA service
   ollama serve
   ```

2. **Python 3.8+** with requests library
   ```bash
   pip install requests
   ```

3. **Inventory CSV** exported from the C++ auditor

---

## Build Artifacts

After building, you'll find:

```
build/
├── bin/
│   └── StelliferumAuditor.exe    (or .out on Linux)
├── lib/                           (libraries)
├── CMakeFiles/                    (CMake cache)
└── Release/                       (Visual Studio)
```

---

## Performance Optimization

### Multi-threaded Build

```bash
# Windows Command Prompt
cmake --build . --config Release --parallel 8

# Or with make
make -j8
```

### Release Build Optimization

The scripts automatically build in Release mode with optimizations enabled:
- `O2` optimization level (MSVC)
- `-O2` optimization level (GCC/Clang)
- Link-time optimization (LTO) enabled

---

## Development Workflow

### Quick Rebuild During Development

```bash
# Build incrementally (faster)
cmake --build build --config Debug --parallel

# Or  
cd build && make
```

### Incremental Builds After Code Changes

The build system automatically tracks dependencies. Only modified files are recompiled:

```bash
# Edit a .c file, then rebuild
cmake --build build
```

### Generate IDE Project Files

Keep IDE projects in sync:

```bash
# Generate Visual Studio project in build/
cmake .. -G "Visual Studio 17 2022"

# Then open in IDE
code build/StelliferumAuditor.sln  # VS Code with solution explorer
# OR
devenv build/StelliferumAuditor.sln  # Visual Studio
```

---

## Continuous Integration

For automated builds (GitHub Actions, etc.):

```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel --verbose
cmake --install .
```

---

## Getting Help

**Build Issues?**
1. Check the log files in `build/`
2. Ensure all prerequisites are installed
3. Try `build.bat clean` followed by fresh build
4. Check CMake compatibility: `cmake --version`

**Still need help?**
- Review CMakeLists.txt for configuration
- Check build.bat or build.ps1 for implementation details
- Enable verbose output: `build.bat verbose` or `.\build.ps1 -Verbose`
