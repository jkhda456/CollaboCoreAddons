# Builds `node` for the build machine (TARGET=host, for development and the
# native tests) or for the guest (TARGET=wasm).  build.sh drives it; the
# sources it compiles are prepared by build.sh's `src` stage in build/src.
#
#   make TARGET=host -j8          build/host/node
#   make TARGET=wasm -j8          build/wasm/node.wasm

TARGET ?= host
A := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ROOT := $(abspath $(A)/../..)
S := $(A)/build/src
Q := $(S)/quickjs
N := $(S)/node
B ?= $(A)/build/$(TARGET)
TOOLS := $(A)/build/tools

ifeq ($(TARGET),host)
CC := clang-19
CXX := clang++-19
AR := llvm-ar-19
OPT ?= -O2 -g
OPENSSL := $(A)/build/host-deps/openssl
OPENSSL_CFLAGS := -I$(OPENSSL)/include
OPENSSL_LIBS := $(OPENSSL)/lib/libssl.a $(OPENSSL)/lib/libcrypto.a
ZLIB_CFLAGS :=
ZLIB_LIBS := -lz
SYS_LIBS := -lm -lpthread -ldl -lstdc++
OUT := $(B)/bin/node
CXXRT_CFLAGS :=
else
CC := $(ROOT)/userspace/bin/wasm-cc
CXX := $(ROOT)/userspace/bin/wasm-cc
AR := llvm-ar-19
OPT ?= -O2
DEPS := $(ROOT)/python/deps
OPENSSL_CFLAGS := -I$(DEPS)/include
OPENSSL_LIBS := $(DEPS)/lib/libssl.a $(DEPS)/lib/libcrypto.a
ZLIB_CFLAGS := -I$(DEPS)/include
ZLIB_LIBS := $(DEPS)/lib/libz.a
LIBCXX ?= $(A)/build/libcxx-install
CXXRT_CFLAGS := -nostdinc++ -I$(LIBCXX)/include/c++/v1 -I$(LIBCXX)/include -fno-exceptions -fno-rtti
SYS_LIBS := $(LIBCXX)/lib/libc++.a $(LIBCXX)/lib/libc++abi.a -lm
STACK_SIZE ?= 16777216
WASM_LDFLAGS := -Wl,--import-memory -Wl,--max-memory=4294967296 -Wl,--shared-memory \
                -Wl,--export-table -Wl,-z,stack-size=$(STACK_SIZE)
OUT := $(B)/bin/node.wasm
endif

UV := $(N)/deps/uv
BROTLI := $(N)/deps/brotli/c
ZSTD := $(N)/deps/zstd/lib
ADA := $(N)/deps/ada
LLHTTP := $(N)/deps/llhttp
NGHTTP2 := $(N)/deps/nghttp2/lib
WASM3 := $(S)/wasm3/source

DEFS := -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -DUV_POSIX_SPAWN_ONLY \
        -DNGHTTP2_STATICLIB
INCS := -I$(A)/src -I$(Q) -I$(UV)/include -I$(UV)/src -I$(BROTLI)/include -I$(ZSTD) \
        -I$(ADA) -I$(LLHTTP)/include -I$(NGHTTP2)/includes -I$(WASM3) $(OPENSSL_CFLAGS) $(ZLIB_CFLAGS) \
        -iquote $(N)/src
CFLAGS := $(OPT) $(DEFS) $(INCS) -Wall -Wno-unused-function -Wno-sign-compare \
          -Wno-unused-variable -Wno-unused-but-set-variable -Wno-missing-braces \
          -Wno-deprecated-declarations
QJS_CFLAGS := $(OPT) -D_GNU_SOURCE -I$(Q) -Wno-everything
DEP_CFLAGS := $(OPT) $(DEFS) $(INCS) -Wno-everything

