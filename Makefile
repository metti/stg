LDLIBS := -lelf -lxml2
LDFLAGS :=
CXXFLAGS := -fstrict-enums -Wall -Wextra -I/usr/include -I/usr/include/libxml2 -std=c++17

SRCS := abigail_reader.cc btf_reader.cc elf_loader.cc elf_reader.cc \
	post_processing.cc reporting.cc stg.cc stgdiff.cc
HDRS := abigail_reader.h btf_reader.h crc.h elf_loader.h elf_reader.h \
	error.h id.h order.h post_processing.h reporting.h scc.h stg.h

OBJS := $(SRCS:.cc=.o)
MAIN := stgdiff

.PHONY: all

all: $(MAIN)

# Conservative header dependencies
$(OBJS): $(HDRS)

$(MAIN): $(OBJS)
	$(LINK.cc) $^ $(LDLIBS) -o $@

clean:
	rm -f $(OBJS) $(MAIN)
