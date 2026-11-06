# =============================================================================
# 文章查重系统构建脚本（Windows + MinGW-w64）
#   构建：mingw32-make          或  mingw32-make -j4
#   清理：mingw32-make clean
# 依赖：g++（C++17）与系统库 ws2_32（WinSock2），不依赖任何第三方库。
# =============================================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Isrc
LDLIBS   ?= -lws2_32

BUILD  := build
TARGET := $(BUILD)/pc_check.exe

SOURCES := $(wildcard src/*.cpp)
OBJECTS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SOURCES))
HEADERS := $(wildcard src/*.h)

.PHONY: all clean

all: $(TARGET)

# 链接主程序：单文件可执行，内置 HTTP 服务与前端静态资源目录
$(TARGET): $(OBJECTS) $(BUILD)/.gitkeep
	$(CXX) $(OBJECTS) -o $@ $(LDLIBS)

# 逐文件编译，头文件变化时自动重编
$(BUILD)/%.o: src/%.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	-$(RM) $(OBJECTS) $(TARGET)
