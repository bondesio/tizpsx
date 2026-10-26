TIZEN := $(HOME)/tizen-studio/tools/ide/bin/tizen
PACKAGE := TIZPSX0001-1.0.0.wgt
.PHONY: all clean
all: out/$(PACKAGE)
out/$(PACKAGE): config.xml index.html icon.png
	mkdir -p out
	rm -f $@
	zip -q $@ config.xml index.html icon.png
	$(TIZEN) package -t wgt -s tvdev -- $@
clean:
	rm -rf out
