/* Bounded, user-file-only PSX image index for internal app data and mounted
 * Tizen storage roots. No image contents are read or modified by the scanner. */
#include "library.h"

extern int open(const char *, int, ...);
extern int close(int);
extern long syscall(long, ...);
extern int snprintf(char *, unsigned long, const char *, ...);
extern unsigned long strlen(const char *);
extern int strcasecmp(const char *, const char *);
extern int storage_foreach_device_supported(unsigned char (*)(int,int,int,const char *,void *), void *);

#ifndef TIZPSX_DATA_DIR
#define TIZPSX_DATA_DIR "/opt/usr/home/owner/apps_rw/org.pablo.tizpsx/data"
#endif
#define O_RDONLY 0
#define O_DIRECTORY 0x4000
#define O_NOFOLLOW 0x8000
#define O_NONBLOCK 0x800
#define SYS_getdents64 217
#define DT_DIR 4
#define DT_REG 8
#define DT_UNKNOWN 0
#define DEPTH_MAX 7
#define ENTRY_MAX 100000
#define ROOT_MAX 16

typedef struct { char path[PSX_PATH_MAX]; } root_t;
static root_t roots[ROOT_MAX];
static int root_count, item_count, entry_count;
static psx_library_item items[PSX_LIBRARY_MAX];

static void copy(char *d, const char *s, int cap) { int i=0; if(cap<=0)return; while(s[i] && i<cap-1){d[i]=s[i];i++;} d[i]=0; }
static int ends(const char *name, const char *ext) {
    unsigned long a=strlen(name), b=strlen(ext);
    return a>b && name[a-b-1]=='.' && strcasecmp(name+a-b,ext)==0;
}
static int is_image(const char *s) {
    return ends(s,"cue") || ends(s,"bin") || ends(s,"iso") || ends(s,"img") || ends(s,"chd") || ends(s,"pbp") || ends(s,"m3u") || ends(s,"ccd") || ends(s,"mdf");
}
static int item_before(const psx_library_item *a,const psx_library_item *b) {
    int c=strcasecmp(a->name,b->name); return c<0 || (c==0 && strcasecmp(a->path,b->path)<0);
}
static unsigned char storage_cb(int id,int type,int state,const char *path,void *data) {
    (void)id;(void)data;
    if((type==1 || type==2) && state>=0 && path && path[0] && root_count<ROOT_MAX) {
        int i; for(i=0;i<root_count;i++) if(!strcasecmp(roots[i].path,path)) break;
        if(i==root_count) copy(roots[root_count++].path,path,PSX_PATH_MAX);
    }
    return 1;
}
static void scan_dir(const char *path,int depth) {
    if(depth>DEPTH_MAX || item_count>=PSX_LIBRARY_MAX || entry_count>=ENTRY_MAX) return;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NONBLOCK); if(fd<0)return;
    char buf[4096];
    for(;;) {
        long n=syscall(SYS_getdents64,fd,buf,sizeof buf); if(n<=0)break;
        for(long off=0;off<n;) {
            unsigned short reclen=*(unsigned short *)(buf+off+16);
            unsigned char type=*(unsigned char *)(buf+off+18);
            const char *name=buf+off+19;
            if(!reclen) { off=n; break; }
            off+=reclen; if(++entry_count>ENTRY_MAX || name[0]=='.')continue;
            if(type!=DT_DIR && type!=DT_REG && type!=DT_UNKNOWN)continue;
            char child[PSX_PATH_MAX]; int len=snprintf(child,sizeof child,"%s/%s",path,name);
            if(len<=0 || len>=(int)sizeof child)continue;
            if(type!=DT_DIR && is_image(name) && item_count<PSX_LIBRARY_MAX) {
                psx_library_item *it=&items[item_count++]; copy(it->path,child,sizeof it->path); copy(it->name,name,sizeof it->name);
            } else if((type==DT_DIR || type==DT_UNKNOWN) && depth<DEPTH_MAX) scan_dir(child,depth+1);
            if(item_count>=PSX_LIBRARY_MAX || entry_count>=ENTRY_MAX)break;
        }
        if(item_count>=PSX_LIBRARY_MAX || entry_count>=ENTRY_MAX)break;
    }
    close(fd);
}
void psx_library_scan(void) {
    root_count=0; item_count=0; entry_count=0;
    copy(roots[root_count++].path,TIZPSX_DATA_DIR,PSX_PATH_MAX);
    storage_foreach_device_supported(storage_cb,0);
    for(int i=0;i<root_count;i++) scan_dir(roots[i].path,0);
    for(int i=1;i<item_count;i++) { psx_library_item t=items[i]; int j=i; while(j>0 && item_before(&t,&items[j-1])) { items[j]=items[j-1]; j--; } items[j]=t; }
}
int psx_library_count(void){return item_count;}
const psx_library_item *psx_library_get(int i){return i>=0&&i<item_count?&items[i]:0;}
int psx_library_is_bios_candidate(const psx_library_item *it){return it && ends(it->name,"bin");}
