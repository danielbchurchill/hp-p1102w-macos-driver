CC      ?= clang
ARCHS   := -arch arm64 -arch x86_64
CFLAGS  := -O2 -mmacosx-version-min=12.0
V       := vendor/foo2zjs
B       := build
PPD     := ppd/HP-LaserJet-P1102w-ZJS.ppd

all: $(B)/foo2zjs $(B)/rastertozjs $(B)/zjsdecode

$(B):
	mkdir -p $(B)

$(B)/foo2zjs: $(V)/foo2zjs.c $(V)/jbig.c $(V)/jbig_ar.c | $(B)
	$(CC) $(CFLAGS) $(ARCHS) -w -o $@ $^

$(B)/zjsdecode: $(V)/zjsdecode.c $(V)/jbig.c $(V)/jbig_ar.c | $(B)
	$(CC) $(CFLAGS) -w -o $@ $^

$(B)/rastertozjs: src/rastertozjs.c | $(B)
	$(CC) $(CFLAGS) $(ARCHS) -Wall -Wextra -Wno-deprecated-declarations -o $@ $< -lcups

# Offline pipeline test: PDF -> cgpdftoraster -> rastertozjs -> foo2zjs.
# Nothing is sent to the printer.
test: all
	python3 -I tools/make_testpage.py $(B)/testpage.pdf letter
	PPD=$(PPD) /usr/libexec/cups/filter/cgpdftoraster 1 test test 1 "" $(B)/testpage.pdf > $(B)/testpage.ras 2>/dev/null
	PPD=$(PPD) FOO2ZJS=$(CURDIR)/$(B)/foo2zjs $(B)/rastertozjs 1 test test 1 "" $(B)/testpage.ras > $(B)/testpage.zjs
	@$(B)/zjsdecode < $(B)/testpage.zjs > $(B)/testpage.decoded 2>&1
	@grep -q ZJT_START_PAGE $(B)/testpage.decoded || { echo "FAIL: no page in stream"; exit 1; }
	@grep -q ZJT_JBIG_BID   $(B)/testpage.decoded || { echo "FAIL: no raster data"; exit 1; }
	@grep -q "MX = 0,"      $(B)/testpage.decoded || { echo "FAIL: JBIG MX must be 0 for P1102"; exit 1; }
	@! grep -q START_PLANE  $(B)/testpage.decoded || { echo "FAIL: START_PLANE present (need -P)"; exit 1; }
	@grep -q "@PJL EOJ"     $(B)/testpage.decoded || { echo "FAIL: job not terminated"; exit 1; }
	@echo "PASS: $$(wc -c < $(B)/testpage.zjs | tr -d ' ') bytes of valid ZjStream for 1 page"

clean:
	rm -rf $(B)

.PHONY: all test clean
