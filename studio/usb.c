/* Native Mote device link — see usb.h. Linux (termios) + Windows (Win32 COM). */
#include "usb.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MVID 0xCAFE
#define MPID 0x4D01

#ifdef _WIN32
  #define INITGUID
  #include <windows.h>
  #include <initguid.h>
  #include <devguid.h>
  #include <setupapi.h>
  typedef HANDLE shandle;
  #define SBAD INVALID_HANDLE_VALUE
  static long now_ms(void){ return (long)GetTickCount(); }
  static int find_port(char *out,int n){ HDEVINFO di=SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS,0,0,DIGCF_PRESENT);
      if(di==INVALID_HANDLE_VALUE)return 0; SP_DEVINFO_DATA dd; dd.cbSize=sizeof dd; int found=0; char want[32];
      snprintf(want,sizeof want,"VID_%04X&PID_%04X",MVID,MPID);
      for(DWORD i=0;SetupDiEnumDeviceInfo(di,i,&dd)&&!found;i++){ char hw[256]={0};
          if(SetupDiGetDeviceRegistryPropertyA(di,&dd,SPDRP_HARDWAREID,0,(BYTE*)hw,sizeof hw,0)&&strstr(hw,want)){
              HKEY k=SetupDiOpenDevRegKey(di,&dd,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_READ);
              if(k!=INVALID_HANDLE_VALUE){ char pn[16]={0}; DWORD sz=sizeof pn,ty=0;
                  if(RegQueryValueExA(k,"PortName",0,&ty,(BYTE*)pn,&sz)==ERROR_SUCCESS){ snprintf(out,n,"\\\\.\\%s",pn); found=1; }
                  RegCloseKey(k); } } }
      SetupDiDestroyDeviceInfoList(di); return found; }
  static shandle ser_open(void){ char port[32]; if(!find_port(port,sizeof port))return SBAD;
      HANDLE h=CreateFileA(port,GENERIC_READ|GENERIC_WRITE,0,0,OPEN_EXISTING,0,0); if(h==INVALID_HANDLE_VALUE)return SBAD;
      DCB dcb; memset(&dcb,0,sizeof dcb); dcb.DCBlength=sizeof dcb; GetCommState(h,&dcb);
      dcb.BaudRate=115200; dcb.ByteSize=8; dcb.Parity=NOPARITY; dcb.StopBits=ONESTOPBIT; SetCommState(h,&dcb);
      COMMTIMEOUTS to; memset(&to,0,sizeof to); to.ReadIntervalTimeout=MAXDWORD; to.ReadTotalTimeoutConstant=30; SetCommTimeouts(h,&to);
      PurgeComm(h,PURGE_RXCLEAR|PURGE_TXCLEAR); return h; }
  static int ser_write(shandle h,const void*b,int n){ DWORD w=0; return WriteFile(h,b,n,&w,0)?(int)w:0; }
  static int ser_read(shandle h,void*b,int n){ DWORD r=0; return ReadFile(h,b,n,&r,0)?(int)r:0; }
  static void ser_close(shandle h){ if(h!=SBAD)CloseHandle(h); }
#else
  #include <fcntl.h>
  #include <unistd.h>
  #include <termios.h>
  #include <dirent.h>
  #include <time.h>
  typedef int shandle;
  #define SBAD (-1)
  static long now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000+t.tv_nsec/1000000; }
  static int find_port(char *out,int n){ DIR *d=opendir("/sys/class/tty"); if(!d)return 0; struct dirent *e; int found=0;
      /* idVendor/idProduct live on the USB *device* (device/..), not the hub above it.
       * Walk a few levels up to be robust across sysfs layouts. */
      const char *ups[]={ "device/..", "device/../..", "device/../../.." };
      while((e=readdir(d))&&!found){ if(strncmp(e->d_name,"ttyACM",6)&&strncmp(e->d_name,"ttyUSB",6))continue;
          for(int u=0;u<3&&!found;u++){ char p[360]; unsigned vid=0,pid=0; FILE *f;
              snprintf(p,sizeof p,"/sys/class/tty/%s/%s/idVendor",e->d_name,ups[u]); if(!(f=fopen(p,"r")))continue; if(fscanf(f,"%x",&vid)!=1)vid=0; fclose(f);
              snprintf(p,sizeof p,"/sys/class/tty/%s/%s/idProduct",e->d_name,ups[u]); if((f=fopen(p,"r"))){ if(fscanf(f,"%x",&pid)!=1)pid=0; fclose(f); }
              if(vid==MVID&&pid==MPID){ snprintf(out,n,"/dev/%.50s",e->d_name); found=1; } } }
      closedir(d); return found; }
  static shandle ser_open(void){ char port[64]; const char*ov=getenv("MOTE_DEV_PORT");   /* test override: a pty/tty path */
      if(ov&&ov[0]) snprintf(port,sizeof port,"%.63s",ov); else if(!find_port(port,sizeof port))return SBAD;
      int fd=open(port,O_RDWR|O_NOCTTY); if(fd<0)return SBAD;
      struct termios t; if(tcgetattr(fd,&t)){ close(fd); return SBAD; } cfmakeraw(&t);
      cfsetispeed(&t,B115200); cfsetospeed(&t,B115200); t.c_cc[VMIN]=0; t.c_cc[VTIME]=1;   /* 0.1s read timeout */
      tcsetattr(fd,TCSANOW,&t); tcflush(fd,TCIOFLUSH); return fd; }
  static int ser_write(shandle h,const void*b,int n){ int r=(int)write(h,b,n); return r<0?0:r; }
  static int ser_read(shandle h,void*b,int n){ int r=(int)read(h,b,n); return r<0?0:r; }
  static void ser_close(shandle h){ if(h!=SBAD)close(h); }
