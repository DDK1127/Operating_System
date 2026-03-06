.PHONY: all clean

STUID := 314553040
TARGET := sched_demo_$(STUID)

CC := riscv64-linux-gnu-gcc
CFLAGS := -O2 -Wall -Wextra -pthread
LDFLAGS := -pthread -static

SRC := sched_demo_$(STUID).c
OBJ := $(SRC:.c=.o)
DEP := $(SRC:.c=.d)

all: $(TARGET)

-include $(DEP)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

clean:
	@rm -f $(TARGET) $(OBJ) $(DEP)
