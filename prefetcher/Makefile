CC = gcc
CFLAGS = -Wall -Iinclude -pthread
LDFLAGS = -pthread

# 源文件目录
SRC_DIR = src
APP_DIR = app
INCLUDE_DIR = include

# 所有源文件（不包含test_app.c，避免main函数重复定义）
SRC_FILES = $(SRC_DIR)/inotify_wrapper.c $(SRC_DIR)/list.c $(SRC_DIR)/log_parser.c $(SRC_DIR)/prefetch.c $(SRC_DIR)/app_config.c $(SRC_DIR)/executor.c $(SRC_DIR)/event_loop.c $(SRC_DIR)/main.c

# 目标文件
MAIN_TARGET = prefetcher
APP_TARGET = $(APP_DIR)/test_app

# 目标
all: $(MAIN_TARGET)

# 编译主程序
$(MAIN_TARGET): $(SRC_FILES)
	$(CC) $(CFLAGS) -o $(MAIN_TARGET) $(SRC_FILES) $(LDFLAGS)



# 清理
clean:
	rm -f $(MAIN_TARGET)

# 运行
run:
	./$(MAIN_TARGET) --help

# 运行并指定应用程序 (示例：make run-app APP="/bin/ls" ARGS="-la")
run-app:
	./$(MAIN_TARGET) $(APP) $(ARGS)

# 运行Firefox浏览器（Linux环境）
# 自动检测常见的Firefox安装路径


# 显示帮助信息
help:
	./$(MAIN_TARGET) --help

# 运行测试脚本


# 测试修复后的编译和运行


.PHONY: all clean run run-app help