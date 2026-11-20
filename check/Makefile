# check/Makefile -- build and run the isocline test jig.
#
# This is hand-written (no autoconf/automake: this fork isn't ours to
# carry that weight for). See build/plans/isocline.rst ("Testing"
# section) for the design this implements.
#
# Layout:
#   jig.c              the test harness, built via "#include <src/isocline.c>"
#   *.keys             pty/keystroke test scripts
#   *.direct           direct-call tests (no pty needed)
#   saved/*.out        golden output -- never written by "make test",
#                      only by "make accept", which is never run here
#                      automatically; it is always the user's call.
#   build/jig          the compiled test jig
#   build/out/*.out    fresh output from the last "make test" run
#
# Targets:
#   make              -- build build/jig
#   make test         -- build, run every *.keys/*.direct test, diff
#                         build/out/ against saved/, report a summary
#                         (does not stop at the first failure)
#   make accept       -- copy build/out/*.out over saved/*.out
#   make clean        -- remove build/

CC      = cc
CFLAGS  = -std=gnu11 -g -O0 -Wall -Wextra -Wformat=2 \
          -fsanitize=address -fno-omit-frame-pointer
LDFLAGS = -fsanitize=address

SED     = sed
# drop the diff header (---/+++ and the @@ hunk marker itself), leaving
# just the hunk body -- so a passing test (no diff) prints nothing
S2O     = | $(SED) '1,/@@/d'

BUILD   = build
OUT     = $(BUILD)/out
JIG     = $(BUILD)/jig

KEYS_TESTS   = $(wildcard *.keys)
DIRECT_TESTS = $(wildcard *.direct)
TESTS        = $(KEYS_TESTS) $(DIRECT_TESTS)
OUTS         = $(addprefix $(OUT)/,$(addsuffix .out,$(basename $(TESTS))))

# Per-test environment overrides, keyed by test basename (see
# bug-01-heap-overflow.keys: it needs a non-UTF-8 locale to reach the
# sbuf_strdup_from_utf8() heap overflow at all; tty.c treats the "C"
# locale itself as UTF-8-capable, so a real non-UTF-8 locale is used).
TEST_ENV                           = TERM=xterm-256color LC_ALL=en_US.UTF-8
ENV_bug-01-heap-overflow           = LC_ALL=en_US.US-ASCII

.PHONY: all test accept clean

all: $(JIG)

$(BUILD):
	mkdir -p $(BUILD)

$(OUT): $(BUILD)
	mkdir -p $(OUT)

$(JIG): jig.c ../src/isocline.c $(BUILD)
	$(CC) $(CFLAGS) -o $(JIG) jig.c $(LDFLAGS)

test: $(JIG) $(OUT)
	-@(for t in $(TESTS); do \
	    base=$$(basename $$t); base=$${base%.*}; \
	    echo "... $$base ..."; \
	    env $(TEST_ENV) $(ENV_$(base)) $(JIG) $$t > $(OUT)/$$base.out 2>&1; \
	    diff -Nu saved/$$base.out $(OUT)/$$base.out $(S2O); \
	    true; \
	  done)

accept:
	@mkdir -p saved
	@for t in $(TESTS); do \
	  base=$$(basename $$t); base=$${base%.*}; \
	  if [ -f $(OUT)/$$base.out ]; then \
	    cp $(OUT)/$$base.out saved/$$base.out; \
	    echo "accepted $$base"; \
	  fi; \
	done

clean:
	rm -rf $(BUILD)
