#define PSP_O_RDONLY 1
#define PSP_O_WRONLY 2
#define PSP_O_CREAT 4
#define PSP_O_APPEND 8
#define PSP_O_TRUNC 16
#define PSP_SEEK_END 2
#define PSP_SEEK_SET 0
int sceIoOpen(const char *,int,int);
int sceIoClose(int);
int sceIoLseek32(int,int,int);
int sceIoRead(int,void *,unsigned);
int sceIoWrite(int,const void *,unsigned);