# ---- sources ----
QJS_SRC := $(Q)/quickjs.c $(Q)/libregexp.c $(Q)/libunicode.c $(Q)/dtoa.c
UV_SRC := $(addprefix $(UV)/src/,fs-poll.c idna.c inet.c random.c strscpy.c strtok.c \
            thread-common.c threadpool.c timer.c uv-common.c uv-data-getter-setters.c version.c) \
          $(addprefix $(UV)/src/unix/,async.c core.c dl.c fs.c getaddrinfo.c getnameinfo.c \
            loop-watcher.c loop.c pipe.c poll.c process.c random-devurandom.c signal.c stream.c \
            tcp.c thread.c tty.c udp.c proctitle.c linux.c procfs-exepath.c random-getrandom.c \
            random-sysctl-linux.c)
BROTLI_SRC := $(wildcard $(BROTLI)/common/*.c $(BROTLI)/dec/*.c $(BROTLI)/enc/*.c)
ZSTD_SRC := $(wildcard $(ZSTD)/common/*.c $(ZSTD)/compress/*.c $(ZSTD)/decompress/*.c)
LLHTTP_SRC := $(wildcard $(LLHTTP)/src/*.c)
NGHTTP2_SRC := $(wildcard $(NGHTTP2)/*.c)
ADA_SRC := $(ADA)/ada.cpp
WASM3_SRC := $(wildcard $(WASM3)/*.c)
NODE_SRC_EXCLUDE ?=
NODE_SRC := $(filter-out $(addprefix $(A)/src/,$(NODE_SRC_EXCLUDE)),$(wildcard $(A)/src/*.c))

GEN := $(B)/gen
BUILTINS_C := $(GEN)/builtins_data.c
DATA_C := $(GEN)/data.c

objs = $(patsubst $(1)/%,$(B)/$(2)/%.o,$(3))
QJS_OBJ := $(call objs,$(Q),qjs,$(QJS_SRC))
UV_OBJ := $(call objs,$(UV),uv,$(UV_SRC))
BROTLI_OBJ := $(call objs,$(BROTLI),brotli,$(BROTLI_SRC))
ZSTD_OBJ := $(call objs,$(ZSTD),zstd,$(ZSTD_SRC))
LLHTTP_OBJ := $(call objs,$(LLHTTP),llhttp,$(LLHTTP_SRC))
NGHTTP2_OBJ := $(call objs,$(NGHTTP2),nghttp2,$(NGHTTP2_SRC))
ADA_OBJ := $(call objs,$(ADA),ada,$(ADA_SRC))
WASM3_OBJ := $(call objs,$(WASM3),wasm3,$(WASM3_SRC))
NODE_OBJ := $(call objs,$(A)/src,node,$(NODE_SRC))
GEN_OBJ := $(B)/gen/builtins_data.c.o $(B)/gen/data.c.o

ALL_OBJ := $(NODE_OBJ) $(GEN_OBJ) $(QJS_OBJ) $(UV_OBJ) $(BROTLI_OBJ) $(ZSTD_OBJ) $(LLHTTP_OBJ) $(NGHTTP2_OBJ) $(ADA_OBJ) $(WASM3_OBJ)

all: $(OUT)

$(OUT): $(ALL_OBJ)
	@mkdir -p $(dir $@)
	@echo "  LINK $(notdir $@)"
ifeq ($(TARGET),host)
	@$(CXX) -o $@ $(ALL_OBJ) $(OPENSSL_LIBS) $(ZLIB_LIBS) $(SYS_LIBS)
else
	@$(CC) -o $@ $(ALL_OBJ) $(OPENSSL_LIBS) $(ZLIB_LIBS) $(SYS_LIBS) $(WASM_LDFLAGS)
endif

$(B)/qjs/%.o: $(Q)/%
	@mkdir -p $(dir $@)
	@echo "  CC $(notdir $<)"
	@$(CC) $(QJS_CFLAGS) -c $< -o $@

$(B)/uv/%.o: $(UV)/%
	@mkdir -p $(dir $@)
	@echo "  CC $(notdir $<)"
	@$(CC) $(DEP_CFLAGS) -c $< -o $@

$(B)/brotli/%.o: $(BROTLI)/%
	@mkdir -p $(dir $@)
	@echo "  CC $(notdir $<)"
	@$(CC) $(DEP_CFLAGS) -c $< -o $@

$(B)/zstd/%.o: $(ZSTD)/%
	@mkdir -p $(dir $@)
	@echo "  CC $(notdir $<)"
	@$(CC) $(DEP_CFLAGS) -DZSTD_MULTITHREAD=0 -DZSTD_DISABLE_ASM -c $< -o $@

$(B)/nghttp2/%.o: $(NGHTTP2)/%
	@mkdir -p $(dir $@)
	@echo "  CC $(notdir $<)"
	@$(CC) $(DEP_CFLAGS) -DHAVE_CONFIG_H -DBUILDING_NGHTTP2 '-D_U_=' -c $< -o $@

$(B)/llhttp/%.o: $(LLHTTP)/%
	@mkdir -p $(dir $@)
	@echo "  CC $(notdir $<)"
	@$(CC) $(DEP_CFLAGS) -c $< -o $@

$(B)/wasm3/%.o: $(WASM3)/%
	@mkdir -p $(dir $@)
	@echo "  CC $(notdir $<)"
	@$(CC) $(DEP_CFLAGS) -Dd_m3HasWASI=0 -c $< -o $@

$(B)/ada/%.o: $(ADA)/%
	@mkdir -p $(dir $@)
	@echo "  CXX $(notdir $<)"
	@$(CXX) $(DEP_CFLAGS) $(CXXRT_CFLAGS) -std=c++20 -DADA_INCLUDE_URL_PATTERN=0 -c $< -o $@

$(B)/node/%.o: $(A)/src/% $(A)/src/node.h $(A)/src/streams.h $(Q)/quickjs.h
	@mkdir -p $(dir $@)
	@echo "  CC $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

# intl.c embeds these (#embed / #include)
$(B)/node/intl.c.o: $(A)/src/intl.js $(A)/src/intl_data.json $(A)/src/unicode_tables.inc

$(B)/gen/%.o: $(GEN)/%
	@echo "  CC $(notdir $<)"
	@$(CC) $(OPT) -c $< -o $@

# ---- generated ----
$(TOOLS)/js2bc: $(A)/tools/js2bc.c $(QJS_SRC) $(Q)/quickjs.h
	@mkdir -p $(dir $@)
	clang-19 -O2 -D_GNU_SOURCE -I$(Q) -o $@ $(A)/tools/js2bc.c $(QJS_SRC) -lm -lpthread

$(GEN)/manifest.txt: $(A)/tools/gen-manifest.py $(shell find $(A)/lib -name '*.js' 2>/dev/null) $(N)/.stamp
	@mkdir -p $(GEN)
	python3 $(A)/tools/gen-manifest.py $(N) $(S)/extra $(A)/lib > $@

$(BUILTINS_C): $(TOOLS)/js2bc $(GEN)/manifest.txt $(shell cut -f2 $(GEN)/manifest.txt 2>/dev/null)
	$(TOOLS)/js2bc $@ $(GEN)/manifest.txt

$(DATA_C): $(A)/tools/gen-data.py $(A)/src/options.json $(A)/src/config.gypi.json \
          $(A)/src/http2_constants.json
	@mkdir -p $(GEN)
	python3 $(A)/tools/gen-data.py $@ node_options_json=$(A)/src/options.json \
	  node_config_gypi_json=$(A)/src/config.gypi.json \
	  node_http2_constants_json=$(A)/src/http2_constants.json

$(N)/.stamp:
	@test -d $(N)/lib || { echo "run build.sh's src stage first"; exit 1; }
	touch $@

clean:
	rm -rf $(B)

.PHONY: all clean
