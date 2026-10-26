# Host harness: ../app/ui/psx_main.c compiled unchanged against fake EFL/input/library/core.
CC ?= gcc
.PHONY: ui-tests clean
ui-tests: build/ui_test
	./build/ui_test
build/ui_test: ui_test.c ../app/ui/psx_main.c ../app/ui/video_convert.h ../app/perf_overlay.h fake_efl.h
	mkdir -p build
	$(CC) -w -std=gnu11 -O1 -g -pthread -DTIZPSX_DATA_DIR='"out/data"' -Dmain=tizpsx_app_main -c ../app/ui/psx_main.c -o build/psx_main.o
	$(CC) -w -std=gnu11 -O1 -g -pthread -c ui_test.c -o build/ui_test.o
	$(CC) -pthread -o $@ build/ui_test.o build/psx_main.o
clean:
	rm -rf build out
