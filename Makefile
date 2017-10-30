ifeq ($(OS),Windows_NT)
  ifneq ($(shell uname -o),Msys)
    RM := del /q
  else
    RM := rm -f
  endif
  EXE := .exe
  LIBS :=
else
  EXE :=
  LIBS := -ldl
endif

BIN=gelida${EXE}
SRC=${wildcard *.c}
OBJ=${SRC:.c=.o}
DEP=${SRC:.c=.d}

CFLAGS=-pipe -fvisibility=hidden -Wall -Wextra -Wno-implicit-fallthrough -DNDEBUG -O2
LDFLAGS=-s -lpthread ${LIBS}

.PHONY: all clean

all: ${BIN}

clean:
	@${RM} ${BIN} ${OBJ} ${DEP}

${BIN}: ${OBJ}
	@echo [L] $@
	@${CC} ${LDFLAGS} -o $@ $^

%_x86.o: %_x86.c
	@echo [C] $<
	@${CC} ${CFLAGS} -maes -mssse3 -MMD -c -o $@ $<

%.o: %.c
	@echo [C] $<
	@${CC} ${CFLAGS} -MMD -c -o $@ $<

-include ${DEP}
