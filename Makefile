# Note that the unfork scaffold and the agent are built with two different toolchains.
# This is intentional; musl-gcc doesn't support C++, and host gcc cannot link a binary compatible
# with our address space requirements. Currently, the agent doesn't reference any libstdc++ symbols
# and so it doesn't link it; one could use ld -r and use STL containers.

CXXFLAGS = -std=c++11 -fno-strict-aliasing -fno-exceptions -Wall -Wextra -O3 -g
LDFLAGS  = -pthread -static -Wl,-melf_i386 -Wl,-Ttext-segment,0x00100000

tf2_healslut.elf: src/unfork.cc agent.o
	gcc -m32 -specs toolchain/lib/musl-gcc.specs $(CXXFLAGS) $(LDFLAGS) -o $@ $^

INCLUDES = $(addprefix -Ivendor/source-sdk/mp/src/, \
	mathlib common public public/tier0 public/tier1 tier1 game/shared game/client)
agent.o: src/agent.cc
	g++ -m32 $(INCLUDES) $(CXXFLAGS) -c -o $@ $^

.PHONY: clean
clean:
	rm -f tf2_healslut.elf agent.o

.PHONY: prepare
prepare:
	cd vendor/musl && \
		git clean -dxf && git reset --hard && \
		patch -p1 <../../musl-no-vdso.patch && \
		./configure --prefix=$(CURDIR)/toolchain \
			--enable-debug --disable-shared \
			--target=i386-linux-musl CC=gcc CFLAGS=-m32 AR=ar RANLIB=ranlib && \
		make && make install && \
		for dir in linux asm asm-generic; do \
			ln -s /usr/include/$$dir $(CURDIR)/toolchain/include/; \
		done
