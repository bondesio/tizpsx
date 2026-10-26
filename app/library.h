#ifndef TIZPSX_LIBRARY_H
#define TIZPSX_LIBRARY_H

#define PSX_LIBRARY_MAX 2048
#define PSX_PATH_MAX 512
typedef struct { char path[PSX_PATH_MAX]; char name[128]; } psx_library_item;

void psx_library_scan(void);
int psx_library_count(void);
const psx_library_item *psx_library_get(int index);
int psx_library_is_bios_candidate(const psx_library_item *item);

#endif
