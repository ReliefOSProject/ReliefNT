/* Run on a real controlling VGA VT, as an ordinary user. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <termios.h>
#include <unistd.h>

/** @brief Query real terminal size and cursor response, restoring input state. */
int main(void)
{
    struct winsize size;
    struct termios saved, raw;
    struct utsname system;
    char answer[32]={0};
    unsigned used=0;
    int query_ok=0;
    if (ioctl(0,TIOCGWINSZ,&size)<0 || tcgetattr(0,&saved)<0) { perror("terminal"); return 1; }
    raw=saved;
    raw.c_lflag &= ~(ICANON|ECHO);
    raw.c_cc[VMIN]=0;raw.c_cc[VTIME]=0;
    if (tcsetattr(0,TCSANOW,&raw)<0) return 1;
    static const char query[]="\033[r\033[?6l\033[7;13H\033[6n";
    if (write(1,query,sizeof(query)-1)==sizeof(query)-1) {
        struct pollfd fd={.fd=0,.events=POLLIN};
        for(unsigned tries=0;tries<10 && used+1<sizeof(answer);++tries) {
            int ret=poll(&fd,1,200);
            if(ret<0 && errno==EINTR)continue;
            if(ret<=0)break;
            ssize_t n=read(0,answer+used,sizeof(answer)-used-1);
            if(n>0)used+=(unsigned)n;
            if(used && answer[used-1]=='R')break;
        }
        query_ok=used==7 && !memcmp(answer,"\033[7;13R",7);
    }
    if (tcsetattr(0,TCSANOW,&saved)<0) return 1;
    uname(&system);
    printf("\033cReliefOS - VGA Text Mode\n\n");
    printf("Kernel: %s %s\n",system.sysname,system.release);
    printf("Terminal: %s   Size: %u columns x %u rows\n",ttyname(0),size.ws_col,size.ws_row);
    printf("User ID: %lu   ANSI cursor query: %s\n\n",(unsigned long)getuid(),query_ok?"PASS":"FAIL");
    puts("ANSI foreground colors:");
    for(unsigned i=0;i<16;++i)printf("\033[%um %02u ",i<8?30+i:90+i-8,i);
    printf("\033[0m\n\nANSI background colors:\n");
    for(unsigned i=0;i<16;++i)printf("\033[30;%um %02u ",i<8?40+i:100+i-8,i);
    printf("\033[0m\n\n");
    puts("Independent TTY1-TTY6 / scrolling / cursor / alternate screen");
    puts("VGA text console -> Xorg XDM -> IceWM -> text console");
    printf("\nVGA_NATIVE_PROBE_%s\n",query_ok && size.ws_col==80 && size.ws_row==25?"PASS":"FAIL");
    fflush(stdout);
    return query_ok && size.ws_col==80 && size.ws_row==25?0:1;
}