#endif

/* read one '\n'-terminated line (CR stripped) with a millisecond timeout */
static int ser_readline(shandle h,char *out,int n,int tmo){ int len=0; long dl=now_ms()+tmo;
    while(now_ms()<dl){ char c; int r=ser_read(h,&c,1);
        if(r==1){ if(c=='\n'){ out[len]=0; return len; } if(c!='\r'&&len<n-1)out[len++]=c; } }
    out[len]=0; return len?len:-1; }

int mote_dev_present(void){ shandle h=ser_open(); if(h==SBAD)return 0; ser_close(h); return 1; }

int mote_dev_ping(mote_log_fn log){ shandle h=ser_open(); if(h==SBAD){ log("no Mote device found (CAFE:4D01)"); return -1; }
    ser_write(h,"PING\n",5); char r[128]; ser_readline(h,r,sizeof r,1500); ser_close(h);
    if(!strncmp(r,"MOTE ",5)){ char m[160]; snprintf(m,sizeof m,"device OK  —  %s",r); log(m); return 0; }
    log("no/odd reply from device"); return -1; }

int mote_dev_list(mote_log_fn log){ shandle h=ser_open(); if(h==SBAD){ log("no Mote device found"); return -1; }
    ser_write(h,"LIST\n",5); char r[128]; int any=0;
    for(;;){ int l=ser_readline(h,r,sizeof r,1500); if(l<=0||!strcmp(r,"OK"))break; if(!strncmp(r,"ERR",3)){ log(r); break; } log(r); any++; }
    ser_close(h); if(!any)log("(no games installed)"); return 0; }

int mote_dev_push(const char *path,const char *name,int launch,mote_log_fn log){
    FILE *f=fopen(path,"rb"); if(!f){ log("push: built .mote not found (build first)"); return -1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET); unsigned char *data=malloc(sz?sz:1);
    if(fread(data,1,sz,f)!=(size_t)sz){} fclose(f);
    shandle h=ser_open(); if(h==SBAD){ log("no Mote device found"); free(data); return -1; }
    char cmd[160],r[80]; snprintf(cmd,sizeof cmd,"PUT %s %ld\n",name,sz); ser_write(h,cmd,(int)strlen(cmd));
    ser_readline(h,r,sizeof r,3000); if(strcmp(r,"READY")){ log("device did not accept PUT"); ser_close(h); free(data); return -1; }
    { char m[112]; snprintf(m,sizeof m,"pushing %s (%ld KB) — throttled by the device as it writes flash...",name,(sz+1023)/1024); log(m); }
    /* Stream a percentage line every ~10% so a long push visibly progresses and
     * the user doesn't think the IDE hung. The device flow-controls these writes
     * while it commits to flash, so `off` advancing is real progress. */
    long off=0; int last_step=-1;
    while(off<sz){ int ch=sz-off>4096?4096:(int)(sz-off); int w=ser_write(h,data+off,ch); if(w<=0)break; off+=w;
        int step=(int)(off*10/sz);
        if(step!=last_step){ last_step=step; char m[80]; snprintf(m,sizeof m,"  ... %d%%  (%ld/%ld KB)",step*10,off/1024,(sz+1023)/1024); log(m); } }
    log("  transfer sent — waiting for the device to finish writing flash...");
    int tmo=20000; if(sz/8>tmo)tmo=(int)(sz/8); ser_readline(h,r,sizeof r,tmo);
    if(strcmp(r,"OK")){ char m[96]; snprintf(m,sizeof m,"push failed: %s",r); log(m); ser_close(h); free(data); return -1; }
    { char m[96]; snprintf(m,sizeof m,"pushed %s  (%ld bytes)",name,sz); log(m); }
    if(launch){ snprintf(cmd,sizeof cmd,"LAUNCH %s\n",name); ser_write(h,cmd,(int)strlen(cmd)); ser_readline(h,r,sizeof r,3000);
        char m[112]; snprintf(m,sizeof m,"launch: %s",r); log(m); }
    ser_close(h); free(data); return 0; }

int mote_dev_wipe(mote_log_fn log){ shandle h=ser_open(); if(h==SBAD){ log("no Mote device found"); return -1; }
    ser_write(h,"WIPE\n",5); char r[80]; ser_readline(h,r,sizeof r,3000); ser_close(h);
    char m[96]; snprintf(m,sizeof m,"wipe: %s",r); log(m); return 0; }

int mote_dev_logs(int seconds,mote_log_fn log,volatile int *stop){ shandle h=ser_open(); if(h==SBAD){ log("no Mote device found"); return -1; }
    long dl=seconds>0?now_ms()+seconds*1000:0; char line[256];
    while((!dl||now_ms()<dl)&&(!stop||!*stop)){ int l=ser_readline(h,line,sizeof line,400); if(l>0)log(line); }
    ser_close(h); return 0; }

/* --- raw persistent pipe (the LAN link bridge): the port stays open and bytes
 * pass through untouched — the device's 2P link owns its end of the CDC pipe,
 * the Studio relays this end to the network peer. read blocks <=~100 ms. */
struct mote_dev_raw { shandle h; };
void *mote_dev_open_raw(void){ shandle h=ser_open(); if(h==SBAD)return 0;
    static struct mote_dev_raw r; r.h=h; return &r; }
int  mote_dev_raw_read (void *p, void *b, int n){ return ser_read (((struct mote_dev_raw*)p)->h, b, n); }
int  mote_dev_raw_write(void *p, const void *b, int n){ return ser_write(((struct mote_dev_raw*)p)->h, b, n); }
void mote_dev_close_raw(void *p){ ser_close(((struct mote_dev_raw*)p)->h); }
