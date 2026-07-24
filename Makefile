# Compiler
CXX = clang++
CC = clang

# Include paths:
#   modules/            - our own code, included as "ModuleName/ModuleName.h"
#   external/imgui(...)  - Dear ImGui (docking branch) + its GLFW/OpenGL3 backend
#   external/miniz        - zip reading (used to parse .xlsx files)
#   external/tinyfiledialogs - native Open/Save file dialogs
#   external/stb           - image loading/writing (stb_image, stb_image_write)
#   external/json           - nlohmann::json (single header)
INCLUDES = -Imodules \
           -Iexternal/imgui -Iexternal/imgui/backends \
           -Iexternal/miniz \
           -Iexternal/tinyfiledialogs \
           -Iexternal/stb \
           -Iexternal/json \
           -I/opt/homebrew/include

CXXFLAGS = -std=c++17 -Wall -Wextra $(INCLUDES)
CFLAGS = $(INCLUDES)

# Linker flags
LDFLAGS = -L/opt/homebrew/lib -lglfw \
          -framework OpenGL \
          -framework Cocoa \
          -framework IOKit \
          -framework CoreVideo

# C++ source files
SRC = main.cpp \
      modules/ImageLoader/ImageLoader.cpp \
      modules/HtmlExporter/HtmlExporter.cpp \
      modules/ProjectIO/ProjectIO.cpp \
      modules/ImportIO/ImportIO.cpp \
      external/imgui/imgui.cpp \
      external/imgui/imgui_demo.cpp \
      external/imgui/imgui_draw.cpp \
      external/imgui/imgui_tables.cpp \
      external/imgui/imgui_widgets.cpp \
      external/imgui/backends/imgui_impl_glfw.cpp \
      external/imgui/backends/imgui_impl_opengl3.cpp

# C source files (tinyfiledialogs and miniz are plain C - must NOT be
# compiled with clang++, which would treat them as C++; they get their own
# objects built with the plain C compiler, then linked in below)
C_SRC = external/tinyfiledialogs/tinyfiledialogs.c \
        external/miniz/miniz.c \
        external/miniz/miniz_tdef.c \
        external/miniz/miniz_tinfl.c \
        external/miniz/miniz_zip.c
C_OBJ = external/tinyfiledialogs/tinyfiledialogs.o \
        external/miniz/miniz.o \
        external/miniz/miniz_tdef.o \
        external/miniz/miniz_tinfl.o \
        external/miniz/miniz_zip.o

# Output binary
TARGET = app

# Default rule
all: $(TARGET)

external/tinyfiledialogs/tinyfiledialogs.o: external/tinyfiledialogs/tinyfiledialogs.c
	$(CC) $(CFLAGS) -c external/tinyfiledialogs/tinyfiledialogs.c -o external/tinyfiledialogs/tinyfiledialogs.o

external/miniz/miniz.o: external/miniz/miniz.c
	$(CC) $(CFLAGS) -c external/miniz/miniz.c -o external/miniz/miniz.o

external/miniz/miniz_tdef.o: external/miniz/miniz_tdef.c
	$(CC) $(CFLAGS) -c external/miniz/miniz_tdef.c -o external/miniz/miniz_tdef.o

external/miniz/miniz_tinfl.o: external/miniz/miniz_tinfl.c
	$(CC) $(CFLAGS) -c external/miniz/miniz_tinfl.c -o external/miniz/miniz_tinfl.o

external/miniz/miniz_zip.o: external/miniz/miniz_zip.c
	$(CC) $(CFLAGS) -c external/miniz/miniz_zip.c -o external/miniz/miniz_zip.o

$(TARGET): $(C_OBJ)
	$(CXX) $(CXXFLAGS) $(SRC) $(C_OBJ) $(LDFLAGS) -o $(TARGET)

# Clean
clean:
	rm -f $(TARGET) $(C_OBJ)
	clear

# Again
again:
	make clean
	make
