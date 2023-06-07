# This is intended for the container, update this to your build of libabigail
LIBABIGAIL_SRC ?= /code

# This assumes you have the following libraries on your system
LDLIBS := -labigail -lbpf -ldw -lelf -licuuc -lxml2 -lz
LDFLAGS := -L$(LIBABIGAIL_SRC)/build/src/.libs
CXXFLAGS := -I$(LIBABIGAIL_SRC)/include -I$(LIBABIGAIL_SRC)/src -I$(LIBABIGAIL_SRC)/build -I/usr/include -I/usr/include/libxml2 -Wall -Wextra

SRCS := abigail_reader.cc btf_reader.cc reporting.cc stg.cc stgdiff.cc
OBJS := $(SRCS:.cc=.o)
MAIN := stgdiff

.PHONY: all

all: $(MAIN)

# Conservative header dependencies
$(OBJS): abigail_reader.h btf_reader.h crc.h error.h id.h order.h reporting.h scc.h stg.h

$(MAIN): $(OBJS)
	$(LINK.cc) $^ $(LDLIBS) -o $@

clean:
	rm -f $(OBJS) $(MAIN)
