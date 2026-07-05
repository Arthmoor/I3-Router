CC = g++

# These protect against stack smashing and buffer overflows in production.
# We add -D_FORTIFY_SOURCE=3 to perform runtime checks on string/memory functions.
HARDENING_FLAGS = -fstack-protector-strong -D_FORTIFY_SOURCE=3

W_FLAGS = -Wall -Wshadow -pedantic -Wmissing-format-attribute -Wpointer-arith -Wcast-align -Wredundant-decls -Wswitch-default -Wuseless-cast -Wwrite-strings -Wformat=2 -Wformat-security -Wformat-signedness -Winline -Wcast-qual -Werror

C_FLAGS= -std=c++23 $(W_FLAGS) $(HARDENING_FLAGS) -g2 -O3
L_FLAGS= -g2 -O3

C_FILES = i3router.cpp
O_FILES := $(patsubst %.cpp,o/%.o,$(C_FILES))

all:
	make -s i3router

i3router: $(O_FILES)
	rm -f i3router
	$(CC) -o i3router $(O_FILES) $(L_FLAGS)
	echo "Done compiling i3router.";
	chmod u+x i3router

clean:
	rm -f o/*.o i3router *~
	make all

purge:
	rm -f o/*.o i3router *~

o/%.o: %.cpp
	echo "  Compiling $@....";
	$(CC) -c $(C_FLAGS) $(L_FLAGS) $< -o $@
